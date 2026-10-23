#include "bg3vr/hold.hpp"

#include "bg3vr/bridge.hpp"
#include "bg3vr/camera.hpp"
#include "bg3vr/corpse.hpp"
#include "bg3vr/grab_math.hpp"
#include "bg3vr/health.hpp"
#include "bg3vr/log.hpp"
#include "bg3vr/memory.hpp"
#include "bg3vr/pattern.hpp"
#include "bg3vr/rayprobe.hpp"
#include "bg3vr/skeleton.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>

#include <windows.h>

#include <safetyhook.hpp>
#include <span>
#include <string>
#include <vector>

namespace bg3vr::hold {

namespace {

ModuleInfo g_module;
bool g_probe = false;
std::atomic<bool> g_hold{false};
bool g_ready = false; // every function checked
float g_reach = 1.5f;

// 4.1.1.7631656, agent E (E_items_interaction.md 1, 2.2, 3); prologues re-disassembled 2026-10-20.
struct Function {
    const char* name;
    std::uintptr_t rva;
    const char* prologue;
};
constexpr Function kEntityToItem{"ecl entity -> ecl::Item", 0x704BD0,
                                 "48 89 5C 24 18 56 48 81 EC 90 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 80 00 00 00 48 8B C2 48 8B DA 48 C1 E8 36"};
constexpr Function kSetTranslate{"ecl::Item::SetTranslate", 0x1C77050,
                                 "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 8D 59 20"};
constexpr Function kSetRotate{"ecl::Item::SetRotate", 0x1C77130,
                              "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 8D 59 20"};
constexpr Function kSuspend{"client physics suspend", 0x1C7DD00, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B D9 80 79 6A 00"};
constexpr Function kResume{"client physics resume", 0x1C7DE60, "48 89 5C 24 08 57 48 83 EC 50 48 8B D9 0F B6 41 6A"};
constexpr Function kSetFlags{"ecl::Item::SetFlags", 0x1C76D50, "40 53 56 57 48 83 EC 30 48 8B 59 18"};
// The game's own pick-up rule for the gloves (agent AD, AD_item_weight.md): what the client PickUp task requires.
constexpr Function kCharacterOf{"ecl::Character of a handle", 0x7841A0, "48 89 5C 24 18 56 48 81 EC 80 00 00 00 48 8B 05"};
constexpr Function kHasCanBeIn{"HasComponent<CanBeIn>", 0xAB1DB0, "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 40"};
constexpr Function kWeight{"item weight (grams)", 0x41D6FE0, "40 53 56 41 54 41 56 41 57 48 83 EC 30"};
constexpr Function kCanLift{"CanLift (Strength x 5 kg)", 0x1FAB030, "40 56 48 81 EC A0 00 00 00 48 8B F2 48 85 C9"};
constexpr Function kLiftingWeight{"GetLiftingWeight (stack)", 0x1BA9130, "40 55 53 41 54 41 57 48 8D 6C 24 C1"};
constexpr Function kStats{"StatsComponent of a handle", 0xAF2FE0, "48 89 5C 24 18 56 48 81 EC 80 00 00 00 48 8B 05"};
constexpr std::uintptr_t kClientWorldHolder = 0x5FDA240; // [holder] + 0x1A8 = client EntityWorld (the PickUp task's)
constexpr std::uint64_t kItemCanBePickedUp = 0x2;          // item+0x18, the client copy of the server's CanBePickedUp
constexpr std::uint64_t kItemBusy = 0x100480100;           // item+0x18 bits that refuse any interaction (0x1F5A030)
constexpr std::size_t kItemParent = 0x60;                  // in an inventory / attached when not null
constexpr std::uint64_t kNullHandle = 0xFFC0000000000000ull;
constexpr std::uintptr_t kEntityHelper = 0x6007F68; // ecl entity helper*
constexpr std::size_t kItemHandle = 0x10;
constexpr std::size_t kItemGetTransformSlot = 0xD0 / 8; // returns {quat +0x00, translate +0x10}
constexpr std::uint64_t kReportTransform = 0x40000;

using EntityToItemFn = std::uintptr_t (*)(std::uintptr_t helper, std::uint64_t handle);
using SetVecFn = void (*)(std::uintptr_t item, const float* value);
using ItemFn = void (*)(std::uintptr_t item);
using SetFlagsFn = void (*)(std::uintptr_t item, std::uint64_t mask);
using GetTransformFn = const float* (*)(std::uintptr_t item);
using CharacterOfFn = std::uintptr_t (*)(std::uintptr_t world, std::uint64_t handle);
using HasComponentFn = bool (*)(std::uintptr_t world, const std::uint64_t* handle);
using WeightFn = int* (*)(int* out, const std::uintptr_t* ctx, const std::uint64_t* handle, int amount, std::uint8_t one_unit);
using CanLiftFn = bool (*)(std::uintptr_t character, const int* grams);
using LiftingWeightFn = int (*)(const std::uintptr_t* ctx, std::uint64_t handle, std::uint32_t check_stacks);
using StatsFn = std::uintptr_t (*)(std::uintptr_t world, std::uint64_t handle);

template <class F>
F fn(const Function& f) {
    return reinterpret_cast<F>(g_module.base + f.rva);
}

bool check(const Function& f) {
    const auto pattern = parse_pattern(f.prologue);
    std::array<std::uint8_t, 64> bytes{};
    if (!pattern || pattern->size() > bytes.size() ||
        !mem::try_copy(bytes.data(), g_module.base + f.rva, pattern->size())) {
        return false;
    }
    const auto at = find_first(std::span<const std::uint8_t>(bytes.data(), pattern->size()), *pattern);
    if (!at || *at != 0) {
        log::error("grab: {} at exe+0x{:X} doesn't have the expected prologue (game updated?)", f.name, f.rva);
        return false;
    }
    return true;
}

// Hands, from the headset.
struct HandInput {
    bool grip = false;
    std::optional<vr::RigidPose> grip_pose;
    std::optional<vr::RigidPose> aim_pose;
    double time = 0.0; // sample time, seconds
    bool in_pack = false; // over a shoulder or behind the back
};
std::mutex g_mutex;
std::array<HandInput, 2> g_input{};

// Throwing and pulling ([grab]).
bool g_throw = true;
float g_throw_min_speed = 1.5f;
bool g_pull = true;
float g_pull_range = 6.0f;
float g_catch_radius = 0.35f;
constexpr float kGravity = 9.81f;
constexpr double kMaxFlight = 4.0;

// Game thread.
struct Held {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
    vr::RigidPose offset; // item relative to the hand
    std::uint64_t frames = 0;
};
struct Flight {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
    vr::Vec3 position{};
    vr::Vec3 velocity{};
    vr::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    double age = 0.0;
    int to_hand = -1; // a pull toward this hand (caught there), or -1 for a throw
};
struct Target {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
};
std::array<bool, 2> g_was_down{};
std::array<std::optional<Held>, 2> g_held{};
std::array<std::optional<Target>, 2> g_pull_target{};
std::array<grab::ThrowTracker, 2> g_trackers{};
std::array<double, 2> g_tracked_time{};
std::vector<Flight> g_flights;
std::uint64_t g_grabs = 0;
std::uint64_t g_throws = 0;
std::uint64_t g_pulls = 0;
std::uint64_t g_catches = 0;
std::atomic<bool> g_forget{false}; // set by forget() on any thread, handled on the game thread

// Gloves: the item each hand's aim highlights (game thread), published for the laser and the grip binding.
struct Highlight {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
    float distance = 0.0f;
    double seen = 0.0; // steady-clock seconds of the last ray that found it
};
std::array<std::optional<Highlight>, 2> g_highlight{};
std::array<std::atomic<int>, 2> g_aim_state{};
std::array<std::atomic<std::uint64_t>, 2> g_aim_entity{}; // the highlighted / locked item's handle, 0 for none

// The game's own outline on the item the gloves aim at (agent AC 1-2): a request into ecl::OutlineSystem's list
// (sys+0x90, pushed with 0x126F650) from inside the system's update 0x1A6DE40, so the unlocked list is only touched by
// its consumer. Request: +0x00 entity, +0x08 r, +0x0C g, +0x10 b (linear), +0x14 u16 key, +0x18 u32 op (0 add or
// recolour, 2 remove this key). Each hand has its own key, never 0 (0 removes every outline of the entity) and not a
// player id, so the mouse hover's outline is left alone.
constexpr std::uintptr_t kOutlineUpdateRva = 0x1A6DE40, kOutlinePushRva = 0x126F650;
constexpr std::uint16_t kOutlineKey = 0x7FF0;
bool g_outline = true; // [grab] outline
safetyhook::InlineHook g_outline_hook;
std::atomic<std::uint64_t> g_outline_pushes{0};
struct OutlineRequest {
    std::uint64_t entity = 0;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    std::uint16_t key = 0;
    std::uint16_t pad = 0;
    std::uint32_t op = 0;
    std::uint32_t pad2 = 0;
};
static_assert(sizeof(OutlineRequest) == 0x20);
using OutlinePushFn = void* (*)(std::uintptr_t list, const OutlineRequest* request);

const char* hand_name(std::size_t h);

void on_outline_update(std::uintptr_t sys, std::uintptr_t ctx) {
    static std::array<std::uint64_t, 2> outlined{};
    static DWORD logged_thread = 0;
    if (g_outline) {
        const auto push = reinterpret_cast<OutlinePushFn>(g_module.base + kOutlinePushRva);
        for (std::size_t h = 0; h < 2; ++h) {
            const std::uint64_t entity = g_aim_entity[h].load(std::memory_order_relaxed);
            const auto state = static_cast<Aim>(g_aim_state[h].load(std::memory_order_relaxed));
            const std::uint16_t key = static_cast<std::uint16_t>(kOutlineKey + h);
            if (outlined[h] && outlined[h] != entity) {
                OutlineRequest remove;
                remove.entity = outlined[h];
                remove.key = key;
                remove.op = 2;
                push(sys + 0x90, &remove);
                outlined[h] = 0;
            }
            if (entity) {
                // Cyan while highlighted, amber once the grip has locked on (the laser's colours).
                OutlineRequest add;
                add.entity = entity;
                const bool locked = state == Aim::Locked;
                add.r = locked ? 1.0f : 0.1f;
                add.g = locked ? 0.45f : 0.75f;
                add.b = locked ? 0.05f : 1.0f;
                add.key = key;
                add.op = 0;
                push(sys + 0x90, &add);
                if (outlined[h] != entity) {
                    log::info("gloves: {} hand outlines entity 0x{:016X} (outline system 0x{:X}, thread {})", hand_name(h),
                              entity, sys, GetCurrentThreadId());
                }
                outlined[h] = entity;
                g_outline_pushes.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    static int thread_logs = 0;
    if (logged_thread != GetCurrentThreadId()) {
        logged_thread = GetCurrentThreadId();
        if (thread_logs++ < 8) {
            log::info("gloves: the outline system updates on thread {} (ECS workers take turns)", logged_thread);
        }
    }
    g_outline_hook.call<void>(sys, ctx);
}

void install_outline() {
    std::array<std::uint8_t, 14> update{}, push{};
    constexpr std::array<std::uint8_t, 14> update_expected{0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x38,
                                                           0x48, 0x8B, 0x99, 0x90, 0x00, 0x00, 0x00};
    constexpr std::array<std::uint8_t, 14> push_expected{0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48,
                                                         0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48};
    if (!mem::try_copy(update.data(), g_module.base + kOutlineUpdateRva, update.size()) || update != update_expected ||
        !mem::try_copy(push.data(), g_module.base + kOutlinePushRva, push.size()) || push != push_expected) {
        log::warn("gloves: the outline system's update / request push don't match this build; no outline on aimed items");
        return;
    }
    auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(g_module.base + kOutlineUpdateRva),
                                               reinterpret_cast<void*>(&on_outline_update));
    if (!hook) {
        log::warn("gloves: hooking the outline system failed; no outline on aimed items");
        return;
    }
    g_outline_hook = std::move(*hook);
    log::info("gloves: outline on aimed items through ecl::OutlineSystem (update exe+0x{:X} hooked, keys 0x{:X} / 0x{:X})",
              kOutlineUpdateRva, kOutlineKey, kOutlineKey + 1);
}
std::array<std::atomic<bool>, 2> g_holding{};
std::array<std::atomic<bool>, 2> g_incoming{}; // an item is flying to this hand
std::array<int, 2> g_cone_step{};
std::uint64_t g_scan_frame = 0;
std::uint64_t g_highlights = 0;
std::uint64_t g_locks = 0;
std::uint64_t g_stows = 0;
constexpr double kHighlightHold = 0.25;   // seconds a highlight survives without a ray finding it (no flicker)
constexpr float kConeDegrees = 4.0f;      // aim assist: the rays around the centre one
constexpr int kConeRays = 6;
constexpr float kAimSkip = 0.25f;         // metres ahead of the hand before the ray starts (the player's own body)
constexpr float kHomingRange = 1.2f;      // a pulled item steers into the hand inside this distance
struct HapticSlot {
    std::atomic<bool> pending{false};
    std::atomic<float> amplitude{0.0f};
    std::atomic<float> seconds{0.0f};
};
std::array<HapticSlot, 2> g_haptic;

double seconds_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void buzz(std::size_t h, float amplitude, float seconds) {
    g_haptic[h].amplitude.store(amplitude, std::memory_order_relaxed);
    g_haptic[h].seconds.store(seconds, std::memory_order_relaxed);
    g_haptic[h].pending.store(true, std::memory_order_release);
}
std::uint64_t g_misses = 0;
std::chrono::steady_clock::time_point g_last_frame{};

const char* hand_name(std::size_t h) {
    return h == 0 ? "left" : "right";
}

vr::Quat conjugate(const vr::Quat& q) {
    return {-q[0], -q[1], -q[2], q[3]};
}

float length(const vr::Vec3& v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

vr::Vec3 minus(const vr::Vec3& a, const vr::Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

std::optional<vr::RigidPose> item_pose(std::uintptr_t item) {
    std::uintptr_t vtable = 0;
    std::uintptr_t slot = 0;
    if (!mem::try_copy(&vtable, item, sizeof(vtable)) ||
        !mem::try_copy(&slot, vtable + kItemGetTransformSlot * sizeof(void*), sizeof(slot)) ||
        !mem::in_section(g_module, ".text", slot)) {
        return std::nullopt;
    }
    const float* t = reinterpret_cast<GetTransformFn>(slot)(item);
    float values[7];
    if (!t || !mem::try_copy(values, reinterpret_cast<std::uintptr_t>(t), sizeof(float) * 4) ||
        !mem::try_copy(values + 4, reinterpret_cast<std::uintptr_t>(t) + 0x10, sizeof(float) * 3)) {
        return std::nullopt;
    }
    return vr::RigidPose{{values[4], values[5], values[6]}, {values[0], values[1], values[2], values[3]}};
}

bool still_item(std::uintptr_t item, std::uint64_t entity) {
    std::uint64_t handle = 0;
    return mem::try_copy(&handle, item + kItemHandle, sizeof(handle)) && handle == entity;
}

void place(std::uintptr_t item, const vr::Vec3& position, const vr::Quat& rotation) {
    const float p[3] = {position[0], position[1], position[2]};
    const float q[4] = {rotation[0], rotation[1], rotation[2], rotation[3]};
    fn<SetVecFn>(kSetTranslate)(item, p);
    fn<SetVecFn>(kSetRotate)(item, q);
}

// Physics back on and the final pose to the server (the end of every hold, throw and pull).
// Every gloves move of an item that doesn't end in the inventory goes to the server as "moved": there an item owned by
// someone outside the party gets the game's own crime for moving it (MoveForbiddenItem, the story's MovedBy rule;
// agent AC 5.4), as dragging or throwing it with the mouse would.
void report_move(std::uint64_t entity, const char* how) {
    bridge::push_request(std::format("moved 0x{:016X} {}", entity, how));
}

void settle(std::uintptr_t item, const char* why) {
    fn<ItemFn>(kResume)(item);
    fn<SetFlagsFn>(kSetFlags)(item, kReportTransform);
    const auto pose = item_pose(item);
    log::info("grab: item 0x{:X} {}{}", item, why,
              pose ? std::format(" at ({:.2f}, {:.2f}, {:.2f}); reported to the server", pose->position[0],
                                 pose->position[1], pose->position[2])
                   : std::string());
}

struct Resolved {
    std::uintptr_t item = 0;
    std::uint64_t entity = 0;
    vr::RigidPose pose;
    float distance = 0.0f;
};

// The client EntityWorld, as the game's own PickUp task reads it ([[exe+0x5FDA240]+0x1A8], agent AD). The global at
// exe+0x6007F68 that was used before held a bad value on 2026-10-23 (skeleton.cpp saw it too), and the entity-to-item
// lookup 0x704BD0, which reads world+0x210, crashed in it at 12:34 the first time the gloves scanned every frame.
// Logged against the camera controller's world (+0x320) and that global whenever it changes.
std::uintptr_t client_world() {
    std::uintptr_t holder = 0, world = 0;
    if (!mem::try_copy(&holder, g_module.base + kClientWorldHolder, sizeof(holder)) || !holder ||
        !mem::try_copy(&world, holder + 0x1A8, sizeof(world))) {
        world = 0;
    }
    static std::uintptr_t logged = 1;
    if (world != logged) {
        logged = world;
        std::uintptr_t camera_world = 0, global = 0;
        if (const std::uintptr_t controller = camera::controller()) {
            mem::try_copy(&camera_world, controller + 0x320, sizeof(camera_world));
        }
        mem::try_copy(&global, g_module.base + kEntityHelper, sizeof(global));
        log::info("gloves: client world 0x{:X} (camera controller's 0x{:X}{}, the exe+0x6007F68 global 0x{:X})", world,
                  camera_world, camera_world == world ? ", same" : ", DIFFERENT", global);
    }
    return world;
}

// 0x704BD0(world, handle) -> ecl::Item*, guarded: a fault in it is logged and treated as "no item".
std::uintptr_t entity_to_item_raw(std::uintptr_t world, std::uint64_t entity, bool& faulted) {
    __try {
        return fn<EntityToItemFn>(kEntityToItem)(world, entity);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        faulted = true;
        return 0;
    }
}

std::uint64_t g_lookup_faults = 0;

std::uintptr_t entity_to_item(std::uintptr_t world, std::uint64_t entity) {
    bool faulted = false;
    const std::uintptr_t item = entity_to_item_raw(world, entity, faulted);
    if (faulted && ++g_lookup_faults <= 20) {
        log::warn("gloves: the entity-to-item lookup faulted for entity 0x{:016X} in world 0x{:X} ({} so far)", entity, world,
                  g_lookup_faults);
    }
    return item;
}

// Can the controlled character take this item in the hand, by the game's own pick-up rule? The client PickUp task's
// checks without the carry-capacity one (a full pack mustn't stop the hand holding a cup): the pick-up flag, the
// CanBeIn component, not in an inventory or attached, no busy bits, and the weight within Strength x 5 kg (CanLift).
// Also whether it may be thrown (the game's CanThrowWeight: Strength^2 x 0.2 kg for the whole stack).
struct Liftable {
    bool grab = false;
    bool throwable = false;
    int grams = -1;
    int stack_grams = -1;
    int strength = -1;
    const char* why = "";
};
struct LiftCall {
    std::uintptr_t world = 0;
    std::uintptr_t item = 0;
    std::uint64_t handle = 0;
    std::uint64_t player = 0;
};

// The game calls, guarded (no C++ objects here: SEH).
int lift_calls(const LiftCall& in, Liftable& out) {
    __try {
        const std::uint64_t flags = *reinterpret_cast<const std::uint64_t*>(in.item + 0x18);
        const std::uint64_t parent = *reinterpret_cast<const std::uint64_t*>(in.item + kItemParent);
        const std::uintptr_t character = fn<CharacterOfFn>(kCharacterOf)(in.world, in.player);
        const bool can_be_in = fn<HasComponentFn>(kHasCanBeIn)(in.world, &in.handle);
        const std::uintptr_t ctx[5] = {in.world, in.world, *reinterpret_cast<const std::uintptr_t*>(in.world + 0x210),
                                       in.world + 0x198, in.world + 0x308};
        int grams = 0;
        fn<WeightFn>(kWeight)(&grams, ctx, &in.handle, 1, 1);
        out.grams = grams;
        // GetLiftingWeight 0x1BA9130 faulted on every item in the 12:44 run (reading 0x40030): not called. The throw
        // rule uses the one-unit weight.
        out.stack_grams = grams;
        const std::uintptr_t stats = fn<StatsFn>(kStats)(in.world, in.player);
        out.strength = stats ? *reinterpret_cast<const int*>(stats + 8) : -1;
        if (!(flags & kItemCanBePickedUp)) {
            out.why = "the game says it can't be picked up";
        } else if (!can_be_in) {
            out.why = "it can't go into an inventory";
        } else if (parent && parent != kNullHandle) {
            out.why = "it is in an inventory or attached";
        } else if (flags & kItemBusy) {
            out.why = "it is busy (the game refuses interactions)";
        } else if (!character) {
            out.why = "no controlled character";
        } else if (!fn<CanLiftFn>(kCanLift)(character, &grams)) {
            out.why = "too heavy for this character (Strength x 5 kg)";
        } else {
            out.grab = true;
            out.why = "liftable";
        }
        const float kg = static_cast<float>(out.stack_grams) / 1000.0f;
        out.throwable = out.grab && out.strength > 0 &&
                        static_cast<float>(out.strength * out.strength) * 0.2f >= kg;
        return 0;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return 1;
    }
}

bool g_lift_ready = false; // the lift functions matched this build
struct LiftVerdict {
    Liftable lift;
    double until = 0.0;
};
std::unordered_map<std::uint64_t, LiftVerdict> g_lift_cache;

double seconds_now();

const Liftable& liftable(std::uintptr_t item, std::uint64_t entity) {
    const double now = seconds_now();
    if (const auto it = g_lift_cache.find(entity); it != g_lift_cache.end() && it->second.until > now) {
        return it->second.lift;
    }
    if (g_lift_cache.size() > 512) {
        g_lift_cache.clear();
    }
    LiftVerdict& v = g_lift_cache[entity];
    v.until = now + 1.0;
    v.lift = {};
    LiftCall call;
    call.item = item;
    call.handle = entity;
    call.player = skeleton::controlled_character();
    call.world = client_world();
    if (!g_lift_ready) {
        v.lift.why = "the lift check doesn't match this build (refused to be safe)";
    } else if (!call.world || !call.player) {
        v.lift.why = "no client world or controlled character";
    } else if (lift_calls(call, v.lift) != 0) {
        v.lift = {};
        v.lift.why = "the lift check faulted";
    }
    static std::uint64_t logged = 0;
    if (logged++ < 400) {
        log::info("gloves: item 0x{:X} (entity 0x{:016X}): {}, {} g (stack {} g), Strength {}, {}", item, entity,
                  v.lift.grab ? "can be taken" : "can't be taken", v.lift.grams, v.lift.stack_grams, v.lift.strength,
                  v.lift.why);
    }
    return v.lift;
}

// The client item an entity handle from a ray hit belongs to (0 when it isn't one).
std::uintptr_t item_of(std::uint64_t entity) {
    if (entity == rayprobe::kNullEntity) {
        return 0;
    }
    const std::uintptr_t world = client_world();
    const std::uintptr_t item = world ? entity_to_item(world, entity) : 0;
    return item && still_item(item, entity) ? item : 0;
}

bool busy_item(std::uint64_t entity) {
    for (const auto& held : g_held) {
        if (held && held->entity == entity) {
            return true;
        }
    }
    for (const Flight& f : g_flights) {
        if (f.entity == entity) {
            return true;
        }
    }
    return false;
}

// One aim ray of the gloves: the item it hits first within the pull range, if any.
std::optional<Highlight> aim_ray(const vr::RigidPose& aim, const vr::Vec3& local_dir, float range) {
    const vr::Vec3 dir = vr::rotate(aim.orientation, local_dir);
    const vr::Vec3 from{aim.position[0] + dir[0] * kAimSkip, aim.position[1] + dir[1] * kAimSkip,
                        aim.position[2] + dir[2] * kAimSkip};
    const vr::Vec3 to{aim.position[0] + dir[0] * range, aim.position[1] + dir[1] * range, aim.position[2] + dir[2] * range};
    const auto hit = rayprobe::raycast(from, to);
    if (!hit) {
        return std::nullopt;
    }
    const std::uintptr_t item = item_of(hit->entity);
    if (!item || busy_item(hit->entity)) {
        return std::nullopt;
    }
    // Chests, barrels, furniture and anything else the character couldn't pick up don't fly: the game's own rule.
    const Liftable& lift = liftable(item, hit->entity);
    if (!lift.grab) {
        static std::uint64_t last_logged = 0;
        if (hit->entity != last_logged) {
            last_logged = hit->entity;
            log::info("gloves: item 0x{:X} not highlighted: {} (physics actor type {})", item, lift.why, hit->actor_type);
        }
        return std::nullopt;
    }
    return Highlight{item, hit->entity, hit->distance + kAimSkip, 0.0};
}

// Gloves, one hand per frame: the centre ray and one of the cone rays around it (all six every 6 scans of a hand).
// Reaching for something next to the hand: a short ray from the grip in one of six directions per scan (the hand's
// own axes), so an item the hand touches is highlighted even when the hand doesn't point at it.
std::optional<Highlight> touch_ray(std::size_t h, const vr::RigidPose& grip) {
    static const std::array<vr::Vec3, 6> kDirections{{{0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                                                      {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}};
    static std::array<int, 2> step{};
    const vr::Vec3 dir = vr::rotate(grip.orientation, kDirections[static_cast<std::size_t>(step[h])]);
    step[h] = (step[h] + 1) % static_cast<int>(kDirections.size());
    constexpr float kTouch = 0.22f;
    const vr::Vec3 to{grip.position[0] + dir[0] * kTouch, grip.position[1] + dir[1] * kTouch, grip.position[2] + dir[2] * kTouch};
    const auto hit = rayprobe::raycast(grip.position, to);
    if (!hit) {
        return std::nullopt;
    }
    const std::uintptr_t item = item_of(hit->entity);
    if (!item || busy_item(hit->entity) || !liftable(item, hit->entity).grab) {
        return std::nullopt;
    }
    return Highlight{item, hit->entity, hit->distance, 0.0};
}

void scan(std::size_t h, const HandInput& input) {
    const double now = seconds_now();
    std::optional<Highlight> found = input.grip_pose ? touch_ray(h, *input.grip_pose) : std::nullopt;
    // An item already touched stays highlighted while the touch rays go round (kHighlightHold), unless the aim finds
    // another one.
    if (!found) {
        found = aim_ray(*input.aim_pose, {0.0f, 0.0f, 1.0f}, g_pull_range);
    }
    if (!found) {
        const float a = kConeDegrees * 3.14159265f / 180.0f;
        const float phi = 2.0f * 3.14159265f * static_cast<float>(g_cone_step[h]) / static_cast<float>(kConeRays);
        g_cone_step[h] = (g_cone_step[h] + 1) % kConeRays;
        found = aim_ray(*input.aim_pose, {std::sin(a) * std::cos(phi), std::sin(a) * std::sin(phi), std::cos(a)}, g_pull_range);
    }
    std::optional<Highlight>& current = g_highlight[h];
    if (found) {
        found->seen = now;
        const bool is_new = !current || current->entity != found->entity;
        current = found;
        if (is_new) {
            ++g_highlights;
            buzz(h, 0.12f, 0.015f);
            const auto pose = item_pose(found->item);
            log::info("gloves: {} hand highlights item 0x{:X} (entity 0x{:016X}) {:.2f} m away{}", hand_name(h), found->item,
                      found->entity, found->distance,
                      pose ? std::format(" at ({:.2f}, {:.2f}, {:.2f})", pose->position[0], pose->position[1], pose->position[2])
                           : std::string());
        }
    } else if (current && (now - current->seen > kHighlightHold || !still_item(current->item, current->entity))) {
        log::info("gloves: {} hand no longer highlights item 0x{:X}", hand_name(h), current->item);
        current.reset();
    }
}

// The client item the hand's aim ray hits within `range`, logged.
std::optional<Resolved> aim_at_item(std::size_t h, const HandInput& input, float range) {
    const vr::Vec3 dir = vr::rotate(input.aim_pose->orientation, {0.0f, 0.0f, 1.0f});
    const vr::Vec3 from = input.aim_pose->position;
    const vr::Vec3 to{from[0] + dir[0] * range, from[1] + dir[1] * range, from[2] + dir[2] * range};
    const auto hit = rayprobe::raycast(from, to);
    if (!hit) {
        ++g_misses;
        log::info("grab: {} hand: nothing within {:.1f} m", hand_name(h), range);
        return std::nullopt;
    }
    const std::uintptr_t item = item_of(hit->entity);
    const bool is_item = item && still_item(item, hit->entity);
    const auto pose = is_item ? item_pose(item) : std::nullopt;
    log::info("grab: {} hand hit at {:.2f} m ({:.2f}, {:.2f}, {:.2f}), actor type {}, wrapper 0x{:X} {}, container 0x{:X}, "
              "entity 0x{:016X} -> item 0x{:X}{}{}",
              hand_name(h), hit->distance, hit->position[0], hit->position[1], hit->position[2], hit->actor_type,
              hit->wrapper,
              mem::describe(g_module, [&] {
                  std::uintptr_t vtable = 0;
                  mem::try_copy(&vtable, hit->wrapper, sizeof(vtable));
                  return vtable;
              }()),
              hit->container, hit->entity, item, is_item ? "" : " (not a client item)",
              pose ? std::format(" at ({:.2f}, {:.2f}, {:.2f})", pose->position[0], pose->position[1], pose->position[2])
                   : std::string());
    if (!is_item || !pose) {
        return std::nullopt;
    }
    return Resolved{item, hit->entity, *pose, hit->distance};
}

void take(std::size_t h, std::uintptr_t item, std::uint64_t entity, const vr::RigidPose& pose, const vr::RigidPose& hand,
          bool already_suspended) {
    if (!already_suspended) {
        fn<ItemFn>(kSuspend)(item);
    }
    const vr::Quat inverse = conjugate(hand.orientation);
    Held held;
    held.item = item;
    held.entity = entity;
    held.offset = {vr::rotate(inverse, minus(pose.position, hand.position)), vr::multiply(inverse, pose.orientation)};
    g_held[h] = held;
    ++g_grabs;
    log::info("grab: {} hand holds item 0x{:X} (offset ({:.2f}, {:.2f}, {:.2f}))", hand_name(h), item,
              held.offset.position[0], held.offset.position[1], held.offset.position[2]);
}

void press(std::size_t h, const HandInput& input) {
    if (!input.aim_pose || !input.grip_pose) {
        return;
    }
    // Closing the hand on an item flying to it: caught.
    for (std::size_t i = 0; g_hold && i < g_flights.size(); ++i) {
        Flight& f = g_flights[i];
        if (f.to_hand == static_cast<int>(h) && length(minus(f.position, input.grip_pose->position)) < g_catch_radius * 1.6f &&
            still_item(f.item, f.entity)) {
            take(h, f.item, f.entity, {f.position, f.rotation}, *input.grip_pose, true);
            ++g_catches;
            buzz(h, 0.8f, 0.08f);
            log::info("gloves: {} hand caught item 0x{:X} by closing on it", hand_name(h), f.item);
            g_flights.erase(g_flights.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
    // The highlighted item.
    if (g_hold && g_highlight[h] && still_item(g_highlight[h]->item, g_highlight[h]->entity)) {
        const Highlight target = *g_highlight[h];
        g_highlight[h].reset();
        const auto pose = item_pose(target.item);
        if (!pose) {
            log::warn("gloves: {} hand: the highlighted item 0x{:X} has no transform", hand_name(h), target.item);
            return;
        }
        const float distance = length(minus(pose->position, input.grip_pose->position));
        if (distance <= g_reach) {
            take(h, target.item, target.entity, *pose, *input.grip_pose, false);
            buzz(h, 0.5f, 0.04f);
        } else if (g_pull) {
            g_pull_target[h] = Target{target.item, target.entity};
            ++g_locks;
            buzz(h, 0.35f, 0.04f);
            log::info("gloves: {} hand locks on item 0x{:X} {:.1f} m away; flick the hand back to pull it", hand_name(h),
                      target.item, distance);
        }
        return;
    }
    if (g_hold) {
        return; // nothing highlighted: the grip keeps its binding (vrplay asks wants_grip())
    }
    const float range = g_reach;
    const auto target = aim_at_item(h, input, range);
    if (!g_hold || !target) {
        return;
    }
    if (target->distance <= g_reach) {
        take(h, target->item, target->entity, target->pose, *input.grip_pose, false);
    } else if (g_pull) {
        g_pull_target[h] = Target{target->item, target->entity};
        log::info("grab: {} hand aims at item 0x{:X} {:.1f} m away; flick the hand back to pull it", hand_name(h),
                  target->item, target->distance);
    }
}

void hold_frame(std::size_t h, const HandInput& input) {
    Held& held = *g_held[h];
    if (!still_item(held.item, held.entity)) {
        log::warn("grab: the held item 0x{:X} is gone; dropped without touching it", held.item);
        g_held[h].reset();
        return;
    }
    if (!input.grip_pose) {
        return; // tracking lost: keep it where it is
    }
    const vr::RigidPose& hand = *input.grip_pose;
    const vr::Vec3 offset = vr::rotate(hand.orientation, held.offset.position);
    place(held.item, {hand.position[0] + offset[0], hand.position[1] + offset[1], hand.position[2] + offset[2]},
          vr::multiply(hand.orientation, held.offset.orientation));
    ++held.frames;
}

void release(std::size_t h, bool in_pack) {
    Held held = *g_held[h];
    g_held[h].reset();
    if (!still_item(held.item, held.entity)) {
        log::warn("grab: the released item 0x{:X} is gone", held.item);
        return;
    }
    if (in_pack) {
        // Over a shoulder or behind the back: into the inventory. The item settles where it is first (physics back on,
        // pose to the server), then Lua asks the server to move it (Client/Holster.lua, Server/Holster.lua).
        settle(held.item, std::format("let go over the shoulder by the {} hand: stowing", hand_name(h)).c_str());
        bridge::push_request(std::format("stow 0x{:016X} {}", held.entity, hand_name(h)));
        ++g_stows;
        buzz(h, 0.5f, 0.05f);
        log::info("gloves: {} hand stows item 0x{:X} (entity 0x{:016X}) in the inventory", hand_name(h), held.item,
                  held.entity);
        return;
    }
    const auto velocity = g_trackers[h].velocity(0.1);
    const float speed = velocity ? length(velocity->linear) : 0.0f;
    const auto pose = item_pose(held.item);
    const bool throwable = liftable(held.item, held.entity).throwable;
    if (g_throw && speed >= g_throw_min_speed && pose && !throwable) {
        log::info("gloves: {} hand let go of item 0x{:X} mid-swing, but it is too heavy to throw (Strength^2 x 0.2 kg): "
                  "dropped", hand_name(h), held.item);
    }
    if (g_throw && speed >= g_throw_min_speed && pose && throwable) {
        g_flights.push_back({held.item, held.entity, pose->position, velocity->linear, pose->orientation, 0.0, -1});
        ++g_throws;
        log::info("grab: {} hand throws item 0x{:X} at {:.1f} m/s (after {} frames held)", hand_name(h), held.item, speed,
                  held.frames);
        return;
    }
    // Put down: it falls from the hand (with the hand's motion, if it may be thrown) onto the first surface below, so
    // lowering it onto a table or the ground and letting go leaves it there. The game's own physics isn't relied on
    // to drop a client-placed item.
    if (pose) {
        const vr::Vec3 drift = throwable && velocity ? velocity->linear : vr::Vec3{0.0f, 0.0f, 0.0f};
        g_flights.push_back({held.item, held.entity, pose->position, drift, pose->orientation, 0.0, -1});
        log::info("gloves: {} hand puts item 0x{:X} down (let go at ({:.2f}, {:.2f}, {:.2f}), {:.1f} m/s, after {} frames held)",
                  hand_name(h), held.item, pose->position[0], pose->position[1], pose->position[2], length(drift), held.frames);
        return;
    }
    settle(held.item, std::format("released by the {} hand after {} frames", hand_name(h), held.frames).c_str());
    report_move(held.entity, "released");
}

// A flick back toward the player while aiming at a far item: it flies on an arc into the hand.
void pull_frame(std::size_t h, const HandInput& input) {
    const Target target = *g_pull_target[h];
    if (!still_item(target.item, target.entity) || !input.grip_pose) {
        return;
    }
    const auto velocity = g_trackers[h].velocity(0.08);
    const auto pose = item_pose(target.item);
    if (!velocity || !pose || !grab::is_flick(input.grip_pose->position, velocity->linear, pose->position)) {
        return;
    }
    fn<ItemFn>(kSuspend)(target.item);
    buzz(h, 0.6f, 0.06f);
    const vr::Vec3 hand = input.grip_pose->position;
    const grab::Launch launch = grab::launch_velocity(pose->position, {hand[0], hand[1] + 0.1f, hand[2]}, kGravity, 0.3f);
    g_flights.push_back({target.item, target.entity, pose->position, launch.velocity, pose->orientation, 0.0,
                         static_cast<int>(h)});
    g_pull_target[h].reset();
    ++g_pulls;
    log::info("grab: {} hand pulls item 0x{:X} (flight {:.2f} s)", hand_name(h), target.item, launch.time);
}

// Moves every item in flight; lands it on the first solid thing in its path, or in a hand that is pulling it.
void flights_frame(double dt, const std::array<HandInput, 2>& input) {
    const std::uint64_t player = skeleton::controlled_character();
    for (std::size_t i = 0; i < g_flights.size();) {
        Flight& f = g_flights[i];
        bool done = false;
        if (!still_item(f.item, f.entity)) {
            log::warn("grab: an item in flight (0x{:X}) is gone", f.item);
            g_flights.erase(g_flights.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        f.age += dt;
        // A pulled item steers into its hand over the last stretch (the hand moved since the launch), keeping its speed
        // (at least 3 m/s) so it arrives instead of hanging in front of the hand.
        if (f.to_hand >= 0) {
            const HandInput& hand = input[static_cast<std::size_t>(f.to_hand)];
            if (hand.grip_pose) {
                const vr::Vec3 to_hand = minus(hand.grip_pose->position, f.position);
                const float d = length(to_hand);
                if (d < kHomingRange && d > 1e-3f) {
                    const float speed = std::max(3.0f, length(f.velocity));
                    const float blend = std::clamp(1.0f - d / kHomingRange, 0.25f, 0.9f);
                    for (std::size_t k = 0; k < 3; ++k) {
                        f.velocity[k] = f.velocity[k] * (1.0f - blend) + to_hand[k] / d * speed * blend;
                    }
                }
            }
        }
        const vr::Vec3 from = f.position;
        vr::Vec3 to{from[0] + f.velocity[0] * static_cast<float>(dt), from[1] + f.velocity[1] * static_cast<float>(dt),
                    from[2] + f.velocity[2] * static_cast<float>(dt)};
        f.velocity[1] -= kGravity * static_cast<float>(dt);

        // Caught by the pulling hand (grip still held, item close).
        if (f.to_hand >= 0) {
            const HandInput& hand = input[static_cast<std::size_t>(f.to_hand)];
            if (hand.grip && hand.grip_pose && !g_held[static_cast<std::size_t>(f.to_hand)] &&
                length(minus(to, hand.grip_pose->position)) < g_catch_radius) {
                take(static_cast<std::size_t>(f.to_hand), f.item, f.entity, {to, f.rotation}, *hand.grip_pose, true);
                ++g_catches;
                buzz(static_cast<std::size_t>(f.to_hand), 0.8f, 0.08f);
                log::info("gloves: {} hand caught item 0x{:X} after {:.2f} s in flight", hand_name(static_cast<std::size_t>(f.to_hand)),
                          f.item, f.age);
                g_flights.erase(g_flights.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        // Landing: the first solid thing between here and the next position (skipping the item's own shape). A pulled
        // item near its hand doesn't land, so the player's own body can't stop it short of the catch.
        bool near_hand = false;
        if (f.to_hand >= 0) {
            const HandInput& hand = input[static_cast<std::size_t>(f.to_hand)];
            near_hand = hand.grip_pose && length(minus(from, hand.grip_pose->position)) < 1.0f;
        }
        vr::Vec3 start = from;
        for (int attempt = 0; attempt < 3 && !done && !near_hand; ++attempt) {
            // Exclude 0x8 only: the ragdoll group 0x4004 shares bit 0x4 with the old 0xC, which hid every corpse (agent Q).
            const auto hit = rayprobe::raycast(start, to, 5, 0xFFFFFFFF, 0x8);
            if (!hit) {
                break;
            }
            // Its own shape, or the player's own body (an item let go at the chest would otherwise sit on it).
            if (hit->entity == f.entity || (hit->entity != rayprobe::kNullEntity && hit->entity == player)) {
                const vr::Vec3 d = minus(to, start);
                const float l = length(d);
                if (l < 1e-4f) {
                    break;
                }
                start = {hit->position[0] + d[0] / l * 0.05f, hit->position[1] + d[1] / l * 0.05f,
                         hit->position[2] + d[2] / l * 0.05f};
                continue;
            }
            to = {hit->position[0] + hit->normal[0] * 0.03f, hit->position[1] + hit->normal[1] * 0.03f,
                  hit->position[2] + hit->normal[2] * 0.03f};
            done = true;
        }
        f.position = to;
        place(f.item, f.position, f.rotation);
        if (done || f.age > kMaxFlight) {
            settle(f.item, done ? "landed" : "stopped (flight too long)");
            report_move(f.entity, f.to_hand >= 0 ? "pulled" : "thrown");
            g_flights.erase(g_flights.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

} // namespace

void install(const Config& config, const ModuleInfo& module) {
    g_module = module;
    g_probe = config.get_bool("grab", "probe", false);
    g_hold = config.get_bool("grab", "hold", true);
    const auto number = [&](const char* key, float fallback, float lo, float hi) {
        const std::string v = config.get("grab", key);
        return v.empty() ? fallback : std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
    };
    g_reach = number("reach", 1.5f, 0.2f, 10.0f);
    g_throw = config.get_bool("grab", "throw", true);
    g_throw_min_speed = number("throw_min_speed", 1.5f, 0.3f, 20.0f);
    g_pull = config.get_bool("grab", "pull", true);
    g_pull_range = number("pull_range", 6.0f, 1.0f, 30.0f);
    g_catch_radius = number("catch_radius", 0.35f, 0.1f, 1.5f);
    g_outline = config.get_bool("grab", "outline", true);
    // The functions are checked even while grabbing is off, so the VR menu can turn it on later.
    g_ready = check(kEntityToItem) && check(kSetTranslate) && check(kSetRotate) && check(kSuspend) && check(kResume) &&
              check(kSetFlags);
    if (g_ready) {
        install_outline();
    }
    g_lift_ready = check(kCharacterOf) && check(kHasCanBeIn) && check(kWeight) && check(kCanLift) && check(kStats);
    log::info("gloves: the game's pick-up rule {}", g_lift_ready ? "is used (flag, CanBeIn, not in an inventory, Strength x 5 kg)"
                                                                 : "doesn't match this build: nothing is taken");
    if (!g_probe && !g_hold) {
        health::set("grabbing", g_ready ? health::State::Off : health::State::Unavailable, g_ready ? "[grab] hold (or the VR menu)" : "a function didn't match");
        log::info("grab: off ([grab] probe / hold, or the VR menu){}", g_ready ? "" : "; UNAVAILABLE: a function didn't match");
        return;
    }
    health::set("grabbing", g_ready ? health::State::Ready : health::State::Unavailable, g_ready ? (g_hold ? "hold, throw, pull" : "probe") : "a function didn't match");
    log::info("grab: {} (reach {:.1f} m{}{}){}",
              g_hold ? "picking items up with the grips" : "probe only (logs what a grip hits)", g_reach,
              g_hold && g_throw ? std::format(", throws above {:.1f} m/s", g_throw_min_speed) : std::string(),
              g_hold && g_pull ? std::format(", pulls from {:.1f} m with a flick", g_pull_range) : std::string(),
              g_ready ? "" : "; UNAVAILABLE: a function didn't match");
}

bool enabled() {
    return g_ready && (g_probe || g_hold);
}

bool hold_on() {
    return g_ready && g_hold.load(std::memory_order_relaxed);
}

void set_hold(bool on) {
    if (g_hold.exchange(on) != on) {
        if (!on) {
            forget(); // let go of anything held
        }
        log::info("grab: picking things up {}{}", on ? "on" : "off", on && !g_ready ? " (UNAVAILABLE: a function didn't match)" : "");
    }
}

void set_hand(std::size_t hand, bool grip, std::optional<vr::RigidPose> grip_pose, std::optional<vr::RigidPose> aim_pose,
              double time_s, bool in_pack) {
    if (hand > 1) {
        return;
    }
    std::scoped_lock lock(g_mutex);
    g_input[hand] = {grip, grip_pose, aim_pose, time_s, in_pack};
}

Aim aim_state(std::size_t hand) {
    return hand > 1 ? Aim::None : static_cast<Aim>(g_aim_state[hand].load(std::memory_order_relaxed));
}

bool wants_grip(std::size_t hand) {
    return hand <= 1 && g_ready && g_hold.load(std::memory_order_relaxed) &&
           (aim_state(hand) != Aim::None || g_incoming[hand].load(std::memory_order_relaxed));
}

bool holding(std::size_t hand) {
    return hand <= 1 && g_holding[hand].load(std::memory_order_relaxed);
}

std::optional<Haptic> take_haptic(std::size_t hand) {
    if (hand > 1 || !g_haptic[hand].pending.exchange(false, std::memory_order_acquire)) {
        return std::nullopt;
    }
    return Haptic{g_haptic[hand].amplitude.load(std::memory_order_relaxed), g_haptic[hand].seconds.load(std::memory_order_relaxed)};
}

void on_frame() {
    if (!enabled()) {
        return;
    }
    if (g_forget.exchange(false, std::memory_order_relaxed)) {
        for (auto& held : g_held) {
            held.reset();
        }
        for (auto& target : g_pull_target) {
            target.reset();
        }
        for (auto& highlight : g_highlight) {
            highlight.reset();
        }
        g_flights.clear();
        g_was_down = {};
    }
    std::array<HandInput, 2> input;
    {
        std::scoped_lock lock(g_mutex);
        input = g_input;
    }
    const auto now = std::chrono::steady_clock::now();
    const double dt = g_last_frame.time_since_epoch().count() == 0
                          ? 0.0
                          : std::min(0.1, std::chrono::duration<double>(now - g_last_frame).count());
    g_last_frame = now;
    for (std::size_t h = 0; h < 2; ++h) {
        if (input[h].grip_pose && input[h].time > g_tracked_time[h]) {
            g_trackers[h].add(input[h].time, *input[h].grip_pose);
            g_tracked_time[h] = input[h].time;
        }
        // corpse::on_frame ran first this frame: a grip that took a body is that body's, not an item press.
        const bool down = input[h].grip && !corpse::holding(h);
        // Gloves: a free, open hand looks for items (one hand per frame; a hand that closed this frame uses what it
        // had highlighted).
        const bool free_hand = !g_held[h] && !g_pull_target[h] && !down && !corpse::holding(h);
        if (g_hold && g_pull && free_hand && input[h].aim_pose && g_scan_frame % 2 == h) {
            scan(h, input[h]);
        } else if (!free_hand && !down) {
            g_highlight[h].reset();
        }
        if (down && !g_was_down[h] && !g_held[h]) {
            press(h, input[h]);
        } else if (down && g_held[h]) {
            hold_frame(h, input[h]);
        } else if (down && g_pull_target[h]) {
            pull_frame(h, input[h]);
        } else if (!down) {
            if (g_held[h]) {
                release(h, input[h].in_pack);
            }
            if (g_pull_target[h]) {
                log::info("gloves: {} hand let go of the lock on item 0x{:X} without pulling", hand_name(h), g_pull_target[h]->item);
            }
            g_pull_target[h].reset();
        }
        g_was_down[h] = down;
    }
    ++g_scan_frame;
    if (dt > 0.0 && !g_flights.empty()) {
        flights_frame(dt, input);
    }
    // What the hands are doing, for the laser, the grip binding and the holsters (other threads).
    for (std::size_t h = 0; h < 2; ++h) {
        const Aim state = g_held[h] ? Aim::Holding : g_pull_target[h] ? Aim::Locked : g_highlight[h] ? Aim::Highlight : Aim::None;
        g_aim_state[h].store(static_cast<int>(state), std::memory_order_relaxed);
        g_aim_entity[h].store(state == Aim::Locked      ? g_pull_target[h]->entity
                              : state == Aim::Highlight ? g_highlight[h]->entity
                                                        : 0,
                              std::memory_order_relaxed);
        g_holding[h].store(g_held[h].has_value(), std::memory_order_relaxed);
        bool incoming = false;
        for (const Flight& f : g_flights) {
            incoming = incoming || f.to_hand == static_cast<int>(h);
        }
        g_incoming[h].store(incoming, std::memory_order_relaxed);
    }
}

void forget() {
    g_forget.store(true, std::memory_order_relaxed);
    for (std::size_t h = 0; h < 2; ++h) {
        g_aim_state[h].store(static_cast<int>(Aim::None), std::memory_order_relaxed);
        g_aim_entity[h].store(0, std::memory_order_relaxed);
        g_holding[h].store(false, std::memory_order_relaxed);
        g_incoming[h].store(false, std::memory_order_relaxed);
    }
}

void report() {
    log::info("grab: {}, {} grabs, {} throws, {} pulls ({} caught), {} misses; gloves: {} highlights, {} locks, {} stowed",
              enabled() ? (g_hold ? "hold" : "probe") : "off", g_grabs, g_throws, g_pulls, g_catches, g_misses, g_highlights,
              g_locks, g_stows);
    log::info("gloves: {} outline requests pushed", g_outline_pushes.load(std::memory_order_relaxed));
}

} // namespace bg3vr::hold
