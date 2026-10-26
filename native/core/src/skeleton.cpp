#include "bg3vr/skeleton.hpp"

#include "bg3vr/bridge.hpp"

#include "bg3vr/bodyik.hpp"
#include "bg3vr/camera.hpp"
#include "bg3vr/gameui.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/signatures.hpp"

#include <safetyhook.hpp>
#include <windows.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <cctype>
#include <format>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace bg3vr::skeleton {

namespace {

ModuleInfo g_module;

// Requests from the bridge thread (under g_mutex).
std::mutex g_mutex;
std::optional<std::uintptr_t> g_request; // a new visual (0 = stop)
std::optional<Head> g_head;

// Game thread only.
std::uintptr_t g_visual = 0;
std::uintptr_t g_skeleton = 0;
short g_head_bone = -1;
short g_eye_bones[2] = {-1, -1}; // Dummy_EyeFX_01 / _02
std::uint64_t g_frames = 0;
std::uint64_t g_reads = 0;
std::uint64_t g_failures = 0;
std::uint64_t g_stale = 0;
constexpr std::uint64_t kLogEvery = 300;
std::atomic<std::uint64_t> g_generation{0};

// Which body to follow, resolved natively every frame (agent U, re-local/ghidra/agents/U_cutscene_actors.md): the
// player-controlled character from the player manager, or the timeline dummy standing in for it while a scene shows
// the dummy and hides the real body (the real one is hidden, not moved, so the camera stayed behind). Its current
// visual is looked up every frame instead of trusting the pointer Lua sent at VR start. 4.1.1.7631656, all Confirmed
// in U's decompiles; prologues checked before use.
constexpr std::uintptr_t kPlayerManager = 0x60487A0;  // PlayerManager*
constexpr std::uintptr_t kNullHandle = 0x5EEB6E0;     // the null EntityHandle value
constexpr std::uintptr_t kEntityWorld = 0x6007F68;    // client EntityWorld*
constexpr std::uintptr_t kPrimaryPlayerRva = 0x3BAADF0;    // u16 (PlayerManager*)
constexpr std::uintptr_t kPlayerCharacterRva = 0x17F4FF0;  // EntityHandle* (PlayerManager*, EntityHandle* out, i16 player)
constexpr std::uintptr_t kVisualComponentRva = 0x1830210;  // VisualComponent* (EntityWorld*, EntityHandle); +0 = ls::Visual*
constexpr std::uintptr_t kPreviewDummyRva = 0x13FA710;     // TLPreviewDummy* (EntityWorld*, EntityHandle) or 0
constexpr std::uintptr_t kSetActorVisibleRva = 0x1D5A350;  // (TimelineSystem*, ctx*, EntityHandle*, Timeline*, bool show, i16 player)
constexpr std::size_t kDummyStandsFor = 0x88;  // TLPreviewDummy: the entity it stands in for
constexpr std::size_t kVisualEntity = 0x108;   // ls::Visual: owning EntityHandle
bool g_native_ready = false;
std::atomic<bool> g_following{false};          // VR play wants the body followed (Lua's skeleton= line)
std::atomic<std::uint64_t> g_dummy{0};          // the dummy being followed instead of the character (0: none)
std::atomic<std::uint64_t> g_swap_calls{0};
std::uint64_t g_followed_entity = 0;            // game thread: the entity whose visual is attached
// Stand-ins the primary player's scenes currently show (agent AV F4): added on show, removed on hide; the followed one
// is checked every frame and re-picked from these when it stops standing for the controlled character.
std::mutex g_shown_mutex;
std::vector<std::uint64_t> g_shown_dummies;
vr::Vec3 g_last_head_model{};     // the last Head_M model position read (for a body without one, agent AV F3)
bool g_have_head_model = false;
std::uint64_t g_native_switches = 0;
safetyhook::InlineHook g_swap_hook;

using PrimaryPlayerFn = std::uint16_t (*)(std::uintptr_t);
using PlayerCharacterFn = std::uint64_t* (*)(std::uintptr_t, std::uint64_t*, std::int16_t);
using ByHandleFn = std::uintptr_t (*)(std::uintptr_t, std::uint64_t);

// The game calls, guarded: during loads the player manager or the world can be half built.
std::uint64_t player_manager_character();

// The character to follow: the one the hotbar acts for (gameui.cpp, agent AF), else the player manager's.
std::uint64_t controlled_raw() {
    if (const std::uint64_t acting = gameui::acting_character()) {
        return acting;
    }
    return player_manager_character();
}

std::uint64_t player_manager_character() {
    __try {
        const auto manager = *reinterpret_cast<std::uintptr_t*>(g_module.base + kPlayerManager);
        if (!manager) {
            return 0;
        }
        const std::uint16_t player = reinterpret_cast<PrimaryPlayerFn>(g_module.base + kPrimaryPlayerRva)(manager);
        std::uint64_t handle = 0;
        reinterpret_cast<PlayerCharacterFn>(g_module.base + kPlayerCharacterRva)(manager, &handle,
                                                                                  static_cast<std::int16_t>(player));
        return handle == *reinterpret_cast<std::uint64_t*>(g_module.base + kNullHandle) ? 0 : handle;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The primary (local) player's index, -1 if unknown.
int primary_player() {
    __try {
        const auto manager = *reinterpret_cast<std::uintptr_t*>(g_module.base + kPlayerManager);
        return manager ? static_cast<int>(reinterpret_cast<PrimaryPlayerFn>(g_module.base + kPrimaryPlayerRva)(manager)) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

std::uintptr_t by_handle_raw(std::uintptr_t rva, std::uint64_t handle, std::uintptr_t world = 0) {
    __try {
        if (!world) {
            world = *reinterpret_cast<std::uintptr_t*>(g_module.base + kEntityWorld);
        }
        return world && handle ? reinterpret_cast<ByHandleFn>(g_module.base + rva)(world, handle) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The client entity world as the gameplay camera's own code reads it (controller +0x320, next to its camera entity at
// +0x318; agent U): the global at exe+0x6007F68 held a bad value on 2026-10-23 and the visual lookup faulted in it.
std::uintptr_t camera_world() {
    const std::uintptr_t controller = camera::controller();
    std::uintptr_t world = 0;
    if (controller) {
        mem::try_copy(&world, controller + 0x320, sizeof(world));
    }
    return world;
}

std::uintptr_t visual_of(std::uint64_t entity) {
    const std::uintptr_t world = camera_world();
    const std::uintptr_t component = world ? by_handle_raw(kVisualComponentRva, entity, world) : 0;
    std::uintptr_t visual = 0;
    if (component) {
        mem::try_copy(&visual, component, sizeof(visual));
    }
    return visual;
}

// TimelineSetActorVisibleForPlayer entry (game thread): a scene hides one body and shows another for a player.
std::uintptr_t on_set_actor_visible(std::uintptr_t system, std::uintptr_t ctx, std::uintptr_t handle_ptr,
                                    std::uintptr_t timeline, std::uintptr_t show_arg, std::uintptr_t player_arg) {
    // Nothing to follow before a character is controlled (the loading screen's scenes: the 2026-10-23 run faulted
    // inside the dummy lookup there, with the global world not ready); the scene's own world otherwise (agent U).
    const std::uint64_t controlled_now = controlled_raw();
    if (!controlled_now) {
        return g_swap_hook.call<std::uintptr_t>(system, ctx, handle_ptr, timeline, show_arg, player_arg);
    }
    std::uint64_t handle = 0;
    mem::try_copy(&handle, handle_ptr, sizeof(handle));
    const bool show = (show_arg & 0xFF) != 0;
    // Only the primary player's calls (agent AV F5): the mod's eye view is player 2, and player 0 recurses over all.
    const int player = static_cast<std::int16_t>(player_arg & 0xFFFF);
    const int primary = primary_player();
    if (player != primary) {
        const std::uint64_t n = g_swap_calls.fetch_add(1, std::memory_order_relaxed);
        if (n < 40) {
            log::info("skeleton: scene {} entity 0x{:X} for player {} (not the primary player {}): ignored", show ? "shows" : "hides",
                      handle, player, primary);
        }
        return g_swap_hook.call<std::uintptr_t>(system, ctx, handle_ptr, timeline, show_arg, player_arg);
    }
    std::uintptr_t world = 0;
    mem::try_copy(&world, ctx, sizeof(world));
    const std::uintptr_t dummy = by_handle_raw(kPreviewDummyRva, handle, world);
    std::uint64_t stands_for = 0;
    if (dummy) {
        mem::try_copy(&stands_for, dummy + kDummyStandsFor, sizeof(stands_for));
    }
    const std::uint64_t controlled = controlled_now;
    if (dummy) {
        std::scoped_lock lock(g_shown_mutex);
        const auto it = std::find(g_shown_dummies.begin(), g_shown_dummies.end(), handle);
        if (show && it == g_shown_dummies.end()) {
            g_shown_dummies.push_back(handle);
        } else if (!show && it != g_shown_dummies.end()) {
            g_shown_dummies.erase(it);
        }
    }
    const std::uint64_t followed = g_dummy.load(std::memory_order_relaxed);
    const char* change = nullptr;
    if (dummy && show && controlled && stands_for == controlled) {
        g_dummy = handle;
        change = "the scene shows a stand-in for the controlled character: the camera follows the stand-in";
    } else if (followed && ((dummy && !show && handle == followed) || (!dummy && show && handle == controlled))) {
        g_dummy = 0;
        change = "the stand-in is gone: the camera follows the controlled character again";
    }
    const std::uint64_t n = g_swap_calls.fetch_add(1, std::memory_order_relaxed);
    if (change || n < 40) {
        log::info("skeleton: scene {} entity 0x{:X} for player {} (stand-in {}{}; controlled 0x{:X}; timeline 0x{:X}){}{}",
                  show ? "shows" : "hides", handle, static_cast<std::int16_t>(player_arg & 0xFFFF),
                  dummy ? "yes, for 0x" : "no", dummy ? std::format("{:X}", stands_for) : std::string(), controlled,
                  timeline, change ? ": " : "", change ? change : "");
    }
    return g_swap_hook.call<std::uintptr_t>(system, ctx, handle_ptr, timeline, show_arg, player_arg);
}

bool prologue(std::uintptr_t rva, std::initializer_list<std::uint8_t> expected) {
    std::size_t i = 0;
    for (const std::uint8_t b : expected) {
        std::uint8_t actual = 0;
        if (!mem::try_copy(&actual, g_module.base + rva + i++, 1) || actual != b) {
            return false;
        }
    }
    return true;
}

// Slot 30 watch (the hook runs on animation threads; everything it touches is atomic).
safetyhook::MidHook g_slot30_hook;
std::atomic<std::uintptr_t> g_watched{0};
std::atomic<std::uint64_t> g_slot30_calls{0};       // for the watched skeleton
std::atomic<std::uint64_t> g_slot30_all{0};         // every skeleton
std::atomic<std::uint32_t> g_slot30_frame{0};       // game frame of the watched skeleton's last call
std::atomic<std::uint32_t> g_slot30_same_frame{0};  // calls in that frame
std::atomic<std::uint32_t> g_slot30_max_per_frame{0};
constexpr std::size_t kThreadSlots = 6;
std::array<std::atomic<std::uint32_t>, kThreadSlots> g_slot30_threads{};
std::atomic<std::uint32_t> g_main_thread{0};        // the thread on_frame runs on
// on_frame's view of the ordering: frames where slot 30 had already run for this frame when the head was
// read, vs. frames where it hadn't (yet).
std::uint64_t g_read_after_slot30 = 0;
std::uint64_t g_read_before_slot30 = 0;
std::uint64_t g_last_slot30_calls = 0;

void note_thread(std::uint32_t id) {
    for (auto& slot : g_slot30_threads) {
        std::uint32_t seen = slot.load(std::memory_order_relaxed);
        if (seen == id) {
            return;
        }
        if (seen == 0 && slot.compare_exchange_strong(seen, id, std::memory_order_relaxed)) {
            return;
        }
        if (seen == id) {
            return;
        }
    }
}

void on_slot30(safetyhook::Context& ctx) {
    g_slot30_all.fetch_add(1, std::memory_order_relaxed);
    if (ctx.rcx != g_watched.load(std::memory_order_relaxed) || ctx.rcx == 0) {
        return;
    }
    g_slot30_calls.fetch_add(1, std::memory_order_relaxed);
    bodyik::apply(ctx.rcx); // arms to the controllers, before skinning reads the pose
    std::uint32_t frame = 0;
    std::memcpy(&frame, reinterpret_cast<const void*>(g_module.base + layout::kFrameCounter), sizeof(frame));
    if (g_slot30_frame.exchange(frame, std::memory_order_relaxed) == frame) {
        const std::uint32_t n = g_slot30_same_frame.fetch_add(1, std::memory_order_relaxed) + 2;
        std::uint32_t max = g_slot30_max_per_frame.load(std::memory_order_relaxed);
        while (n > max && !g_slot30_max_per_frame.compare_exchange_weak(max, n, std::memory_order_relaxed)) {
        }
    } else {
        g_slot30_same_frame.store(0, std::memory_order_relaxed);
    }
    note_thread(GetCurrentThreadId());
}

std::string slot30_threads() {
    std::string out;
    const std::uint32_t main = g_main_thread.load(std::memory_order_relaxed);
    for (const auto& slot : g_slot30_threads) {
        if (const std::uint32_t id = slot.load(std::memory_order_relaxed)) {
            out += std::format("{}{}{}", out.empty() ? "" : ", ", id, id == main ? " (game thread)" : "");
        }
    }
    return out.empty() ? "none" : out;
}

struct Bone {
    std::string name;
    short parent = -1;
};

std::uintptr_t rva(std::uintptr_t offset) {
    return g_module.base + offset;
}

template <class T>
bool copy(T& out, std::uintptr_t address) {
    return mem::try_copy(&out, address, sizeof(T));
}

// A C string of at most 127 characters, read safely.
std::string read_name(std::uintptr_t address) {
    std::array<char, 128> buffer{};
    for (std::size_t i = 0; i + 1 < buffer.size(); ++i) {
        if (!copy(buffer[i], address + i) || buffer[i] == '\0') {
            buffer[i] = '\0';
            break;
        }
    }
    return buffer.data();
}

// The skeleton of the visual's default slot (remapper slot -1), or of the first slot; 0 with a reason.
std::uintptr_t find_skeleton(std::uintptr_t visual, std::string& why, bool verbose) {
    std::uintptr_t vtable = 0;
    if (!copy(vtable, visual) || vtable != rva(layout::kVisualVtable)) {
        why = std::format("not an ls::Visual (vtable {})", mem::describe(g_module, vtable));
        return 0;
    }
    std::uintptr_t slots = 0;
    std::uint8_t count = 0;
    if (!copy(slots, visual + layout::kVisualSkeletonSlots) || !copy(count, visual + layout::kVisualSkeletonSlotCount) ||
        !slots || count == 0) {
        why = std::format("no skeleton slots (array 0x{:X}, count {})", slots, count);
        return 0;
    }
    std::uintptr_t fallback = 0;
    for (std::uint8_t i = 0; i < count && i < 16; ++i) {
        std::uintptr_t slot = 0;
        std::int32_t remapper = 0;
        std::uintptr_t skeleton = 0;
        std::uintptr_t skeleton_vtable = 0;
        if (!copy(slot, slots + i * sizeof(void*)) || !copy(remapper, slot) ||
            !copy(skeleton, slot + layout::kSlotSkeleton) || !copy(skeleton_vtable, skeleton)) {
            continue;
        }
        if (skeleton_vtable != rva(layout::kSkeletonVtable)) {
            if (verbose) log::info("skeleton: slot {} of visual 0x{:X}: 0x{:X} has vtable {}, not the Skeleton's", i, visual,
                      skeleton, mem::describe(g_module, skeleton_vtable));
            continue;
        }
        if (remapper == -1) {
            return skeleton;
        }
        if (!fallback) {
            fallback = skeleton;
        }
    }
    if (!fallback) {
        why = std::format("none of {} slot(s) holds a Skeleton", count);
    }
    return fallback;
}

std::vector<Bone> read_bones(std::uintptr_t skeleton) {
    std::vector<Bone> bones;
    std::uintptr_t granny = 0;
    std::int32_t count = 0;
    std::uintptr_t array = 0;
    if (!copy(granny, skeleton + layout::kSkeletonGranny) || !copy(count, granny + layout::kGrannyBoneCount) ||
        !copy(array, granny + layout::kGrannyBones) || count <= 0 || count > 1024) {
        log::warn("skeleton: can't read the Granny skeleton of 0x{:X} (granny 0x{:X}, {} bones)", skeleton, granny,
                  count);
        return bones;
    }
    bones.reserve(static_cast<std::size_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uintptr_t bone = array + static_cast<std::uintptr_t>(i) * layout::kGrannyBoneStride;
        std::uintptr_t name = 0;
        std::int16_t parent = -1;
        copy(name, bone + layout::kGrannyBoneName);
        copy(parent, bone + layout::kGrannyBoneParent);
        bones.push_back({name ? read_name(name) : std::string("?"), parent});
    }
    return bones;
}

bool bone_matrix(std::uintptr_t skeleton, short bone, float (&out)[16]) {
    using GetBoneWorldMatrixFn = bool (*)(std::uintptr_t, short, float*);
    const auto fn = mem::read<GetBoneWorldMatrixFn>(rva(layout::kSkeletonVtable) +
                                                    layout::kGetBoneWorldMatrixSlot * sizeof(void*));
    return fn(skeleton, bone, out);
}

// Rotation of a row-major matrix whose rows 0-2 are the bone's X, Y, Z axes (row-vector convention, as the
// world-pose buffers are).
vr::Quat rows_to_quat(const float (&m)[16]) {
    // Column-vector rotation R = transpose of the 3x3 rows: R[r][c] = m[c*4 + r].
    const auto R = [&](int r, int c) { return m[c * 4 + r]; };
    const float trace = R(0, 0) + R(1, 1) + R(2, 2);
    vr::Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(R(2, 1) - R(1, 2)) / s, (R(0, 2) - R(2, 0)) / s, (R(1, 0) - R(0, 1)) / s, 0.25f * s};
    } else if (R(0, 0) > R(1, 1) && R(0, 0) > R(2, 2)) {
        const float s = std::sqrt(1.0f + R(0, 0) - R(1, 1) - R(2, 2)) * 2.0f;
        q = {0.25f * s, (R(0, 1) + R(1, 0)) / s, (R(0, 2) + R(2, 0)) / s, (R(2, 1) - R(1, 2)) / s};
    } else if (R(1, 1) > R(2, 2)) {
        const float s = std::sqrt(1.0f + R(1, 1) - R(0, 0) - R(2, 2)) * 2.0f;
        q = {(R(0, 1) + R(1, 0)) / s, 0.25f * s, (R(1, 2) + R(2, 1)) / s, (R(0, 2) - R(2, 0)) / s};
    } else {
        const float s = std::sqrt(1.0f + R(2, 2) - R(0, 0) - R(1, 1)) * 2.0f;
        q = {(R(0, 2) + R(2, 0)) / s, (R(1, 2) + R(2, 1)) / s, 0.25f * s, (R(1, 0) - R(0, 1)) / s};
    }
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length > 0.0f) {
        for (float& c : q) {
            c /= length;
        }
    }
    return q;
}

// Logs every bone (index, name, parent) eight to a line, then the ones an arm/head IK chain needs.
void log_bones(const std::vector<Bone>& bones) {
    log::info("skeleton: {} bones", bones.size());
    std::string line;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        line += std::format("{}{}:{}<{}", line.empty() ? "" : "  ", i, bones[i].name, bones[i].parent);
        if ((i + 1) % 8 == 0 || i + 1 == bones.size()) {
            log::info("skeleton:   {}", line);
            line.clear();
        }
    }
    static const char* const kInteresting[] = {"head", "neck", "spine", "clavicle", "shoulder", "arm", "elbow",
                                               "wrist", "hand", "finger", "thumb", "index", "root", "pelvis", "hip"};
    for (std::size_t i = 0; i < bones.size(); ++i) {
        std::string lower = bones[i].name;
        for (char& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        for (const char* word : kInteresting) {
            if (lower.find(word) != std::string::npos) {
                log::info("skeleton: chain bone {} '{}' parent {} ('{}')", i, bones[i].name, bones[i].parent,
                          bones[i].parent >= 0 && static_cast<std::size_t>(bones[i].parent) < bones.size()
                              ? bones[static_cast<std::size_t>(bones[i].parent)].name
                              : "-");
                break;
            }
        }
    }
}

void attach_now(std::uintptr_t visual) {
    g_visual = 0;
    g_skeleton = 0;
    g_head_bone = -1;
    g_eye_bones[0] = g_eye_bones[1] = -1;
    g_watched.store(0, std::memory_order_relaxed);
    bodyik::set_rig(0, 0, {});
    {
        std::scoped_lock lock(g_mutex);
        g_head.reset();
    }
    if (!visual) {
        log::info("skeleton: stopped");
        return;
    }
    // Slot 10 must still be the GetBoneWorldMatrix this code was checked against.
    std::uintptr_t slot10 = 0;
    copy(slot10, rva(layout::kSkeletonVtable) + layout::kGetBoneWorldMatrixSlot * sizeof(void*));
    if (slot10 != rva(layout::kGetBoneWorldMatrix)) {
        log::error("skeleton: Skeleton vtable slot 10 is {}, expected exe+0x{:X}; not reading bones (game updated?)",
                   mem::describe(g_module, slot10), layout::kGetBoneWorldMatrix);
        return;
    }
    std::string why;
    const std::uintptr_t skeleton = find_skeleton(visual, why, true);
    if (!skeleton) {
        log::warn("skeleton: visual 0x{:X}: {}", visual, why);
        return;
    }
    std::int16_t count = 0;
    copy(count, skeleton + layout::kSkeletonBoneCount);
    log::info("skeleton: visual 0x{:X} -> skeleton 0x{:X} ({} bones in the Skeleton)", visual, skeleton, count);
    const std::vector<Bone> bones = read_bones(skeleton);
    log_bones(bones);
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == "Head_M") {
            g_head_bone = static_cast<short>(i);
        } else if (bones[i].name == "Dummy_EyeFX_01") {
            g_eye_bones[0] = static_cast<short>(i);
        } else if (bones[i].name == "Dummy_EyeFX_02") {
            g_eye_bones[1] = static_cast<short>(i);
        }
    }
    // The visual's attachments (agent K, K_weapons.md 1.1: Visual+0xB8, 0x30 each, count +0xC4): which bone each
    // hangs from, so the body IK knows which dummy bones to move with the hands (weapons ride Dummy_R/L_Hand).
    std::uintptr_t attachments = 0;
    std::uint32_t attachment_count = 0;
    copy(attachments, visual + 0xB8);
    copy(attachment_count, visual + 0xC4);
    log::info("skeleton: {} attachment(s) on the visual", attachment_count);
    for (std::uint32_t i = 0; i < attachment_count && i < 64 && attachments; ++i) {
        const std::uintptr_t a = attachments + i * 0x30;
        std::uintptr_t child = 0;
        std::int32_t source = -1, attach = -1;
        std::uint32_t flags = 0;
        std::uint8_t source_type = 0, target_type = 0;
        float offset[3]{};
        copy(child, a + 0x00);
        copy(source, a + 0x08);
        copy(attach, a + 0x0C);
        copy(flags, a + 0x18);
        copy(source_type, a + 0x1C);
        copy(target_type, a + 0x1D);
        mem::try_copy(offset, a + 0x24, sizeof(offset));
        const std::string bone = target_type == 1 && attach >= 0 && static_cast<std::size_t>(attach) < bones.size()
                                     ? bones[static_cast<std::size_t>(attach)].name
                                     : std::string(attach < 0 ? "(root)" : "(socket or other skeleton)");
        log::info("skeleton:   attachment {}: child visual 0x{:X} from source {} (type {}) on parent {} '{}' (type {}), "
                  "flags 0x{:X}{}{}{}, offset ({:.3f}, {:.3f}, {:.3f})",
                  i, child, source, source_type, attach, bone, target_type, flags, flags & 0x8000 ? " weapon" : "",
                  flags & 0x4000 ? " armor" : "", flags & 0x800 ? " do-not-update" : "", offset[0], offset[1], offset[2]);
    }
    if (g_head_bone < 0) {
        log::warn("skeleton: no Head_M bone; head tracking from the skeleton is off");
    } else {
        log::info("skeleton: Head_M is bone {}; eyes {} / {}{}", g_head_bone, g_eye_bones[0], g_eye_bones[1],
                  g_eye_bones[0] >= 0 && g_eye_bones[1] >= 0 ? "" : " (no eye bones: eye height from the head bone)");
    }
    g_visual = visual;
    g_skeleton = skeleton;
    g_generation.fetch_add(1, std::memory_order_relaxed);
    g_reads = g_failures = g_stale = 0;
    g_watched.store(skeleton, std::memory_order_relaxed);
    std::vector<bodyik::Bone> rig;
    rig.reserve(bones.size());
    for (const Bone& b : bones) {
        rig.push_back({b.name, b.parent});
    }
    bodyik::set_rig(skeleton, visual, rig);
    g_read_after_slot30 = g_read_before_slot30 = 0;
    g_last_slot30_calls = g_slot30_calls.load(std::memory_order_relaxed);
}

} // namespace

void init(const ModuleInfo& module) {
    g_module = module;
    // Native follow (agent U): the game functions it calls, checked byte for byte.
    g_native_ready = prologue(kPrimaryPlayerRva, {0x83, 0x79, 0x74, 0x00}) &&
                     prologue(kPlayerCharacterRva, {0x8B, 0x81, 0x34, 0x01, 0x00, 0x00, 0x4C, 0x8B, 0xD2}) &&
                     prologue(kVisualComponentRva, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00}) &&
                     prologue(kPreviewDummyRva, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00});
    if (g_native_ready && prologue(kSetActorVisibleRva, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54})) {
        if (auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(g_module.base + kSetActorVisibleRva),
                                                       reinterpret_cast<void*>(&on_set_actor_visible))) {
            g_swap_hook = std::move(*hook);
        }
    }
    log::info("skeleton: native follow {} (the controlled character's visual every frame{})",
              g_native_ready ? "ready" : "unavailable (a function didn't match this build; Lua's visual only)",
              g_swap_hook ? ", scene stand-ins tracked at exe+0x1D5A350" : "");
    const auto site = sig::lookup("skeleton_update_mesh_bindings");
    if (!site) {
        health::set("body hook (slot 30)", health::State::Unavailable, "signature skeleton_update_mesh_bindings");
        log::warn("skeleton: signature skeleton_update_mesh_bindings not resolved; no slot 30 watch");
        return;
    }
    if (*site != module.base + layout::kUpdateMeshBindings) {
        log::warn("skeleton: skeleton_update_mesh_bindings at exe+0x{:X}, expected exe+0x{:X}; no slot 30 watch",
                  *site - module.base, layout::kUpdateMeshBindings);
        return;
    }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(*site), &on_slot30);
    if (!hook) {
        health::set("body hook (slot 30)", health::State::Unavailable, "hook failed");
        log::error("skeleton: slot 30 mid-hook failed (error type {})", static_cast<int>(hook.error().type));
        return;
    }
    g_slot30_hook = std::move(*hook);
    health::set("body hook (slot 30)", health::State::Ready);
    log::info("skeleton: slot 30 watch installed at exe+0x{:X} (counts only, for the followed skeleton)",
              *site - module.base);
}

void attach_visual(std::uintptr_t visual) {
    // With the native follow ready, Lua's skeleton= line only turns following on (agent AV F7: re-attaching its
    // character visual put the camera on the hidden real body for a frame during a stand-in scene); on_frame picks the
    // body. Off still detaches.
    if (g_native_ready && visual) {
        if (!g_following.exchange(true)) {
            log::info("skeleton: following on (the body is picked natively; Lua's visual 0x{:X} not attached)", visual);
        }
        return;
    }
    std::scoped_lock lock(g_mutex);
    g_request = visual;
    g_following = visual != 0;
}

void on_frame() {
    ++g_frames;
    g_main_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    std::optional<std::uintptr_t> request;
    {
        std::scoped_lock lock(g_mutex);
        request.swap(g_request);
    }
    if (request) {
        attach_now(*request);
        g_followed_entity = 0;
    }
    // The body actually shown for the controlled character, every frame (Lua's pointer is the fallback).
    if (g_native_ready && g_following.load(std::memory_order_relaxed)) {
        const std::uint64_t controlled = controlled_raw();
        // The followed stand-in must still exist and stand for the controlled character (agent AV F4); otherwise
        // another shown stand-in that does, else the character itself.
        const auto stands_for_controlled = [&](std::uint64_t d) {
            const std::uintptr_t object = d ? by_handle_raw(kPreviewDummyRva, d, camera_world()) : 0;
            std::uint64_t stands_for = 0;
            return object && mem::try_copy(&stands_for, object + kDummyStandsFor, sizeof(stands_for)) && controlled &&
                   stands_for == controlled;
        };
        if (const std::uint64_t d = g_dummy.load(std::memory_order_relaxed); d && !stands_for_controlled(d)) {
            std::uint64_t replacement = 0;
            {
                std::scoped_lock lock(g_shown_mutex);
                g_shown_dummies.erase(std::remove_if(g_shown_dummies.begin(), g_shown_dummies.end(),
                                                     [&](std::uint64_t x) {
                                                         return !by_handle_raw(kPreviewDummyRva, x, camera_world());
                                                     }),
                                      g_shown_dummies.end());
                for (const std::uint64_t x : g_shown_dummies) {
                    if (x != d && stands_for_controlled(x)) {
                        replacement = x;
                        break;
                    }
                }
            }
            g_dummy = replacement;
            log::info("skeleton: the followed stand-in 0x{:X} no longer stands for the controlled character 0x{:X}: {}", d,
                      controlled, replacement ? std::format("following the stand-in 0x{:X}", replacement)
                                              : std::string("following the character"));
        }
        const std::uint64_t dummy = g_dummy.load(std::memory_order_relaxed);
        // Lua hides the followed body's head parts: the stand-in's own while one is followed (agent AV F8).
        static std::uint64_t published = ~0ull;
        if (dummy != published) {
            published = dummy;
            bridge::set_status("followed", dummy ? std::format("0x{:016X}", dummy) : std::string());
        }
        static std::uint64_t last_controlled = 0;
        if (controlled != last_controlled) {
            log::info("skeleton: the player manager's controlled character is now 0x{:X} (was 0x{:X})", controlled,
                      last_controlled);
            last_controlled = controlled;
        }
        const std::uint64_t entity = dummy ? dummy : controlled;
        const std::uintptr_t visual = entity ? visual_of(entity) : 0;
        std::uintptr_t vtable = 0;
        std::uint64_t owner = 0;
        if (visual) {
            mem::try_copy(&vtable, visual, sizeof(vtable));
            mem::try_copy(&owner, visual + kVisualEntity, sizeof(owner));
        }
        if (visual && vtable == rva(layout::kVisualVtable) && (visual != g_visual || entity != g_followed_entity)) {
            ++g_native_switches;
            log::info("skeleton: following the visual 0x{:X} of {} 0x{:X} (was 0x{:X}; the visual's own entity 0x{:X}{})",
                      visual, dummy ? "the scene's stand-in" : "the controlled character", entity, g_visual, owner,
                      owner == entity ? "" : ", NOT the followed one");
            if (dummy) {
                log::info("skeleton: the stand-in's head isn't hidden by the mod (only the character's is)");
            }
            g_followed_entity = entity;
            if (visual != g_visual) {
                attach_now(visual);
            }
        }
    }
    if (!g_visual) {
        return;
    }
    // Equipment changes and reloads replace skeletons: re-resolve from the visual every frame.
    std::string why;
    const std::uintptr_t skeleton = find_skeleton(g_visual, why, false);
    if (skeleton != g_skeleton) {
        if (!skeleton) {
            log::warn("skeleton: the visual 0x{:X} lost its skeleton ({}); stopped", g_visual, why);
            attach_now(0);
            return;
        }
        log::info("skeleton: the visual's skeleton changed 0x{:X} -> 0x{:X}; re-reading bones", g_skeleton, skeleton);
        attach_now(g_visual);
        if (!g_visual) {
            return;
        }
    }
    if (g_head_bone < 0) {
        // No Head_M (e.g. a shapeshifted stand-in, agent AV F3): the body's own transform with the last head offset
        // read, so the camera stays with this body instead of falling back to the character's.
        std::array<float, 10> t{};
        std::uint32_t now = 0;
        copy(now, rva(layout::kFrameCounter));
        if (g_have_head_model && mem::try_copy(t.data(), g_visual + layout::kVisualWorldTransform, sizeof(t))) {
            Head h;
            h.model = g_last_head_model;
            h.visual_position = {t[4], t[5], t[6]};
            h.visual_rotation = {t[0], t[1], t[2], t[3]};
            h.visual_scale = t[7] != 0.0f ? t[7] : 1.0f;
            const vr::Vec3 o = vr::rotate(h.visual_rotation, {h.model[0] * h.visual_scale, h.model[1] * h.visual_scale,
                                                              h.model[2] * h.visual_scale});
            h.world = {t[4] + o[0], t[5] + o[1], t[6] + o[2]};
            h.world_rotation = h.visual_rotation;
            h.frame = now;
            h.fresh = true;
            std::scoped_lock lock(g_mutex);
            g_head = h;
        }
        return;
    }

    float matrix[16];
    std::array<float, 10> transform{}; // visual world transform: quat, translate, scale
    std::uint32_t frame = 0;
    std::uint32_t pose_frame = 0;
    copy(frame, rva(layout::kFrameCounter));
    copy(pose_frame, g_skeleton + layout::kSkeletonPoseFrame);
    if (!bone_matrix(g_skeleton, g_head_bone, matrix) ||
        !mem::try_copy(transform.data(), g_visual + layout::kVisualWorldTransform, sizeof(transform))) {
        if (g_failures++ % kLogEvery == 0) {
            log::warn("skeleton: head bone read failed ({} so far)", g_failures);
        }
        return;
    }
    ++g_reads;
    if (g_slot30_frame.load(std::memory_order_relaxed) == frame) {
        ++g_read_after_slot30;
    } else {
        ++g_read_before_slot30;
    }
    const bool fresh = pose_frame + 1 >= frame;
    if (!fresh) {
        ++g_stale;
    }
    Head head;
    head.model = {matrix[12], matrix[13], matrix[14]};
    g_last_head_model = head.model;
    g_have_head_model = true;
    const vr::Quat visual_rotation{transform[0], transform[1], transform[2], transform[3]};
    const vr::Vec3 scaled{head.model[0] * transform[7], head.model[1] * transform[8], head.model[2] * transform[9]};
    const vr::Vec3 offset = vr::rotate(visual_rotation, scaled);
    head.world = {transform[4] + offset[0], transform[5] + offset[1], transform[6] + offset[2]};
    head.world_rotation = vr::multiply(visual_rotation, rows_to_quat(matrix));
    head.frame = frame;
    head.fresh = fresh;
    head.visual_position = {transform[4], transform[5], transform[6]};
    head.visual_rotation = visual_rotation;
    head.visual_scale = transform[7] != 0.0f ? transform[7] : 1.0f;
    float eye_matrix[2][16];
    if (g_eye_bones[0] >= 0 && g_eye_bones[1] >= 0 && bone_matrix(g_skeleton, g_eye_bones[0], eye_matrix[0]) &&
        bone_matrix(g_skeleton, g_eye_bones[1], eye_matrix[1])) {
        const vr::Vec3 mid{(eye_matrix[0][12] + eye_matrix[1][12]) * 0.5f, (eye_matrix[0][13] + eye_matrix[1][13]) * 0.5f,
                           (eye_matrix[0][14] + eye_matrix[1][14]) * 0.5f};
        const vr::Vec3 mid_scaled{mid[0] * transform[7], mid[1] * transform[8], mid[2] * transform[9]};
        const vr::Vec3 mid_offset = vr::rotate(visual_rotation, mid_scaled);
        head.eyes_model = mid;
        head.eyes_world = {transform[4] + mid_offset[0], transform[5] + mid_offset[1], transform[6] + mid_offset[2]};
        const float dx = eye_matrix[0][12] - eye_matrix[1][12], dy = eye_matrix[0][13] - eye_matrix[1][13],
                    dz = eye_matrix[0][14] - eye_matrix[1][14];
        head.eye_separation = std::sqrt(dx * dx + dy * dy + dz * dz) * transform[7];
        head.has_eyes = true;
    }
    {
        std::scoped_lock lock(g_mutex);
        g_head = head;
    }
    if (g_reads % kLogEvery == 1) {
        if (head.has_eyes) {
            log::info("skeleton: eyes (Dummy_EyeFX_01/02 midpoint) model ({:.3f}, {:.3f}, {:.3f}) world ({:.3f}, {:.3f}, {:.3f}), "
                      "{:.3f} apart", head.eyes_model[0], head.eyes_model[1], head.eyes_model[2], head.eyes_world[0],
                      head.eyes_world[1], head.eyes_world[2], head.eye_separation);
        }
        log::info("skeleton: frame {} (pose frame {}) Head_M model ({:.3f}, {:.3f}, {:.3f}) world ({:.3f}, {:.3f}, "
                  "{:.3f}) rot ({:.3f}, {:.3f}, {:.3f}, {:.3f}); visual at ({:.3f}, {:.3f}, {:.3f}) rot ({:.3f}, "
                  "{:.3f}, {:.3f}, {:.3f}) scale ({:.3f}, {:.3f}, {:.3f}); matrix rows ({:.3f} {:.3f} {:.3f} | {:.3f} "
                  "{:.3f} {:.3f} | {:.3f} {:.3f} {:.3f})",
                  frame, pose_frame, head.model[0], head.model[1], head.model[2], head.world[0], head.world[1],
                  head.world[2], head.world_rotation[0], head.world_rotation[1], head.world_rotation[2],
                  head.world_rotation[3], transform[4], transform[5], transform[6], transform[0], transform[1],
                  transform[2], transform[3], transform[7], transform[8], transform[9], matrix[0], matrix[1],
                  matrix[2], matrix[4], matrix[5], matrix[6], matrix[8], matrix[9], matrix[10]);
        const std::uint64_t calls = g_slot30_calls.load(std::memory_order_relaxed);
        log::info("skeleton: slot 30 for this skeleton: {} calls since the last line ({} for all skeletons), up to {} "
                  "in one frame, threads {}; head reads after this frame's slot 30 {} / before {}",
                  calls - g_last_slot30_calls, g_slot30_all.load(std::memory_order_relaxed),
                  g_slot30_max_per_frame.load(std::memory_order_relaxed), slot30_threads(), g_read_after_slot30,
                  g_read_before_slot30);
        g_last_slot30_calls = calls;
    }
}

std::uintptr_t current() {
    return g_skeleton;
}

std::uint64_t followed_stand_in() {
    return g_dummy.load(std::memory_order_relaxed);
}

std::optional<Head> head() {
    std::scoped_lock lock(g_mutex);
    return g_head;
}

std::uint64_t generation() {
    return g_generation.load(std::memory_order_relaxed);
}

void report() {
    log::info("skeleton: slot 30 watch: {} calls for the followed skeleton, {} for all, up to {} per frame, threads {}; "
              "head reads after slot 30 {} / before {}",
              g_slot30_calls.load(std::memory_order_relaxed), g_slot30_all.load(std::memory_order_relaxed),
              g_slot30_max_per_frame.load(std::memory_order_relaxed), slot30_threads(), g_read_after_slot30,
              g_read_before_slot30);
    log::info("skeleton: visual 0x{:X} skeleton 0x{:X} head bone {}: {} reads, {} failed, {} with a pose older than "
              "the last frame",
              g_visual, g_skeleton, g_head_bone, g_reads, g_failures, g_stale);
}

std::uint64_t controlled_character() {
    return controlled_raw();
}

} // namespace bg3vr::skeleton
