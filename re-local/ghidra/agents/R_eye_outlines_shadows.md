# One-eye outlines (and a note on eye shadows) - agent R, 2026-10-20

Static analysis of bg3_dx11.exe 4.1.1.7631656 (Ghidra project BG3_A, read-only, headless, request server
`agents\R\RServer.java`) plus the mod's log `bin\bg3vr\bg3vr.log` of 2026-10-20 14:01. RVAs are relative to
0x140000000. Raw output is in `re-local\ghidra\agents\R\` (r1..r30). Problem 2 (shadows) was dropped by the user
(the "artifact" turned out to be blood decals). Only the one confirmed shadow finding is kept, in section 3.

## Hook points for the VR mod

| need | RVA | how to use | confidence |
|------|-----|-----------|------------|
| Give the eye view the main view's player-room data (the only per-player-view input the outline draw reads) | existing hook **0x3CC6CB9** (eye view's begin-recording, render thread), or `on_frame` on the game thread | Every frame copy main to eye: `+0x2BE8..+0x2BF7` (player character handle, 16 B), `+0x2BF8` (int room id), `+0x2BFC` (byte), `+0x2BFD` (byte), `+0x2AEA` (byte). The eye's stage 0x1F job runs after its begin-recording (counters are released only after every view's begin-recording, A 2), so it sees the copy. | Mechanism Confirmed (0x17E3320, 0x17E7FD0, 0x17F1E60). That it's the observed cause: Unconfirmed (log below) |
| Defensive: same input-slot byte | view `+0x2258` (WriteState+0x78, copied to ReadState `+0x21D0` by 0x4072CB0) | Copy main's `+0x2258` into the eye's. Both are 0xFF in single player, so this changes nothing today. It only matters if split-screen mode 3 ever writes it. | Confirmed (0x407B080, 0x1E14130, 0x1E20670, 0x17DC550) |
| Room filter decision per draw (logging) | **0x17E3320**(rcx = level render object `[level+0xB8]`, rdx = view, r8 = object position) returns al | Count calls and false results per view (rdx). False means the outline of that object was dropped for that view. | Confirmed (code) |
| Outline object job (logging) | **0x17DB570** (rcx = stage 0x1F + 0x20) | At entry: view = `[rcx-0x18]`, CullStage = `[rcx+0x2110]`, outline object count = `[[rcx+0x2110]+0x2164]`. If it never runs for the eye, the problem is the gate. If it runs with count > 0, the problem is per-draw (the room filter). | Confirmed (code) |
| Outline post step gate (logging) | 0x17FCF10 (prepare), step object = the entry in stage 0x21's step list (+0x2150, count +0x215C) whose vtable is exe+0x55F4900 | Log step `+0x18` (runs), `+0x110` (TAA variant), `+0x60/+0x64` (input/output chain keys) for both views | Confirmed (code) |

## 1. Problem 1: what the outline path reads per view

Path (Confirmed): CullStage job 0x3E24970 fills `m_OutlineObjects` (CullStage+0x2150, count +0x2164). Stage 0x1F job
0x17DB570 draws those objects into the mask 0x5AE0F2DF (0x17DC550 single draws, 0x17DC960 + 0x17DBF00 instanced).
The outline post step (vtable 0x55F4900, job 0x17FCFB0) runs BlurH -> 0x1C84A002, BlurV -> 0x513E5C20, then Combine
into the chain output. EoCOutlineTAA runs only when `[DAT_14604AA10+0x80] == 2` (0x17FE400). The log shows aa 3, so
the TAA branch and its shared history keys 0x1C84A002/0xDC7D4A87 are not involved.

### 1.1 Leads from the brief that are ruled out (Confirmed)

(a) Per-player index. The CullStage job reads `[view+0x2268]` live every frame (0x3E24B7B in 0x3E24970: `local_278 =
view+0x2268`, block = `0x1817500(world)+index*0x30`, passed to sub-jobs at +0x38/+0x40). The cull parameters do the
same (0x3E24798 in 0x3E246B0). 0x1817500 returns an ECS singleton component (0x18187B0). Its per-player 0x30-byte
hash set is looked up in 0x3E21C70. When it hits, item+0x18 = the entity's outline entry list (0x3E280D0), and
0x3E220C0 pushes the item into `m_OutlineObjects` (cull+0x2150) iff item+0x18 != 0. The eye renders with index 0
(views.cpp `g_split_index_after_build`, log "0 for rendering"), so it fills its bucket from the same block[0] as the
main view. No outline object caches the index at construction: stage 0x1F ctor 0x17DA740 and init 0x17DAA70, and the
outline step ctor 0x17FCD00 and init 0x17FCEE0/0x17FE400/0x17FE800/0x17FEB40 never read +0x2268. A full displacement
scan for +0x2268 (r16) shows no reader in 0x17Dxxxx-0x17Fxxxx other than stage 0x3A's job 0x17DE2A0, which is
per-frame.

(b) The stage 0x1F prepare 0x17DB4C0 (disassembly r: rdx = 0 at the call): gate 0x17DA690 (init ok, ReadState.Enabled,
frame+0x42F0, slot 13, scene, global flag, controller), `[view+0x2B10]+0x48 == [DAT_146007A28]+0x90`,
`stage+0x2140 = [level+0xB8]`, `[DAT_14604B1B8+0xE]`, and 0x17DD430(world, 0). 0x17DD430 is a world-global ECS scan
(any entity with +0x1EC != 0). The eye's scene pointer is the main view's (log 14:01:53.838 "level scene
0x27C1990A7C0 (main view's 0x27C1990A7C0)"), and the controller is shared. Identical for both views.

(c) The outline post step exists for the eye: 0x588E50 attaches 0x180F4B0 and 0x17FCD00 to every non-special view's
stage 0x21. Its gate 0x17FCF10 needs `[view+0x2961] == 0` (0 on both, from the log), the same level check,
`[switches+0xE]` and 0x17DD430. The base prepare 0x3CF0BA0 ANDs with stage 0x21's +0x18. The only view-specific
branch there is for the last step (`step+8 == [DAT_146046F98+0xA0]` selects load action 0 or 2 for 0x684EE890). It
does not gate the outline step.

(d) Shared keys. The mask is cleared and drawn per view rectangle (ClearRTCompute over the 0x407B8C0 rect, viewport
0x40814F0). Every blur and combine pass uses the view's viewport CB (b9) and clamps UVs to cb9[4]
(EoCOutlineBlurH.asm). The whole-RT clear on an empty bucket (0x4097F70 + 0x40C9690) comes after the main view has
consumed the mask, because views execute in order (A 1.5; the log's gpu trace shows main's 0x1F/0x33/0x21 before the
SPLIT ones). The TAA history is unused at aa 3. The object shaders (N shaders_asm) read only cb0/cb1/cb10/cb11,
t0, t30 = linear depth 0xFF193183 (disassembly 0x17DB6D1) and t126. There is no player or view data in them.

Colour selection 0x17DC550 (Confirmed): `slot = (s8)[view+0x21D0]` (ReadState+0x78). For slot -1 it draws each
object with its first outline entry. Otherwise it maps slot to player id (player manager map +0x70) and draws only
entries whose player id (+0xC) matches. The root ctor 0x407B080 sets +0x21D0/+0x2258 = 0xFF. Only the split-screen
mode-change handler 0x1E14130 writes it: mode 3 per player through 0x1E20670 (slot or 0xFF), otherwise 0xFF to the
primary player's view. View slot 12 = 0x3CCCB10 is a plain setter. In single player both views are 0xFF, so the
selection is the same.

### 1.2 The per-view input that differs: the room filter (mechanism Confirmed)

0x17DC550 (0x17DC749) and 0x17DC960 (0x17DC9BB) call **0x17E3320(levelObj = stage+0x2140 = [level+0xB8], view,
object pos)** whenever `[levelObj+0x250] != 0`:
```
room = byte from levelObj+0x48 grid (0x1441F49A0(grid, 3, cell(pos))), 0 if not found
return [view+0x2BF8] == room          // false -> no outline draw for this object in this view
```
`[view+0x2BF8]` is written only by 0x17E7FD0 (with +0x2BFC, +0x2AEA, handle +0x2BE8..+0x2BF4). That function writes
the view returned by `GetPlayerView(player)` 0x3BAAE20 from the player character's room component
(`[char+0x100]+0x7A`, `[char+0x108]`). 0x17E00D0 sets +0x2BFD/+0x2AEA the same way. The created eye view is never
registered in the player manager (B 3.2; N 4), so these stay at the GameRenderView ctor values (0x17F1E60:
+0x2BE8 = null handle, +0x2BF8 = 0, +0x2BFC/+0x2BFD = 0). Effect: while the player stands in a room with id != 0
(building interiors and floors) on a level whose render object has +0x250 set, the main view outlines objects in the
player's room. The eye view outlines only room-0 objects, so in practice nothing that's indoors. Outdoors (room 0)
both agree. 0x17E32D0 applies the same filter for game code through the primary player's view, so hover picking
follows the main view.

Status: this is the only per-player-view state on the outline draw path (Confirmed by the reads above). The log's
field diff (14:01:53 / 14:01:56) shows +0x2BF8 equal in both views at that moment (it isn't in the "set in main, zero
in split" list, while +0x2BF0 is). So the player was in room 0 then. Whether the headset footage was taken in a
non-zero room is Unconfirmed. The log below settles it in one run.

### 1.3 Fix (minimal, per frame)

In the existing 0x3CC6CB9 hook when `view == eye` (stereo.cpp `on_camera_built`, next to the 0x2961 copy), or on the
game thread each frame:
```
memcpy(eye+0x2BE8, main+0x2BE8, 0x18);   // handle 16 B, room id +0x2BF8, +0x2BFC, +0x2BFD
eye[0x2AEA] = main[0x2AEA];
eye[0x2258] = main[0x2258];               // defensive (both 0xFF today)
```
Keep the existing `+0x2961` copy. Don't call 0x17E7FD0 for the eye (it looks the view up through 0x3BAAE20). If the log
shows the eye's stage 0x1F job running with a non-empty bucket, `0x17E3320 == false` for the eye, and outlines still
missing after this copy, the cause is downstream (GPU). Then use the gpu trace on a hovered frame (see 1.4).

### 1.4 One-run confirmation log (hover an item and a character, indoors and outdoors, hold left-alt)

Each frame while `[[DAT_145FDA240+0x1A8]]`'s 0x17DD430 is true, or at least every 10th frame, for main and eye:
1. In the 0x3CC6CB9 hook: view, `+0x2268`, `+0x21D0`, `+0x2258`, `+0x2961`, `+0x2BF8`, `+0x2BFC`, `+0x2BFD`, `+0x2AEA`,
   handle `+0x2BE8/+0x2BF0`; level = `[[0x146007A28]+0x90]`, L = `[level+0xB8]`, `[L+0x250]`; `[view+0x2B10]` and its
   +0x48.
2. Stage 0x1F (stage array view+0x28, count +0x34, slot 1 returns 0x1F): `+0x18` (runs), `+0x19` (init ok), `+0x2140`,
   `+0x2130` (CullStage). At job entry 0x17DB570 (rcx = stage+0x20): view `[rcx-0x18]`, count
   `[[rcx+0x2110]+0x2164]`.
3. 0x17E3320 (detour): per view (rdx) the number of calls and of false returns, plus the first object's room byte
   (the decision is `[rdx+0x2BF8] == room`).
4. Outline step (stage 0x21 list +0x2150/+0x215C, vtable exe+0x55F4900): `+0x18`, `+0x110`, `+0x60`, `+0x64`.
5. `[DAT_14604AA10+0x80]` (aa mode) once.
6. One gpu trace (the mod's `gpu:` dump) on a hovered frame. Expect a draw into the mask (format 0x5120002 RT) under
   "stage 0x1F of view ... (SPLIT)", and BlurH/BlurV/Combine draws in its stage 0x21.

Prediction if 1.2 is the cause: the eye's 0x1F runs with count = main's count, main `+0x2BF8 != 0`, eye 0, `[L+0x250]`
!= 0, and 0x17E3320 false for the eye on every indoor object. With the copy: equal counts and no false results.

## 2. Other per-view differences seen (not on the outline path)

* Focus / see-through data: `+0x2C00..+0x2C3F` (focus CB b7, built by 0x17F2B80 from `+0x2CC8` own player and
  `+0x2D00` other) is zero in the eye (log field diff 14:01:56). 0x17F2230 fills the source only for the player's own
  view. Wall/roof see-through can therefore differ per eye. Fix if seen: copy main `+0x2CC8..+0x2D87` into the eye
  before the eye's 0x17F2B80 (a hook there, rcx = view).
* Stage 0x21's first output object (view+0x2BE0, 0x180F4B0) gets room data from 0x17E00D0/0x17E2AA0 only for player
  views. The eye's copy gets none (room-related post effect; not decompiled further).

## 3. Shadows (dropped; one Confirmed note)

Stage 0x02's distant-shadow cache (sub-object stage02+0x21A0) takes its player index at construction
(0x3E19654: `stage02+0x241A = [view+0x2268]`, which is 1 for the eye because it was built during setup). So the eye
renders keys 0xB7F464DD/0x337B7295 (0x3E1C230). The eye's stage 0x03 resolve picks keys by the live index (0x3E1F683:
index 0 -> 0xC0F3544B/0x447C4203) but uses its own stage 0x02's matrices and toggle (0x3E1D830, +0x21D8). When that
cache is enabled (`[DAT_14604AA10+0x48]`) and the two views' progressive updates are out of phase, the eye's distant
shadows would misalign. Fix if it ever shows: point the eye's stage 0x03 `+0x2150` at the main view's stage 0x02 and
set the eye's stage02 `+0x241B = 0`.

## Open leads

* Whether `[[level+0xB8]+0x250]` is set on the test levels and the player's room id there (log 1.4 items 1 and 3).
* If the room copy doesn't restore the outlines: decompile the step output-key assignment (outline step vtable slot
  +0xC8) and compare the eye's post chain (input/output keys per step). The 14:01:57 gpu trace already shows the eye's
  stage 0x21 chain differs from main's (no exposure-apply draw, because eyefix skips the eye's exposure job
  0x3EDE160). Check that the outline step's input key is still current for the eye.
* View +0x22B0..+0x22EF (clustered-lighting CB +0x170..) are zero in the eye per the field diff. Writers 0x3CC5AA0 and
  0x3AC51C0 (0x3AC5896). Not related to outlines; worth one log for lighting.
