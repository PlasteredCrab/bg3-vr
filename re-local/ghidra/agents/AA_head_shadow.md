# Hidden first-person head still casting its shadow - agent AA, 2026-10-23

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_G, read-only, no analysis pass, headless, request
server `agents\AA\AAServer.java`; disassembly with bg3vr_disasm). RVAs are relative to 0x140000000. Raw output is in
`re-local\ghidra\agents\AA\` (aa1..aa13, helper scripts `riprefs.py`, `rendvt.py`, `scanf0.py`). Earlier decompiles
of other agents are cited as H/dec_*.c, T/t5_skin.c. The mod's own log (`bin\bg3vr\bg3vr.log`, 2026-10-23 10:49)
is used only for the flag values it printed. "Confirmed" = read in code at the cited address. "Unconfirmed" = the
code read does not settle it; section 5 says how one run settles it.

## Fix for the VR mod

| what | RVA / hook | how | risk | confidence |
|------|-----------|-----|------|------------|
| **Hide the head from the camera passes only: clear cull bit 0x0001 instead of 0x0600** | setter **0x3C6B810**`(Visual* v, u16 mask, bool set)` (free function, not virtual; recurses into attachments, notifies the scene) | Hide: `0x3C6B810(v, 0x0001, false)`. Leave 0x0600 set; when switching from the current build, first call slot 15 `v->vf[0x78](v, true)` once to give 0x600 back. Re-hide check in `cull::on_frame`: `if (flags & 0x0001)` instead of `& 0x600`. | low. Bit 0x01 is the engine's own "not a shadow proxy" bit: 0x3C64730 clears it for visuals with VisualFlags `IsShadowProxy` (0x40), which is how the game makes shadow-only meshes. Only the camera cull (CullStage, mask 0x201/0x401/0x1) requires it. Every shadow cull requires the view bit 0x200 instead, so the head stays in all of them. | Mechanism Confirmed (code). On-screen result Unconfirmed (one run, 5.1) |
| Show again | slot 23 **0x3C64730** (`v->vf[0xB8](v)`, UpdateCullFlags) on the visual and each attachment (`[v+0xB8]`, stride 0x30, count `[v+0xC4]`, recursively) | Recompute rather than set 0x01 blindly: slot 23 sets 0x01 only when `IsShadowProxy` is clear, so a real shadow-proxy attachment stays a proxy. Slot 23 does not touch 0x200/0x400. | low | Confirmed |
| Keep it hidden when the game recomputes the flags | **0x3C64730** (post-hook), or the existing 32-frame poll | 0x3C64730 rewrites bit 0x01 from VisualFlags each time it runs (it never touched 0x600, which is why the current hide was never undone by it). A post-hook that re-clears 0x01 for visuals in the hidden set makes it immediate. The poll alone allows up to 32 frames of visible head after each recompute. | low (post-hook runs on whatever thread calls slot 23; take the mod's mutex, only compare pointers) | Writers Confirmed (aa12: for the Visual class only 0x3C625E0 ctor, 0x3C64730, 0x3C6B810, subclass ctor 0x3ECBF00). How often 0x3C64730 runs at run time: Unconfirmed (5.2) |
| Not recommended: (b) strip 0x200 from the shadow queries | 0x3F7D370 (cascades), 0x3E00310 (local lights), 0x3F7DFD0 (distant cache) | Would make every per-view-hidden object cast shadows in that view, for all objects and both split-screen players, not just the head. | medium (global) | masks Confirmed |
| Not recommended: set VisualFlags `IsShadowProxy` (0x40 at `[v+0xF0]`) and call slot 23 | 0x3C64730 | Gives the same cull bits, but other readers of VisualFlags 0x40 are not audited (no `test byte [r+0xF0],0x40` exists; readers use shifts, scanf0.py), and the game may rewrite `+0xF0`. | medium | Unconfirmed side effects |

Answer in one line: the shadow is missing because **every shadow caster cull requires the split-view bit 0x200**,
the same bit the mod clears. The camera cull is the only one that requires bit **0x0001**, so clearing 0x0001
(and keeping 0x200) gives exactly "invisible to both eye cameras, still casting sun, local-light and
distant-cache shadows".

## 1. How a scene cull query tests the flags (Confirmed)

* Query layout (from the builders 0x3CEE530 / 0x3CEE3D0 (aa2) and the users below): `+0x28` camera position,
  `+0x34` type (1 frustum, 2 sphere), **`+0x38` u16 required-flags mask**, `+0x3C` per-query options,
  `+0x50..` up to 17 planes, `+0x168` render view.
* Scene cull 0x3CD1E80 runs 0x3CD0FA0 per object type, always with "excluded mask" = 0 (H/dec_cull1.c, every
  call passes `local_438 = 0`). In 0x3CD0FA0 the test is a call through table 0x58D7070 at index
  `(mask != 0) | (excluded != 0) << 2` (H/dec_lod2.c 1007-1016; table read from the file). Index 1 is
  **0x3F1E940** (aa3): `culled[i] |= (flags[i] & mask) != mask`. So **the query mask lists bits an object must
  have, all of them**. The other table entries (0x3F1E9B0.. 0x3F1EC40) add "any of" and "none of" masks that no
  caller uses. Index 0 is a `ret` stub.
* The per-object flags array is the scene's `scene + class*400 + 0x1D0` u32 array, indexed by the object's scene
  slot. For a Visual it is written by 0x3CD2F20 with `[visual+0xF4]` verbatim (r8 = the new flags at the call in
  0x3C6B810, disassembly 0x3C6B82B..0x3C6B85F, and in 0x3C64730). 0x3C64830 then writes each ObjectDesc
  renderable (`[visual+0x98]`, stride 0x10) with the renderable's GetCullFlags (vtable +0x68). All renderable
  vtables use **0x3C60740** there (rendvt.py: 11 vtables with base slot 2 0x3C5FFD0), which returns
  `parent_visual->GetCullFlags() & 0xFFBF` (or 0x86BF with no parent; disassembly). So head meshes carry the
  visual's bits minus 0x40.
* So the mod's "hide" is an inclusion mask (cull.hpp already says so): the head visuals log `flags 0x86FF`
  visible and `0x80FF` hidden (bg3vr.log lines 223, 16863).

## 2. Every query and its required bits (Confirmed)

All callers of the scene cull 0x3CD1E80 (aa1): 0x3E246B0, 0x3E00310, 0x3F79E80, 0x3E51DE0 (x2), 0x3F7D370,
0x3F7DFD0. All read the split index `[view+0x2268]` (R/r16 list) and add 0x200 (index 0), 0x400 (index 1) or
nothing (other).

| pass | function (object) | required mask, view index 0 / 1 / other | pass bit | evidence |
|------|-------------------|------------------------------------------|----------|----------|
| **Camera** (CullStage 0x08, all colour/depth/outline buckets) | 0x3E246B0, frustum via 0x3CEE3D0 | **0x201** / 0x401 / 0x001 | 0x0001 | H/dec_cullstage.c 80-88 (`uStack_170` = query+0x38) |
| **Sun cascade i** (stage 0x02) | job 0x3F7D300 -> **0x3F7D370**, cascade cull objects `stage02+0x2180+i*8` (ctor 0x3F7CD90, `+0x1F4` = i, written at 0x3E19FAB) | **0x200 \| {0x02, 0x04, 0x08, 0x10}[i]** / 0x400 \| .. / bit only | 0x02, 0x04, 0x08, 0x10 | aa5 lines 213-245; the job is `object+0xD0` (vtable 0x58DC638, slot 2 = 0x3F7D300), submitted by 0x3E1B0C0 (aa8 230-235) |
| **Distant shadow cache** (stage 0x02) | job 0x3F7E2F0 -> **0x3F7DFD0**, object `stage02+0x2460` (made in 0x3E19910 at 0x3E19DA3, +0xD0 vtable 0x58DC738) | **0x8200** / 0x8400 / 0x8000 | 0x8000 | aa5 lines 441-449; submitted by 0x3E1B0C0 when `[[view+0x10]+0x40] == 0` and `+0x241B` (aa8 241-247) |
| **Local lights** (stage 0x1E, one job per light) | **0x3E00310** (vtable 0x58C4A50 slot 2; objects built by 0x3DFFFF0 from 0x1E's per-frame 0x3E01710, kept in `stage1E+0x2290`) | **0x220** / 0x420 / 0x020 | 0x0020 | aa7 197-209: point lights (`light+0xD3 == 0`) sphere query of radius `light+0xB8`, split into the 4 tiles; spot/other lights go through 0x3F79E80 with the same mask (aa5 51-58) |
| Texture streaming (stage 0x3E) | 0x3E51DE0, sphere 40 m | 0x240 / 0x440 / 0x040 | 0x0040 (visual only; renderables drop it) | H 4.2 |

Correction to C 2.2: 0x3E00310 is **not** the main-scene gather. It is stage 0x1E's per-light shadow caster cull
(vtable entry 0x58C4A60 next to 0x1E's 0x58C49C0; ctor caller 0x3E01710; teardown 0x3E03590 walks
`stage+0x2290`). The camera gather is CullStage 0x3E246B0 with 0x201/0x401/0x1, as H and N already noted.

How the bits are set (0x3C64730, slot 23, disassembly in full): `0x0001 = !(VisualFlags & 0x40 IsShadowProxy)`;
`0x803E (= 0x02|0x04|0x08|0x10|0x20|0x8000) = VisualFlags & 0x01 CastShadow`; `0x0080 = !IsShadowProxy &&
ReceiveDecal`; `0x0800 = IsScenery`; all other bits (0x40, 0x200, 0x400, ...) are kept. The base ctor 0x3C625E0
and the subclass ctor 0x3ECBF00 compute the same. 0x3DE5690 is the equivalent for another class (no 0x01).

### 2.1 Answer to question 1

* Every shadow pass (all 4 cascades, every local light, the distant cache) **requires** the view bit. Clearing
  0x200 removes the head from all of them. That is the reported bug, explained by code.
* The camera pass requires bit 0x0001; no shadow pass does. Bits 0x02..0x20 and 0x8000 are required only by the
  shadow passes. So:
  * camera only, keep shadows: clear **0x0001** (the engine's shadow-proxy state);
  * shadows only, keep camera: clear 0x803E (the engine's "CastShadow off" state).
* SE's names for these bits (CullNonProxy, CullShadow..., CullRenderView1/2) describe the bits as exclusions.
  In this build they are inclusion bits, and SE has no name for 0x0001.

### 2.2 Answer to question 2

(a) is the fix: bit 0x0001, the shadow-proxy path the engine already has (option (c) of the brief is the same
mechanism: "IsShadowProxy" visuals are the engine's hidden-but-shadowing objects). (b) works but is global (table).
Nothing in the camera path needs the head once it is out of CullStage. The decal-receive bit 0x80 also follows
`!IsShadowProxy` in 0x3C64730, so the hidden head (0x80 left as is by 0x3C6B810) can still receive decals (not
visible anyway).

Side effects of clearing 0x0001 (Confirmed from the mask table): the head also leaves the camera pass of views
with split index >= 2 (mask 0x1). The 3D UI dolls render private scenes with clones (P report), so they are not
affected. Texture streaming (0x240) keeps requesting the head's textures, unlike today's 0x600 hide.

## 3. Skinning of a shadow-only head (question 3) (Confirmed)

0x41's job 0x3E15670 (T/t5_skin.c 486-640) builds its object list from:
1. the view's CullStage result (`job+0x2110`),
2. stage 0x1E (`job+0x2150`), if its `+0x18` run flag is set: for each light entry with record index
   `+0x320 >= 0`, the per-light cull result `[[stage1E+0x2290][i]] + 0x20` (the 0x3F79C00 object of 0x3DFFFF0),
3. stage 0x02 (`job+0x2158`), if it runs and the sun is on (`[[view+0x28A0]+0x50]`): the cascade objects
   `+0x2180..` (count `+0x2688`) and, when `[[view+0x10]+0x40] == 0`, the distant-cache object `+0x2460`.

It de-duplicates the objects of their `+0x90` / `+0xA0` lists and skins the union. So a head that is out of the
camera result but in any cascade, light or cache result **is skinned in the same view and frame**. When it is in
none of them it is drawn nowhere, so no skinning is needed. The CPU side (skeleton -> mesh-binding matrices)
does not depend on visibility: the finish step 0x3BD6670 rebuilds every mesh binding of the skeleton (C 1.3), and
the head shares the body's skeleton. Off-screen shadow casters (a character behind the camera) take the same path
every frame in the unmodded game.

Interaction with T fix #1 (eye reuses main's cascades, eye's 0x02 off): the eye's 0x41 then skips the cascade
casters, but the eye also draws no cascades. Main's cascade draws use main's 0x41 output, which includes the
head. No conflict.

## 4. Notes for the implementation (cull.cpp)

* `layout::kMainViewBits` (0x600) becomes 0x0001 for the hidden test. `set_main_view` calls 0x3C6B810 by RVA with
  `(object, 0x0001, visible)` for hiding. Showing calls slot 23 per visual of the tree (table). The code check in
  `check()` (looks for 0x600 / ~0x600 in slot 15) needs a matching check for 0x3C6B810: its first bytes
  `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 41 0F B6 E8 0F B7 F2` and
  `66 44 89 81 F4 00 00 00` at +0x3B.
* Migration: visuals hidden by an older build have 0x600 cleared. Give it back once with slot 15 `(v, true)`.
* Threading: 0x3C6B810 and 0x3C64730 write the scene flags under the scene's SRW lock (0x3CD2F20 takes it). The
  mod already calls slot 15 from `on_frame`. Same thread, same lock.

## 5. One-run logging plan (for the Unconfirmed items)

All in one session: hide on, walk outdoors in sunlight, then near a torch, then equip/unequip a helmet.

5.1 **The head is in the shadow casters and out of the camera list.** Once per second, for each hidden visual:
`[v+0xF4]` (expect 0x86FE), each ObjectDesc renderable's `vf[0x68]` flags (expect 0x86BE). Hook 0x3CD1E80
entry and return: log `[query+0x38]`, the return address (identifies the caller: 0x3E24932 CullStage,
0x3F7D78A cascade, 0x3F7E2A8 cache, 0x3E009F8 / 0x3F7A0A9 lights) and the view. After 0x3F7D370 returns for each
cascade object `stage02+0x2180+i*8`, scan its `+0x90` (count `+0x9C`) and `+0xA0` (count `+0xAC`) lists for the
head renderables' pointers and log hit/miss per cascade (which of the two lists holds renderables is
Unconfirmed: log both). Do the same for the CullStage buckets (expect miss). Expected: hits in the cascades that
cover the player, miss in the camera buckets. Visual check: the head's shadow is on the ground and follows HMD
nods (skinned).

5.2 **Who resets bit 0x01.** Post-hook 0x3C64730: log return address, visual, flags before/after, frame, for
visuals in the hidden set. Expected: only on equipment/visual changes. If it fires every frame, the post-hook
(not the poll) is required.

5.3 **Local lights and the distant cache.** Near a shadow-casting torch, at 0x3E00310 exit, scan the per-light
result object `[job+0x20]` lists for the head renderables (as 5.1). At 0x3F7E2F0, log whether the cache cull runs
(it may run only on cache updates) and whether the head is in its result.

5.4 **No new artefacts.** Log nothing; check by eye that no head pieces (hair cards, beard, helmet) remain in
either eye. Attachments are covered by the 0x3C6B810 recursion. A head part that is an ObjectDesc of the body
visual (C 2.3, open) would be hidden together with the body by this mechanism, so check the body stays visible.

## Raw files (agents\AA\)

aa1 callers of the query builders / scene cull / per-type cull; aa2 0x3CEE530, 0x3CEE3D0, 0x3C64730, 0x3C6B810;
aa3 mask-test table functions 0x3F1E940..0x3F1EC40; aa4/aa6 callers of the cull functions and wrappers; aa5
0x3F79E80, 0x3F7D370, 0x3F7DFD0, 0x3F7A700, 0x3CD2F20; aa7 0x3E00310, 0x3DFFFF0, 0x3F7D300, 0x3F7E2F0; aa8 0x3E1B0C0;
aa9/aa10/aa13 vtable and ctor references; aa11 0x3ECBF00, 0x3C64830, 0x3CD3280; aa12 every `+0xF4` operand.
