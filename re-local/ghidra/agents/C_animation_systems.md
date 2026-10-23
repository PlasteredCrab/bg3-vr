# Agent C: animation, skeleton, visuals, input, control (bg3_dx11.exe 4.1.1.7631656)

Static RE only (Ghidra project `BG3_C`, headless; disassembler `bg3vr_disasm.exe`). Image base 0x140000000;
every address below is an RVA. Raw decompiles and xref dumps are in `re-local/ghidra/agents/C/` (file names
`rNN_*.c` / `rNN_*.txt`, cited as [rNN]). Names in `code` that come from the Script Extender (SE,
`downloads/bg3se-src`) are marked SE; everything else is derived from the decompiles cited.
"Unconfirmed" means the code read does not settle it; the item says what would.

Status (final, 2026-10-19): sections 1 (animation/skeleton), 2 (visual hiding) and 3 (input) are done in depth;
section 4 (character control) and 5 (other) are partial. Every claim cites a decompile/disassembly; items
that the code read does not settle are marked **unconfirmed** inline and collected in "Open leads" at the end.
Names of Granny API functions are Granny's names for the behaviour seen in the decompile (the exe has no
symbols for them).

---

## 0. Summary of hook points for the VR mod

| Need | Where | RVA | Notes |
| --- | --- | --- | --- |
| Override bones after animation, before skinning (model space) | Skeleton vtable slot 30 `UpdateMeshBindings` (entry) | 0x3A20590 (vtable 0x588D4A8 +0xF0) | `rcx` = skeleton. At entry `[skel+0xE0]` is the current world pose (just swapped in). Edit bone matrices there, then let it run: it rebuilds every mesh binding's skinning matrices from +0xE0. Called on animation worker threads (section 1.6). |
| Override bones in **local** space (children follow) | After the granny post-execute plugin builds the pose | 0x3A051F0 (post-hook) | Writes the genome's final local pose into `[skel+0xF0]` and builds the world pose into `[skel+0xE8]` (back buffer). A post-hook can edit `[skel+0xF0]` and rebuild `[skel+0xE8]` with Granny BuildWorldPose (thunk 0x4A11CC0). |
| Head bone world transform for the camera | Skeleton vtable slot 10 `GetBoneWorldMatrix(bone, mat4*)` | 0x3A1F170 (+0x50) | Model-space 4x4 (row-major, translation in row 3) from `[skel+0xE0]`. World = model x entity world matrix (as the genome node `GrannyGetBoneWorldTransform` 0x3A41700 does). Bone index from slot 17 `FindBone(FixedString name, short* out)` 0x3A1FB50 (+0x88). The engine's head bone name is `Head_M` (string 0x54EB48C, used with FindBoneByName at 0x3A131A6). |
| Per-frame order point | Animation job finish | 0x3BD6670 | Per character: `skel->vf[0xE8]` (swap world poses) at 0x3BD6992, `skel->vf[0xF0]` (skinning) at 0x3BD699E, then attachments 0x3F12D60 / 0x3F12F40 and visual post-update 0x3C6B900. |
| Hide a visual in one render view (head in first person, keep shadow) | `Visual` vtable slot 14 | 0x3C6B6F0 (vtable 0x588CB30 +0x70) | `(visual, renderView, bool hide)`: view index `[view+0x2268]` 0 sets cull bit 0x200, 1 sets 0x400 in `[visual+0xF4]`; setter 0x3C6B810 recurses into attached visuals. Main-scene gather masks 0x220 / 0x420 (0x3E00310). |

---

## 1. Animation and skeleton

### 1.1 Libraries and layers

- **Granny 3D 2.12.0.9999** is linked statically: source-path assert strings `E:\DepotFW4v3\3rdParty\src\Granny\2.12.0.9999\source\granny_*.cpp` (0x541A240 onwards) for `granny_local_pose`, `granny_world_pose`, `granny_skeleton`, `granny_ik`, `granny_mesh_binding`, `granny_mesh_deformer`, `granny_model_instance`, `granny_controlled_animation`, `granny_retargeter`, `granny_track_mask`, `granny_pose_cache` and others ([strings_anim_other.txt]). Library code: about 0x4A00000-0x4A90000. The game calls it only through small thunks (`mov eax,N; call __chkstk; ...; jmp target`, 0x4A0B000-0x4A11E00) [r15].
- **Genome** (Larian's animation blueprint graph, SE `gn::GenomeBlueprint` / `GenomeBlueprintInstance`): node types `GrannyNode_*` (FinalPose, PlayAnimation, Blend..., TransformBone, GetBoneWorldTransform, LocalToComponent ...), `IKNode_*` (IKSolverNode, IKPositionTaskNode, IKRotationTaskNode, IKLookAtTaskNode, IKGlobalLookAtTaskNode, IKFootPlacementTaskNode, IKKneeTaskNode, IKLinkTasksNode) and `ServerNode_*` (strings 0x54EFB78-0x54F0018, 0x56AFC50-0x56B0338, 0x588CCA8-0x58912C8).
- **Game glue** (call it "gr" layer): 0x3A00000-0x3A60000 (skeleton class, mesh binding, genome nodes, granny plugin). ls::AnimationBlueprintSystem: 0x3BD0000-0x3BDB000.
- ECS systems (type-name strings, [typenames_all.txt]): `ecl::AnimationUpdateSystem`, `ecl::TimelinePreAnimationUpdateSystem`, `ls::AnimationBlueprintSystem`, `ecl::animation::{GameplaySystem, EventDispatchSystem, BlueprintRefreshSystem}`, `ecl::lookat::TargetSystem`, `ecl::dummy::FootIKStateSystem`, `ecl::AnimationAttitudeSystem`; components `ls::AnimationUpdateComponent`, `ls::AnimationBlueprintComponent`, `ls::animation::StoredPoseComponent`, `ls::ActiveSkeletonSlotsComponent`, `eoc::lookat::TargetComponent`, `ecl::dummy::FootIKStateComponent`, `ecl::TimelineEyeLookAtOverrideComponent`, `ik::IKTask`, `gr::GrannyLocalPoseDesc`.

### 1.2 How ECS systems are registered (needed to find any system's update)

- Each `ls::GetTypeName<T>` string is referenced by a static initialiser that sets a 32-bit type-id global to 0x7FFFFFFF and links a descriptor into a list; e.g. 0xAA300 for `ecl::AnimationUpdateSystem` sets `DAT_146059210` [r2]. Script `TypeIdRefs.java` (agent dir) finds the global and all its readers [r3].
- A system's registration function reads that global, allocates the system object, and passes an update callback; e.g. 0x747AF0 (`ecl::AnimationUpdateSystem`) [r4]:

  ```c
  uVar1 = DAT_146059210;                       // system type id
  puVar3 = alloc(0x18); *puVar3 = &PTR_FUN_1454a92c0; puVar3[2] = param_2;
  local_150 = FUN_140761650;                   // update callback
  local_1a8 = DAT_14604c928;                   // dependency: ls::AnimationBlueprintSystem
  local_68 = DAT_14605ba30; local_64 = DAT_14605ba3c; ...   // component type ids
  FUN_1405de830(param_1 + 0x28,uVar1,&local_168);           // register
  ```
- All client (`ecl::`) systems are registered by 0x70EC90 (called from 0x5EBED0) [r5].
- The callback has the form `cb(systemObject, ctx)` and forwards to the body with ctx fields `+0x210`, `+0x198`, `+0x308` [r5]. Bodies found:

| System | Registration | Callback | Body |
| --- | --- | --- | --- |
| ecl::AnimationUpdateSystem | 0x747AF0 | 0x761650 | 0x1F14C80 |
| ecl::TimelinePreAnimationUpdateSystem | 0x747DA0 | 0x761740 | 0x1F1A1E0 |
| ecl::lookat::TargetSystem | 0x727300 | 0x7575D0 | 0x19336E0 |
| ecl::dummy::FootIKStateSystem | 0x73D1A0 (vtable 0x54C6908) | 0x75DE60 | 0x19F9750 + 0x19F9970 |
| ecl::equipment::VisualsVisibilityStateSystem | 0x72C7A0 | 0x759030 | 0x19A2550 (not decompiled) |

### 1.3 Per-frame animation pipeline (end to end)

1. **ECS tick, client world** - `ecl::AnimationUpdateSystem` body 0x1F14C80 [r6]: early-out if any entry in a singleton has `+0x1EC != 0` (same test appears in other animation code; probably pause, unconfirmed); iterates the query of animated entities; for each entity clears/sets bit 0x10 of the animation-instance flags (`+0x24`), calls 0x3BD3C60 / `FUN_143bd35b0(animSystem, ctx, &entityData)`; at the end root motion 0x3BD1AB0 and 0x1F157B0.
2. **Schedule the job** - 0x3BD35B0 (ls::AnimationBlueprintSystem) [r14]: looks the entity up in `AnimationUpdates` (`HashMap<EntityHandle, UpdateJob*>`, SE layout confirmed: buckets +0x98, bucket count +0xA0, next +0xA8, keys +0xB8, size +0xC4, values +0xC8), sets the job's delta time (`job+0x30 = dt * speed`, accumulated time `job+0x28`, frame counters +0x34/+0x38, state `job+0x10 = 1`), then either queues it to the thread dispatcher (`FUN_14410e5d0(system+0x168, job)`) or runs it inline (`FUN_143bd5e30`).
3. **Evaluate the blueprint** - 0x3BD5E30 (job execute) [r14]: state 1 -> 2; builds an execution context (0x3FF6580); runs pre-execute plugins (list at `[[instance+0x48]]+0xF0`), the blueprint's sequences (0x3FF7080 per sequence), post-execute plugins (list at `+0x100`); state -> 3; then either dispatches the finish job or calls the finish 0x3BD6670 directly.
4. **Granny post-execute plugin** - 0x3A051F0 (registered as a callback by 0x3A05A70; param_2 == 0 is the post phase) [r20]. Per skeleton slot of the character:
   - copies the final LocalPose produced by the graph into the skeleton's working local pose (`FUN_144a0ead0(src, FUN_143a1efe0(skel))`, i.e. Granny CopyLocalPose into `[skel+0xF0]`);
   - builds the world pose into the **back buffer** `[skel+0xE8]` with 0x4A11D00 (Granny BuildWorldPose, LOD variant, offset = NULL);
   - sets `[skel+0x129] = 1`, `[skel+0x124] = frame counter [0x614FB88]`;
   - also posts `RootMotionEvent` when the pose has root motion.
5. **Finish** - 0x3BD6670 (state 3 -> 4 -> 5) [r14], per skeleton slot without physics (`[skel+0x30] == 0`):
   ```asm
   03BD6992  FF 90 E8 00 00 00   call [rax+0xE8]   ; skeleton slot 29: swap +0xE0 <-> +0xE8
   03BD699E  FF 90 F0 00 00 00   call [rax+0xF0]   ; skeleton slot 30: rebuild mesh-binding skinning
   ```
   then `FUN_143f12d60(&ctx, visual)`, `FUN_143f12f40` for each attachment (`visual+0xB8`, stride 0x30), `FUN_143c6b900(visual)` (bone-driven attachments, blend-shape weights 0x3C6CCC0, other updates), and either queues the visual for physics (`system+0x240` list, when a renderable has flag `+0xF2 & 1`) or calls `FUN_143c63900(visual)`.
6. **Skinning matrices** - MeshBinding vtable 0x5891890 slot 4 (+0x20) = 0x3A57270 [r16]: swaps its two pose buffers (`+0x10` / `+0x18`, SE `MeshBinding::Pose{Pose1, Pose2}`), resolves bone remaps for attached/remapped skeletons (`+0xB8` source skeleton, `+0xC0` skeleton, index tables `+0xA0` / `+0xA8`, count `+0xB0`), then `FUN_144a11e00` (Granny BuildIndexedCompositeBuffer: `world[i] * InverseWorld4x4(bone i)` written as 3x4 matrices, 0x4A75CF0 [r16]) into the front pose buffer. Rigid bindings (`[binding+0x28]+0xE8` set) copy the single composite into the binding's 4x4 `+0x30..+0x6C`. Ends with `binding->vf[0x68]()`.

**Threads.** The job (steps 3-5) runs on whatever thread the dispatcher at `AnimationBlueprintSystem+0x168/+0x170` picks, or inline on the ECS thread when no dispatcher/flag (`(flags & 0x22) == 0x22` decides, 0x3BD5E30 end). Any hook in steps 3-6 must be thread-safe and filter by skeleton/visual pointer.

### 1.4 Data layouts

**Skeleton (game class, vtable 0x588D4A8; ctor 0x3A1EDD0, dtor 0x3A1EE50)** [r17-r19]. Base class fields match SE `Skeleton`:

| Offset | Type | Meaning | Evidence |
| --- | --- | --- | --- |
| +0x30 | ptr | physics skinned object (SE `Physics`); finish skips skeletons with it | 0x3BD6985 `cmp qword [rsi+0x30],0` |
| +0x40 | `{int FixedString name; short parent}`[] (8 bytes) | bone table (SE `Bones`) | 0x3A57270 reads `+0x40` with `+4` parent |
| +0x88 / +0x94 | `SkeletonSocket`[] (0x68 each) / count | sockets (SE) | 0x3A41700 `plVar8[0x11] + idx*0x68`; 0x3A57270 `+0x88`, `+0x94` |
| +0x98 | short | bone count (SE `BoneCount`) | many, e.g. 0x3A206F0 |
| +0xD8 | `granny_skeleton*` | | 0x3A1FB50, 0x3A57270 |
| +0xE0 | `granny_world_pose*` | **current** world pose (model space) | slots 10-12 read, 22-26 write; swap 0x3A1FBE0 |
| +0xE8 | `granny_world_pose*` | back/previous world pose; animation writes it | 0x3A051F0, slots 15-16, 27-28 |
| +0xF0 | `granny_local_pose*` | working local pose (lazily created by 0x3A1EFE0) | 0x3A1EFE0, 0x3A1F040 |
| +0xF8 | `granny_world_pose*` | alternative world pose (used when +0xE0 null; source of slot 6 copy) | 0x3A1F170, 0x3A206F0 |
| +0x100 | `granny_local_pose*` | base local pose (rest/reference) | 0x3A1F0E0, 0x3A1EF00 |
| +0x108 / +0x114 | `MeshBinding*`[] / count | mesh bindings driven by this skeleton | 0x3A20590 |
| +0x118 | double | timestamp (0x3A051F0) | |
| +0x120 / +0x124 | uint / uint | frame counters | 0x3A206D0, 0x3A1FBE0 |
| +0x128 / +0x129 | bool / bool | pose valid (front / back) | 0x128AF60 returns +0x128 |

**Skeleton vtable 0x588D4A8** (slot: offset RVA - behaviour) [r19 + disassembly]:

| Slot | +off | RVA | Behaviour |
| --- | --- | --- | --- |
| 0 | 0x00 | 0x3A1EE50 | destructor |
| 1 | 0x08 | 0x3A207A0 | `return [this+0x124] >= frame-1` (pose updated recently) |
| 2 | 0x10 | 0x128AF60 | `return [this+0x128]` |
| 3 | 0x18 | 0x3A206D0 | `+0x129 = 1; +0x120 = 0; +0x124 = frame` |
| 4 | 0x20 | 0x3A206B0 | clear +0x120 / +0x128 |
| 5 | 0x28 | 0x3A207C0 | full update: copy +0xF0 local pose into +0x100 if present, BuildWorldPose(+0xD8, 0, n, +0x100, NULL) into +0xE0 and +0xE8, then each mesh binding `vf[0x48]`/`vf[0x20]` |
| 6 | 0x30 | 0x3A206F0 | memcpy world pose +0xF8 -> +0xE0 |
| 7 | 0x38 | 0x3A20750 | BuildWorldPose from +0xF0 into +0xE0 |
| 8 | 0x40 | 0x3A1F040 | `GetBoneLocalTransform(short bone, Transform* out)` from +0xF0 (or +0x100); scale/shear dropped (scale out = 1) |
| 9 | 0x48 | 0x3A1F0E0 | same from +0x100 |
| 10 | 0x50 | 0x3A1F170 | `GetBoneWorldMatrix(short bone, float out[16])` from +0xE0 (or +0xF8) |
| 11 | 0x58 | 0x3A1F1E0 | `GetBoneWorldTransform(bone, Transform*)` (matrix decomposed: quat via 0x4149330, translation row 3, scale = row lengths) |
| 12 | 0x60 | 0x3A1F4A0 | `GetBoneWorldTranslation(bone, vec3*)` |
| 13 | 0x68 | 0x3A1F510 | matrix from +0xF8 |
| 14 | 0x70 | 0x3A1F570 | decomposed from +0xF8 |
| 15 | 0x78 | 0x3A1F820 | matrix from +0xE8 (or +0xF8) |
| 16 | 0x80 | 0x3A1F890 | decomposed from +0xE8 |
| 17 | 0x88 | 0x3A1FB50 | `FindBone(FixedString name, short* out) -> bool` (Granny FindBoneByName on +0xD8; out = -1 on failure) |
| 18 | 0x90 | 0x3A1FB90 | bone name (granny bone +0x00) by index |
| 19 | 0x98 | 0x3A1FBC0 | parent index (granny bone +0x08) |
| 20 | 0xA0 | 0x3A1FF20 | `SetBoneLocalTransform(bone, const Transform*, uint flags)` into +0xF0 (Granny transform: flags, position, orientation, scale diagonal) |
| 21 | 0xA8 | 0x3A1FEE0 | local transform flags |
| 22 | 0xB0 | 0x3A1FC20 | set world translation (+0xE0 matrix row 3) |
| 23 | 0xB8 | 0x3A1FE60 | set world 3x3 rotation/scale |
| 24 | 0xC0 | 0x3A1FC70 | set world rotation from quat + scale |
| 25 | 0xC8 | 0x3A20240 | set world 4x4 |
| 26 | 0xD0 | 0x3A1FFB0 | set world matrix from `Transform` (quat +0x00, translate +0x10, scale +0x1C) |
| 27 | 0xD8 | 0x3A20530 | set 4x4 in +0xE8 |
| 28 | 0xE0 | 0x3A202A0 | set from Transform in +0xE8 |
| 29 | 0xE8 | 0x3A1FBE0 | swap +0xE0/+0xE8, copy +0x129 -> +0x128, stamp +0x124 |
| 30 | 0xF0 | 0x3A20590 | for each mesh binding: `if (!b->vf[0x48]()) b->vf[0x20]()` (skinning) |
| 31 | 0xF8 | 0x3A20690 | mesh binding by index |
| 32 | 0x100 | 0x14E5FB0 | returns the mesh-binding array (used by 0x3C52E10, 0x3C6B900) |
| 33 | 0x108 | 0x3A20680 | mesh-binding count |

Slots 22-28 write directly into world-pose matrices of a single bone (children are not updated). 0x3C52E10 uses exactly this pattern to apply a stored pose: for each bone `vf[0xB0](bone, &translate)` and `vf[0xC0](bone, &quat, &scale)`, then `vf[0xF0]()` [r14].

**Granny structures (from the library code)** [r12, r16]:

| Struct | Layout | Evidence |
| --- | --- | --- |
| `granny_skeleton` | +0x00 name, +0x08 int BoneCount, +0x0C bone* Bones | FindBoneByName 0x4A1F790 |
| `granny_bone` (0xA4) | +0x00 char* Name, +0x08 int ParentIndex, +0x0C transform LocalTransform (0x44), +0x50 float InverseWorld4x4[16], (+0x90.. LOD / extended data) | 0x4A1F790, 0x4A2B9A0 walks parents via +8, 0x4A2AA50 copies +0x0C..+0x4F, 0x4A75CF0 uses +0x50 |
| `granny_transform` (0x44) | +0x00 uint Flags, +0x04 Position[3], +0x10 Orientation quat[4] (x,y,z,w), +0x20 ScaleShear[3][3] | 0x3A1F040 / 0x3A1FF20 |
| `granny_local_pose` | +0x00 int BoneCount, +0x04 entry* Transforms (0x50 each: +0 float Weight, +4 Count, +8 transform, +0x4C TraversalID), +0x0C float FillThreshold (0.2f), +0x10 TraversalID | NewLocalPose 0x4A2A5D0 |
| `granny_world_pose` | +0x00 int BoneCount, +0x04 float(*)[16] WorldTransformBuffer, +0x0C float(*)[16] CompositeTransformBuffer (null for "no composite") | 0x4A75120, 0x4A75250 |

Granny API thunks the game uses (behaviour identified from the target code; names are Granny's API names for that behaviour):

| Thunk | Target | Behaviour |
| --- | --- | --- |
| 0x4A0E780 | 0x4A2A5D0 | NewLocalPose(boneCount) |
| 0x4A0E7A0 | 0x4A2A7C0 | FreeLocalPose |
| 0x4A0E7C0 / 0x4A0E7E0 | 0x4A2A7F0 / 0x4A2A8D0 | GetResultingLocalPoseSize / NewLocalPoseInPlace |
| 0x4A0E800 | 0x4A2AA00 | GetLocalPoseBoneCount |
| 0x4A0E820 | 0x4A2AA10 | GetLocalPoseTransform(pose, bone) -> `&entry.Transform` |
| 0x4A0E860 | 0x4A2AA50 | BuildRestLocalPose(skeleton, first, count, pose) |
| 0x4A0E9E0 | 0x4A2B9A0 | GetWorldMatrixFromLocalPose(skeleton, bone, pose, offset, out) |
| 0x4A0EA40 | 0x4A2C340 | BuildLocalPoseFromWorldPose |
| 0x4A0EAD0 | 0x4A2C980 | CopyLocalPose(src, dst) |
| 0x4A100F0 | 0x4A1F790 | FindBoneByName(skeleton, name, int* out) -> bool |
| 0x4A11B20 / 0x4A11B40 / 0x4A11B60 | 0x4A74E50 / 0x4A74EA0 / 0x4A74F00 | NewWorldPose / NewWorldPoseNoComposite / FreeWorldPose |
| 0x4A11C00 / 0x4A11C20 | 0x4A75010 / 0x4A75120 | GetResultingWorldPoseSize / NewWorldPoseInPlace |
| 0x4A11C40 | 0x4A75250 | GetWorldPose4x4(pose, bone) -> `float*` (null if out of range) |
| 0x4A11C80 | 0x4A752A0 | GetWorldPose4x4Array |
| 0x4A11CC0 | 0x4A752C0 | BuildWorldPose(skeleton, first, count, localPose, offset4x4, worldPose) |
| 0x4A11D00 | 0x4A753A0 | BuildWorldPose (LOD variant, extra count) |
| 0x4A11E00 | 0x4A75CF0 | BuildIndexedCompositeBuffer(skeleton, worldPose, indices, count, out 3x4[]) |

All game call sites read pass `offset = 0` to BuildWorldPose (0x3A1EF00, 0x3A20750, 0x3A207C0, 0x3A30C60, 0x3A4FD10, 0x3A051F0), so world poses are relative to the skeleton root ("component/model space"). The NULL handling inside 0x4A75450 was not decompiled (unconfirmed, low risk: the genome node below multiplies by the entity matrix afterwards, which only makes sense for model-space poses).

**Matrix convention.** World-pose matrices are row-major with translation in elements 12-14 (0x3A1F4A0 returns `+0x30..+0x38`; 0x3A1FFB0 writes the translation to [12..14] and `1.0` to [15]). Engine `Transform` (SE) = quat (x,y,z,w) +0x00, translate +0x10, scale +0x1C, size 0x28 (0x3A1FFB0 reads `[0..3]` quat, `[4..6]` translate, `[7..9]` scale; 0x3C52E10 reads `+0x10` translate, `+0x1C` scale, `+0x00` quat from 0x28-byte entries).

**Visual -> skeleton.** `Visual+0x88` = array of `SkeletonSlot*`, count byte at `Visual+0x91` (SE `MiniCompactSet<SkeletonSlot*>`); `SkeletonSlot{+0 FixedString RemapperSlot (-1 = default slot), +8 Skeleton*}` (0x3C6B900, 0x3C6CCC0, 0x3C52E10: `*(int*)**(visual+0x88) == -1` then `Skeleton = *(slot+8)`).

### 1.5 Getting the head bone and its world transform (first-person camera)

1. Character entity -> `ls::VisualComponent` (SE: `Visual* Visual` at +0) -> `Visual`.
2. Default skeleton slot: first entry of `Visual+0x88` whose `+0` is -1; `Skeleton* s = *(slot+8)` (as 0x3C52E10 does).
3. `short head; s->vf[0x88](s, FixedString("Head_M"), &head)` (0x3A1FB50). Bone name evidence: the engine itself looks up `"Head_M"` with FindBoneByName at 0x3A131A6 (timeline head correction, below) and builds FixedString-like globals for it at 0x2DB440 / 0x526390 / 0x5263C0. Neck/eye bone names for BG3 rigs are not in the exe (data-driven; read them at runtime by walking slots 18/19).
4. Each frame after the finish step (or inside the slot-30 hook): `float m[16]; s->vf[0x50](s, head, m)` gives the head in model space.
5. World: `world = m x entityWorld`. The genome node `GrannyNode_GrannyGetBoneWorldTransform` Execute 0x3A41700 does exactly this: `(*plVar8+0x50)(plVar8, bone, &local_198)`, optional socket offset, then multiplies by a 4x4 taken from the animation instance (`*(**(ctx+0x18)+0x28+DAT_146046fcc)+0x18`). Which object owns that 4x4 (visual world matrix vs. entity Transform) is unconfirmed; check at runtime that `m x Visual.WorldTransform` (SE: `MoveableObject::WorldTransform` at Visual+0x08) matches.
6. Timing: the skeleton is rebuilt on animation threads during the frame; read after the ECS animation systems for the frame have completed (the camera view update 0x4085150 runs later in the client frame; ordering vs. the job dispatcher is unconfirmed - a fence or reading `+0x124 == frame` (slot 1) tells whether this frame's pose is in).

### 1.6 Bone override points (IK) and how to apply

- **A. Model-space, post-swap (recommended for "final say")** - detour skeleton slot 30 0x3A20590 (or patch the vtable entry 0x588D4A8+0xF0 = 0x588D598). At entry: `this` = skeleton, `[this+0xE0]` = this frame's pose. Overwrite the matrices of the IK chain (head, neck, spine, clavicles, arms, hands) - every affected bone individually, since parent changes don't propagate in a world pose - then call the original. Mesh bindings (skinning), then attachments and bone-attached visuals (0x3F12D60/0x3F12F40/0x3C6B900) read the edited pose. Also called by 0x3C52E10 (stored-pose apply) and by slot 5.
- **B. Local-space (children follow automatically)** - post-hook 0x3A051F0: edit `[skel+0xF0]` via slot 20 (0x3A1FF20) or Granny GetLocalPoseTransform (thunk 0x4A0E820), then rebuild `[skel+0xE8]` with BuildWorldPose (thunk 0x4A11CC0: `(granny_skel=[s+0xD8], 0, count, [s+0xF0], NULL, [s+0xE8])`). The finish swaps it in. Cost: one extra BuildWorldPose per frame for the player.
- **Previous-frame pose / motion vectors**: `+0xE8` after the swap holds the previous frame (double buffering via 0x3A1FBE0). Editing only `+0xE0` leaves the previous pose untouched, which is correct for velocity.
- **Filter**: compare `this` against the player's skeleton pointer (from step 1.5); hooks run for every animated character and on worker threads.

### 1.7 Existing IK and look-at the engine has

- **Genome IK nodes** (`IKNode_*` types registered by 0x20AABB0; FixedString globals 0x5EF0B20-0x5EF0B44) [r11, r23]:

| Node | Template ctor | Node ctor | Node vtable | Execute (slot 5) |
| --- | --- | --- | --- | --- |
| IKSolverNode | 0x20ABF80 | 0x20AC050 | 0x56B0458 | 0x20B0620 (PreExecute 0x20B05A0) |
| IKPositionTaskNode | 0x20AC1F0 | 0x20AC2E0 | 0x56B05F0 | 0x20B2970 |
| IKRotationTaskNode | 0x20AC4A0 | 0x20AC590 | 0x56B0570 | 0x20B2E50 |
| IKFootPlacementTaskNode | 0x20AC680 | 0x20AC770 | 0x56B0890 | 0x20B6880 |
| IKLookAtTaskNode | 0x20AC990 | 0x20ACA60 | 0x56B0720 | 0x20B3B60 |
| IKGlobalLookAtTaskNode | 0x20ACBF0 | 0x20ACCC0 | 0x56B0720 | 0x20B3B60 |
| IKKneeTaskNode | 0x20ACDD0 | 0x20ACEA0 | 0x56B0920 | 0x20B88E0 |
| IKLinkTasksNode | 0x20AD030 | 0x20AD100 | 0x56AFDE0 | 0x579DF0 |

  The task nodes fetch a solver object (`*(param_1+0x10)`, virtual interface: tasks by index via `vf[0x90]`/`vf[0x98]`/`vf[0xA0]`, targets set with `vf[8]` per axis) and use Granny GetWorldMatrixFromLocalPose (0x4A0E9E0) on the character skeleton. They only run if the character's blueprint contains them (the IK rig comes from `IKRigResource` / `IKRigBank`, strings 0x58B8BF8 / 0x590C7E0). Reusing them would mean injecting nodes or variables into Larian's blueprints; not practical. Own IK in hook A/B is simpler.
- **Timeline head correction** - 0x3A13090 [r28]: when a timeline animation has `HeadCorrection` (SE `GenomeTimelineAnimation::HeadCorrection`, quat at `param_3+0x48`), it finds `Head_M` (FindBoneByName) and rotates the head's local transform by the full correction and the parent's by half (`FUN_143a12b90(..., 1.0f)` and `(..., 0.5f)`) in the local pose. This is a working example of the engine editing head/neck in local space.
- **Look-at**: `eoc::lookat::TargetComponent` + `ecl::lookat::TargetSystem` (body 0x19336E0 only clears a per-entity byte `+0x14` of a 0x20-stride array after the query; the actual head turn happens in the blueprint, unconfirmed where). `ecl::TimelineEyeLookAtOverrideComponent` / `TimelineAutomatedLookatComponent` and strings `EyeLookAtBone`, `LookAtBone`, `IsEyeLookAtEnabled` (0x58BD718..) belong to timelines.
- **Foot IK**: `ecl::dummy::FootIKStateComponent` / `FootIKStateSystem` (0x19F9750/0x19F9970) are for "dummies" (timeline/photo-mode doubles); the solver itself is the genome `IKFootPlacementTaskNode`.
- **Root motion**: animation results queue `AnimationReceivedRootMotionEvent` (SE) records (EntityHandle + 7 floats: quat + translation); 0x3BD1AB0 drains the queue (`system+0x180`, CS `+0x198`) and applies it to the entity transform via 0x3C78110 [r14]. For VR locomotion this is where animation-driven movement enters the transform.

### 1.8 Genome nodes worth knowing (for reading bones the way the engine does)

| Node | FixedString | Template vtable / ctor | Node ctor / vtable | Execute |
| --- | --- | --- | --- | --- |
| GrannyNode_FinalPose | 0x5EF1C18 | 0x5890568 / 0x3A2C1C0 | (no runtime node) | function node; output consumed by 0x3A051F0 |
| GrannyNode_TransformBone | 0x5EF1C4C | 0x5891750 / 0x3A2D520 | 0x3A2D610 / 0x5891820 | 0x3A53910 |
| GrannyNode_GrannyGetBoneWorldTransform | 0x5EF1C68 | 0x58907D8 / 0x3A2DCD0 | 0x3A2DD90 / 0x5890BE8 | 0x3A41700 |
| GrannyNode_GrannyGetBoneComponentTransform | 0x5EF1C6C | registered by 0x3A29530 | | |
| GrannyNode_GrannyGetBoneLocalTransform | 0x5EF1C70 | registered by 0x3A29680 | | |
| GrannyNode_LocalToComponent | 0x5EF1C44 | 0x588E500 / 0x3A2D380 | | |

Granny node types are registered into the GenomeTypeManager by 0x3A221D0 (desc = {CreateNodeTemplate, CreateNode, CreateCompilerNode, 0, type id, GetInputSockets, GetOutputSockets}) [r11]; FixedString globals are created by 0x3A1B3C0 [r9].

---

## 2. Visuals: structure and hiding the head

### 2.1 Visual layout used by the code above

`ls::Visual` vtable **0x588CB30** (`GetRTTI` 0xE86450 returns 0x1001, unique in the exe). Offsets confirmed in decompiles:

| Offset | Meaning | Evidence |
| --- | --- | --- |
| +0x38 | `Scene*` (SE) | 0x3C65420 attaches, 0x3C6B810 notifies `FUN_143cd2f20(scene, visual)` |
| +0x88 / +0x91 | skeleton slots / count | 0x3C6B900 |
| +0x98 / +0xA4 | `ObjectDesc[]` (0x10 each: `Renderable*`, LOD byte +8, `VisualObjectType` flags byte +9) / count | 0x3C6B900, 0x3C67350 (stride 0x10) |
| +0xB8 / +0xC4 | `Attachment[]` (0x30 each, `Visual*` at +0) / count | 0x3C6B810, 0x3BD69E2 |
| +0xC8 | parent visual (SE `Parent`) | 0x3C6B6F0 condition |
| +0xD0 | `VisualResource*` (SE) | 0x3C6B040 |
| +0xF0 | `VisualFlags` (uint32, SE enum) | 0x3C64730 |
| +0xF4 | `VisualCullFlags` (uint16) | 0x3C6B810 |
| +0xF8 | bool (HasValidPose-like, recomputed by 0x3C6AF60) | 0x3C6B040 |
| +0x102 | short `BlendShapeBase` (-1 = none) | 0x3C6CCC0, SE VisualHelpers.cpp |
| +0x108 / +0x110 | entity handle / world (SE `VisualEntity`) | 0x3C6B810 |

SE enums (Engine.inl): `VisualCullFlags` CullNonProxy 0x02, CullShadow 0x04, CullShadow2 0x08, CullShadow3 0x10, CullDecal 0x80, **CullRenderView1 0x200, CullRenderView2 0x400**, CullScenery 0x800, CullShadowProxy 0x4000, CullShadow4 0x8000. `VisualObjectType` (ObjectDesc flags): Beard 0x08, Ears 0x10, **Head 0x20**, Horns 0x40. `VisualAttachmentFlags`: Hair 0x100, Armor 0x4000, Weapon 0x8000, Wings 0x10000, Horns 0x20000, Overhead 0x40000, InheritAnimations 0x20, ...

### 2.2 Per-render-view culling (the hiding mechanism)

- `Visual` vtable slot 14 (+0x70) **0x3C6B6F0** [r24]:
  ```c
  if (*(char *)(param_2 + 0x2268) == '\0') uVar3 = 0x200;      // render view index 0
  else if (*(char *)(param_2 + 0x2268) == '\x01') uVar3 = 0x400; // render view index 1
  else uVar3 = 0;
  ...
  FUN_143c6b810(param_1,uVar3,param_3);   // set/clear that bit in [visual+0xF4]
  ```
  `param_2` is a render view (`[view+0x2268]` = split-screen index, docs/re-notes/stereo.md), `param_3` = hide.
- Slot 15 (+0x78) **0x3C6B790**: same with mask 0x600 (both views).
- **0x3C6B810** (setter): `flags = hide ? flags | mask : flags & ~mask` at `+0xF4`; then `FUN_143cd2f20(scene, visual)` + `FUN_143c64830` (or, if not in a scene, an ECS-side update via `+0x108/+0x110`); then recurses into every attachment `(visual+0xB8)[i].Visual`.
- Slot 23 (+0xB8) **0x3C64730** recomputes the derived cull bits from `VisualFlags` (`CastShadow` 0x1 -> shadow bits 0x1E/0x803E, `ReceiveDecal` -> 0x80, `IsScenery` 0x400 -> 0x800) and notifies the scene; it leaves 0x200/0x400 alone (they are not in its masks).
- The main-scene visibility gather 0x3E00310 builds its cull query with `0x220` for view 0, `0x420` for view 1, `0x20` otherwise (`FUN_143cee530(&q, ..., uVar41)`) [r25], i.e. objects whose cull flags contain the view's bit are excluded from that view's main pass. stereo.md (live) also saw per-player visibility bits 0x220 / 0x420. Shadow passes: their masks were not identified (0x3E51DE0 is the other caller of 0x3CEE530 and passes masks from a variable) - **unconfirmed** whether a 0x200-hidden visual still casts shadows; test: hide the head via slot 14 and look for the head's shadow.

**Use for first person:** call `visual->vf[0x70](visual, eyeRenderView, true)` for the head/hair/helmet visuals (view 0 for the main/left eye view; if both eyes render through two views, hide in both or use slot 15). Unhide with `false`.

### 2.3 What is a separate visual (open)

- Equipment (armor, helmet, weapons) is attached as separate visuals: SE `ecl::EquipmentVisualRequest` carries `Parent`, `Bone`, `AttachFlags`, `HairType`; attachments carry `VisualAttachmentFlags` (Hair, Armor...). Since 0x3C6B810 recurses into attachments, hiding the **body** visual hides everything attached; hiding **one attachment** hides only it and its children.
- Head and hair: whether they are attachments (separate `Visual`s) or `ObjectDesc` renderables of the body visual (flag Head 0x20 exists for ObjectDescs) is **unconfirmed**. Check at runtime with SE Lua: `Ext.Entity.Get(uuid).Visual.Visual` -> `Attachments[i].Flags` / `.Visual.VisualResource`, and `ObjectDescs[i].Flags`. If the head is an ObjectDesc, per-renderable hiding needs a different lever (renderables are scene objects too: 0x3C6B900 calls `FUN_143cd2e80(scene, renderable)` per ObjectDesc; candidate: give the renderable its own cull bit in the scene's `CullFlags` array, to be investigated).
- The game's own "hide hair under helmet" logic lives in `ecl::equipment::VisualsVisibilityStateSystem` (registration 0x72C7A0, callback 0x759030 -> 0x19A2550, not decompiled yet) - next lead for how Larian hides head parts.
- Per-view fading also exists: `ecl::*FadeView0Component` / `*FadeView1Component` / `*FadeActiveView{0,1}Component` (obstruction, invisibility, crowd, timeline...), systems `ecl::FadeResolveSystem` (registration 0x723CC0), `ecl::obstruction::FadeSystem` (0x7236B0), `ls::CullSystem` (0x3AE5E20), `ls::OcclusionSystem` (0x3AE58D0). Not followed further.

---

## 3. Input

### 3.1 Pipeline: devices -> raw inputs -> input events -> listeners

```
SDL2.dll (imported; no SDL_GameController polling APIs are imported, only events)
  SDL_PollEvent loop  0x57AAA0  (window/platform object; called once per frame)
    keyboard / mouse / wheel / text  -> InputManager::InjectRaw 0x40F1F00 (or inline append, same queue)
    joystick + game-controller events -> 0x57DC00 (returns 1 when it consumed the event)
                                         -> InputManager::InjectRaw 0x40F1F00
InputManager (global ls::gInputManager = [0x604B1A8], SE layout)
  Update 0x40F0450(this, GameTime*)  - called by the game-loop thread 0x3F770A0 at 0x3F77333
    drains the raw queue (+0x3D0 ptr / +0x3D8 cap / +0x3DC count, 0x14-byte entries, CS at +0x28)
    -> 0x40F1FA0 per entry: raw state (InputStates), modifiers, bindings (InputScheme) -> InputEvent(s)
    -> hold / repeat / accelerated-repeat processing (0x40F3250, 0x40F3420, 0x40F3500)
    -> listeners (InputListenerGroup at +0x318 / +0x2E8; vf calls +0x40 modifiers, +0x48 text, +0x50 ...)
Listeners: camera controller OnInputEvent (3.3), ecl::InputController + CharacterTasks (section 4), UI (Noesis).
```

Evidence:
- **SDL pump 0x57AAA0** [r31]: `SDL_GetGlobalMouseState`, then `while (SDL_PollEvent(&e))`: if `FUN_14057dc00(this+0x60, &e)` returns 0 it handles `e.type` 0x400 (SDL_MOUSEMOTION: raw 0x80 on device 1, plus window grab), 0x200 (window events), 0x300/0x301 (key down/up: scancode -> raw via table `DAT_145f9bdf0`, appended directly to `InputManager+0x3D0`), 0x303 (text -> `+0x3C0`), 0x401/0x402 (mouse buttons, table `DAT_145f9c1f0`), 0x403 (wheel -> raw 0x85-0x88, scaled by `[DAT_14604b1b8+0x94]`). After the loop it warps the mouse back (`SDL_WarpMouseInWindow`) when a relative-mouse window is set (`this+0x48`) and emits raw mouse-motion axes 0x81/0x83 (x) and 0x82/0x84 (y) with value `delta * [DAT_14604b1b8+0x90] / (dt*20000)`.
- **Controller events 0x57DC00** [r32], `switch (e.type - 0x603)`: 0x603/0x604 joystick button down/up (table `DAT_145f9bd80`, 14 entries); **0x650 SDL_CONTROLLERAXISMOTION** (axis value, Y axes negated, radial dead zones from the options object `[DAT_14604b1b8+0xA0/+0xA4/+0xA8]`, sensitivity `+0x98/+0x9C`; emits the axis raw input from table `DAT_145f9bdd0` indexed `axis*2 + sign`, and releases the opposite direction); **0x651/0x652 controller button down/up** (table `DAT_145f9bda0`, 21 entries); 0x653 device added -> 0x57E3D0 (SDL_GameControllerOpen, registers device with the InputManager via 0x40F1140); 0x654 removed -> 0x57E620. Device id = controller slot + 3 (`local_72 = sVar13 + 3`), matching SE `EInputDeviceId` Controller1 = 3.
- The three tables are in .bss (filled at runtime); the raw ids they produce are SE's `InputRawType` (0x80 motion, 0x85-0x88 wheel match the decompile): leftstick_x 138, leftstick_y 139, leftstick_xneg 140, leftstick_ypos 141, leftstick_xpos 142, leftstick_yneg 143, rightstick_* 145-150, lefttrigger 151, righttrigger 152, controller_a/b/x/y 153-156, back 157, guide 158, start 159, stick presses 160/161, shoulders 162/163, dpad 164-167.
- **InjectRaw 0x40F1F00** [r32]:
  ```c
  ulonglong FUN_1440f1f00(longlong im, undefined4 *change /*16 bytes*/, undefined1 flag)
    EnterCriticalSection(im + 0x28);
    entry = im[+0x3D0] + count*0x14;  entry[0..3] = change[0..3]; entry+0x10 = flag;
    ++count (+0x3DC);  LeaveCriticalSection
  ```
  The 16 bytes are SE `InputRawChange` minus the two trailing bools: `{u16 InputId; u16 DeviceId; float Value; float Value2; u8 State}`; the 0x14-byte queue entry is SE `InjectInputData` (`InputInjects`).
- **Update 0x40F0450** [r37]: at the end, under the second critical section (`im+0x28`), it takes ownership of the `+0x3D0` array and calls `FUN_1440f1fa0(im, entry)` for each entry; also replays `+0x370`-area pending ids through 0x40F1FA0 and dispatches text entries to listeners (`listener->vf[0x48]`).

### 3.2 Where to inject synthetic controller input (VR controllers -> game)

- **Recommended:** call `0x40F1F00([0x604B1A8], &change, false)` from the game thread (or any thread: it locks) before `InputManager::Update` runs (0x3F77333 in the loop 0x3F770A0). Example: left stick forward = `{InputId = 141 (leftstick_ypos), DeviceId = 3, Value = v, Value2 = v, State = v > 0}`; release with value 0. This produces exactly what a real pad would, through the user's bindings (`PlayerProfiles/Default/inputconfig_controller.json` and per-player `inputconfig_p1.json`, strings 0x5899848 / 0x549DC80).
- Alternative: inject `SDL_Event`s at `SDL_PollEvent` (what the Script Extender does in `Extender/Client/SDL.cpp`: wrap SDL_PollEvent and return queued events first). Our hook would have to chain with the SE's Detours wrapper; the InputManager route avoids that and also skips the dead-zone code.
- **Device registration:** a synthetic device id 3 must be known to the InputManager (a real pad, or a device added through 0x40F1140 the way 0x57E3D0 does). Unconfirmed whether raw inputs for an unregistered device id are accepted; test by injecting with no pad connected. The InputManager also tracks the "preferred" device (keyboard vs controller UI mode, `ToggleInputMode` 0xC0); injecting controller input switches the UI to controller mode.
- **Mouse look / camera rotation:** raw 0x81-0x84 on device 1 (mouse), or stick 145-150; but for VR the camera is overridden (camera.md), so rotation inputs only matter for the facing logic of movement (section 4).

### 3.3 Input events (ids registered by 0x1B8E510)

0x1B8E510 registers every gameplay input event with `FUN_1440f36f0(im, &id, flags, type, category, description, &name, 0)` (camera events 0x61-0x68 use `FUN_1441705f0` + 0x40F51C0 in the same function) [r45]. Table extracted by `evtable.py` (agent dir, `input_event_ids.txt`); the name is the string built just before each id, so for the few repeated names (0xCA/0xCB, 0xF4/0xF5, 0x130/0x131) one of the two is probably mislabelled.

| Ids | Events |
| --- | --- |
| 0x61-0x6E | CameraCenter 0x61, CameraToggleMouseRotate 0x62, CameraBackward 0x63, CameraForward 0x64, CameraLeft 0x65, CameraRight 0x66, (CameraZoom 0x67, unconfirmed), CameraZoomIn 0x68, CameraZoomOut 0x69, CameraRotate 0x6A, CameraRotateLeft 0x6B, CameraRotateRight 0x6C, CameraRotateMouseLeft 0x6D, CameraRotateMouseRight 0x6E |
| 0x6F-0x86 | FreeCamera* (toggle mouse rotate, move F/B/L/R, FoV, speed, rotate controller/mouse, height, slowdown, freeze game time) |
| 0x87-0x99 | PhotoModeCamera* |
| 0x9D-0xA0 | **CharacterMoveForward, CharacterMoveBackward, CharacterMoveLeft, CharacterMoveRight** |
| 0xA1-0xDC | DragSingleToggle, TogglePresentation, ToggleSummaryFocus, JumpToFromTab, PartyManagement, PanelSelect, SpeakerSelect, CycleCharactersNext/Prev, ClimbingToggle, DestructionToggle, HighlightCharacters, **Interact 0xAD**, **ActionCancel 0xAE**, NextObject, Pause 0xB0, PrevObject, QuickLoad, QuickSave, SkipVideo, SplitItemToggle, RotateItemLeft/Right, TeleportPlayer/Party, ToggleCombatMode 0xBA, ToggleWeaponSet, ToggleDualWielding, ToggleSplitscreen 0xBD, ToggleInGameMenu, ToggleInfo, ToggleInputMode 0xC0, CancelSelectorMode, ToggleCharacterOverview, ToggleInventory, ToggleReactions, ToggleJournal, ToggleMap, ToggleSpells, ToggleSpellsEnd, ToggleInspiration, ToggleRestPanel, Shove 0xCC, ToggleSneak 0xCD, Throw 0xCE, ToggleGroupMode, ToggleGroupStealth, ToggleManualTurnBasedMode, TogglePartyCharacterView, GroupingSelect/Split/Confirm, **ContextMenu 0xD6**, ShortRest, Combine, **ToggleTacticalCamera 0xD9**, ShowWorldTooltips, ExpandTooltip, ShowSneakCones |
| 0xDD-0x142 | UI*: UIAccept 0xDF, UIBack 0xE0, UICancel 0xE1, UITakeAll, UIEndTurn 0xE3, ..., UIRadialLeft/Right/Up/Down 0xE8-0xEB, UIMove 0xF2, UISelection* 0xF7-0xFA, UISelectChar1-4, UISelectSlot0-12 (hotbar), UITabPrev/Next, map controls 0x11C-0x128, message box A/B/X/Y 0x12B-0x12E, photo mode toggles 0x140-0x142 |

### 3.4 How camera look is driven by input

- Camera controller vtable slot 17 (SE `OnInputEvent`) of the gameplay controller (vtable 0x58B6750, +0x88) = **0x3CF0780**: `if ([this+0x310]) [this+0x310]->vf[0x70]([this+0x310], this+0x318 /*camera entity*/, a, b)`.
- For the gameplay camera that ends in **0x1E0B740** [r43]: if the camera entity's camera component has `MasterBehaviorType == 8` (`*pcVar1 == '\b'`, same value camera.md measured) and accepts input (`+0x15`), it copies the event (0x50 bytes) and calls **0x1E0EE20**, else the generic handler 0x3B91DC0.
- 0x1E0EE20 [r44] switches on the event id: 0x61 (CameraCenter), 0x62 (toggle mouse rotate), 0x65/0x66 (pan left/right), 0x68-0x6E (zoom in/out, rotate), 0xD9 (ToggleTacticalCamera); it reads current values back with `FUN_1440efb10(gInputManager, ...)` (InputManager::GetInputValue).
- The camera system body (`ecl::CameraSystem`, registration 0x738A70 -> callback 0x75D250 -> **0x1E0A160**) and the camera behaviour code (0x1E0xxxx-0x1E1xxxx) apply the accumulated input each tick. Not decompiled in full here (camera.md covers the pose path).

### 3.5 Frame order on the game thread (0x3F770A0)

Per iteration [r38]: delta time from QPC (`[DAT_146046f98+0x22]`), clamped to 0.0667 s (0.2 s in a debug mode); scaled by `[DAT_14604b1b8+0x40]` (game speed); globals **`DAT_14614FB78` game time (double), `DAT_14614FB80` scaled dt, `DAT_14614FB84` real dt, `DAT_14614FB88` frame counter** (the counter the skeleton stamps at +0x124); then
1. `FUN_144122270(...)`;
2. **`InputManager::Update`** (0x3F77333);
3. `SetEvent(...)` (unblocks another thread);
4. **`App->vf[0x288](app, &gameTime)`** - the client update (EoCClient::Update 0x5889E0 via the SE mapping; ECS world update incl. animation and camera);
5. `vf[0x50]` on `[DAT_146047958+0x90]` and `+0x98`;
6. signals the render thread (`SetEvent` x2) and waits (`FUN_144108440`).

So synthetic input injected any time before step 2 is seen in the same frame's ECS tick.

## 4. Character control

### 4.1 Structure (client side)

- Player commands go through `ecl::InputController` (SE `Controllers/Input.h`: `OnInputEvent`, `Tasks` array, `RunningTask`, `PreviewTask`, `MousePos`, flags) and its **character tasks** (SE `CharacterTask`, virtual `Enter/Update/Exit/EnterPreview/UpdatePreview`, `OnInputEventText`, ...). Task type ids, confirmed by the exe's own enum registration 0x1583C80 [r40]: MoveTo 0, (1 ControllerSelection per SE), MoveToController/MoveController 2, MoveInDirection 3, Spell 4, Dialog 5, ItemUseRemotely 6, ItemUse 7, ItemDrop 8, ItemInteractArea 9, ItemMove 10, ItemMoveDummy 11, ItemCombine 12, ItemCombineDummy 13, SheathUnsheath 14, Lockpick 15, DisarmTrap 16, Pickpocket 17, PickUp 18, Listen 19, (Loot) 20, **CharacterMove 21** (absent from the SE enum), DefaultTargetAction 22, ClimbTo 23.
- Movement state lives in components: `eoc::MovementComponent` (SE: Direction vec3, Acceleration, Speed, Speed2; type id global 0x605BAC8), `eoc::SteeringComponent` (SE: TargetRotation, Speed, RequestSteering; type id 0x605BAB8), `eoc::controller::LocomotionComponent`, `ecl::steering::SSSteerComponent`, `eoc::steering::SyncComponent`.
- Client systems (registration -> callback -> body): `ecl::MovementSystem` 0x727760 -> 0x757710 -> **0x1937780**; `ecl::steering::SSSteerSystem` 0x727A40 -> 0x757760 -> **0x193B550**; `ecl::steering::SyncSystem` 0x727CB0 -> 0x7577B0 -> **0x193DBA0**; `ecl::PickingHelperManager` 0x7299A0 -> 0x7588E0 -> **0x1CC2F40** (per-player `PlayerPickingHelper` update 0x1C43820). Bodies 0x1937780 / 0x193B550 / 0x193DBA0 are query loops that call further helpers (0x19379C0, 0x19383A0, ...), not yet followed.
- **Root motion** from animation is applied to the entity transform by 0x3BD1AB0 -> `FUN_143c78110(transformSystem, &entity, &newTransform, 1, 1)` (section 1.7); 0x3C78110 is therefore a "set entity world transform" routine usable for teleport-style moves (unconfirmed side effects: server authority / replication; the server `esv::` systems own the authoritative position).

### 4.2 Driving the character from VR (options with evidence)

1. **Synthetic stick** - inject left-stick raw inputs (section 3.2); the game's controller movement (task MoveController / CharacterMove events 0x9D-0xA0) does the rest, relative to the camera. Because the gameplay camera is overridden by the mod, the camera yaw the movement code uses must match the HMD yaw (camera.md: the camera Transform written at 0x4085150 is what the controller getters return). Which yaw the movement task reads (camera entity Transform vs. `ecl::GameCameraBehavior` yaw) is unconfirmed - pending the search for the CharacterMove handlers (0x9D-0xA0) in progress in `r46_move_*.txt`.
2. **Keyboard WASD events** - inject raw keys (w=58, a=36, s=54, d=39) on device 0; same events 0x9D-0xA0 (flags 0x20).
3. **Click-to-move** - the MoveTo task (type 0) uses the picking helper's world position; driving `PlayerPickingHelper` (SE: `Inner.Position`/`WorldPosition`, `PickingDirection`, `WindowCursorPos`, `TargetOverride`) from a controller ray gives point-and-teleport-like movement. Picking update 0x1C43820 not yet decompiled.

Facing/rotation: `eoc::SteeringComponent.TargetRotation` (SE; yaw in radians at component +0xC) is the facing target; the steering systems above apply it.

**Yaw convention, confirmed** in the picking helper update **0x1C43820** [r47, r48]: for the local player's character entity (looked up by player id `[helper+0x168]` in the player map at `[DAT_1460487a0+0x138]`), it fetches a component with `FUN_141bbd7a0(world, entity)`, whose type id is `DAT_146053d74` (the second type-id global initialised for `eoc::SteeringComponent`, init 0x8B7C0), reads `yaw = [comp+0xC]` and writes

```c
fVar26 = sinf(fVar27); fVar27 = cosf(fVar27);
*(float *)(param_1 + 0x1b8) = (fVar27 * 0.0 + 0.0) - fVar26 * 1.0;   // x = -sin(yaw)
*(undefined4 *)(param_1 + 0x1bc) = 0;                                 // y = 0
*(float *)(param_1 + 0x1c0) = (0.0 - fVar26 * 0.0) - fVar27 * 1.0;   // z = -cos(yaw)
```

i.e. the helper's forward direction (SE `PlayerPickingHelper::PickingDirection`) is the character's steering yaw: **forward = (-sin yaw, 0, -cos yaw)** in BG3 world space (Y up). The controller nudge (`+0x1C4/+0x1C8`, flag `+0x1CC`, SE `ControllerNudge`) is read with 0x1C4BD20 from another component of the same entity; controller-mode picking then runs in 0x1C47610 and mouse picking in 0x1C48750. So in controller mode the "cursor" is relative to the character's facing, not to the camera.

## 5. Other VR-relevant findings

- **Frame timing globals** (0x3F770A0): game time `DAT_14614FB78`, frame counter `DAT_14614FB88`, dt `DAT_14614FB80` (scaled) / `DAT_14614FB84` (real). Skeleton pose stamps compare against the frame counter (slots 1, 3, 29).
- **Render-view cull bits per player** (section 2.2) are the same mechanism split-screen uses; with two views (stereo.md) the eye views are view 0 and view 1, so hiding the head must target both if both eyes render through game views.
- **Head bone at runtime**: section 1.5. The engine's own "Head_M" users: 0x3A13090 (timeline head correction), FixedString-like globals 0x5FC8BE8 (read by 0x2054490, IK/genome area), 0x5FCEA68/0x5FCEA70 (read by 0x20084B0 and the render-side 0x3D8EC30); the latter two were not decompiled.

---

## Signatures (unique in .text, from `bg3vr_disasm --sig`)

| RVA | Function | Signature |
| --- | --- | --- |
| 0x3A20590 | Skeleton slot 30, mesh-binding/skinning update (IK hook A) | `48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 8B B1 14 01 00 00` |
| 0x3A051F0 | granny post-execute plugin (IK hook B) | `85 D2 0F 85 ?? ?? ?? ?? 48 8B C4` |
| 0x3BD6670 | animation job finish | `40 55 56 41 56 48 8D AC 24 C0 FE FF FF` |
| 0x3A1FBE0 | Skeleton slot 29, swap world poses | `48 8B 81 E8 00 00 00 48 8B 91 E0 00 00 00` |
| 0x3A1F170 | Skeleton slot 10, GetBoneWorldMatrix | `40 53 48 83 EC 20 48 8B C1 49 8B D8 48 8B 89 E0 00 00 00 48 85 C9 75 ?? 48 8B 88 F8 00 00 00 48 85 C9 74 ?? 66 85 D2 78 ?? 0F BF D2 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 0F 10 00` |
| 0x3A1FB50 | Skeleton slot 17, FindBone | `40 53 48 83 EC 20 48 8B 89 D8 00 00 00` |
| 0x3A57270 | MeshBinding skinning (slot 4) | `4C 8B DC 55 57 49 8D AB 18 FF FF FF` |
| 0x3C6B6F0 | Visual per-render-view cull | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F BE 82 68 22 00 00` |
| 0x40F1F00 | InputManager inject raw input | `48 89 5C 24 10 57 48 83 EC 50 48 8B F9 C6 44 24 41 00` |
| 0x40F0450 | InputManager::Update | `48 8B C4 48 89 50 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 28 FF FF FF` |

Vtables (for vtable-slot patching instead of inline hooks): Skeleton 0x588D4A8, MeshBinding 0x5891890, Visual 0x588CB30, gameplay camera controller 0x58B6750.

## Open leads

1. **Head / hair as separate visuals or ObjectDescs** (2.3): check at runtime with SE (`Visual.Attachments[i].Flags`, `ObjectDescs[i].Flags` Head 0x20). Decides whether per-view culling (0x3C6B6F0) can hide only the head.
2. **Shadows of a view-culled visual** (2.2): only the main gather's mask (0x220/0x420, 0x3E00310) is confirmed; shadow-pass masks not found. Test live.
3. **Which 4x4 the engine uses as entity world matrix** for bone world transforms (1.5, 0x3A41700); verify `boneModel x Visual.WorldTransform` against a known bone position.
4. **Animation job threading / fence** (1.3): where the main thread waits for all jobs of the frame before the camera update; needed to read the head bone safely in 0x4085150. Leads: dispatcher `AnimationBlueprintSystem+0x168/+0x170`, 0x410E5D0, the spin-wait in 0x3BD4060.
5. **CharacterMove event handlers (0x9D-0xA0)** and the MoveController task: a full-text search (`case 0x9d:` & `case 0xa0:` over 0x1500000-0x2400000) was started and aborted at wrap-up; rerun it to find which yaw controller movement uses (camera vs. character steering).
6. **Synthetic device registration** (3.2): whether raw input for device id 3 is accepted without a registered pad (0x40F1140 is the add-device path used by 0x57E3D0).
7. **Steering/movement system bodies** 0x1937780, 0x193B550, 0x193DBA0 and 0x3C78110 (set entity transform) are only located, not analysed.
8. **Look-at**: where the blueprint turns the head toward `eoc::lookat::TargetComponent` targets (0x19336E0 only maintains a flag). The genome IK look-at node Execute is 0x20B3B60.
9. **UI** (goal 5): Noesis 3.1.6 is linked (source paths 0x531FDC0+); world-space UI systems `gui::tooltips::WorldTooltipsSystem`, `gui::contextmenu::WorldContextMenuSystem` exist. No render-target hook identified yet.
10. Unread users of `Head_M` globals: 0x2054490, 0x20084B0, 0x3D8EC30.

Tooling left in `re-local/ghidra/agents/C/`: `TypeIdRefs.java` (type-name string -> type-id global -> readers), `XrefTree.java`, `RangeCallers.java`, `SymbolRefs.java`, `CServer.java` + `req.ps1` (persistent request server; note a stale `req/quit.req` was left there - delete it before starting the server again), `strscan.py`, `sigfind.py`, `typenames.py`, `readmem.py`, `evtable.py`, `run.ps1`.

---

## Function index

| RVA | Name (ours / SE) | Section |
| --- | --- | --- |
| 0x0AA300 | static init: type id of ecl::AnimationUpdateSystem (0x6059210) | 1.2 |
| 0x70EC90 | registers all client ECS systems | 1.2 |
| 0x747AF0 | register ecl::AnimationUpdateSystem | 1.2 |
| 0x747DA0 | register ecl::TimelinePreAnimationUpdateSystem | 1.2 |
| 0x761650 / 0x1F14C80 | AnimationUpdateSystem callback / body | 1.3 |
| 0x761740 / 0x1F1A1E0 | TimelinePreAnimationUpdateSystem callback / body | 1.2 |
| 0x7575D0 / 0x19336E0 | lookat::TargetSystem callback / body | 1.7 |
| 0x75DE60 / 0x19F9750 / 0x19F9970 | dummy::FootIKStateSystem | 1.7 |
| 0x3BD35B0 | AnimationBlueprintSystem: schedule entity update job | 1.3 |
| 0x3BD5E30 | animation job: execute blueprint | 1.3 |
| 0x3BD6670 | animation job: finish (swap pose, skinning, attachments) | 1.3 |
| 0x3BD4060 | AnimationBlueprintSystem: remove entity (waits for job) | 1.3 |
| 0x3BD1AB0 | AnimationBlueprintSystem: apply root motion queue | 1.7 |
| 0x3BD3C60 | AnimationBlueprintSystem: per-entity pre-update | 1.3 |
| 0x3A051F0 | granny blueprint plugin, post-execute: final local pose -> world pose (+0xE8) | 1.3 |
| 0x3A05A70 | granny genome plugin init | 1.3 |
| 0x3A1EDD0 / 0x3A1EE50 | Skeleton ctor / dtor (vtable 0x588D4A8) | 1.4 |
| 0x3A1EF00 / 0x3A1EF70 / 0x3A1EFE0 | Skeleton: lazy world pose +0xE0 / +0xE8, local pose +0xF0 | 1.4 |
| 0x3A1F040 ... 0x3A20690 | Skeleton vtable slots (table 1.4) | 1.4 |
| 0x3A56DB0 / 0x3A56F10 | MeshBinding ctor / dtor (vtable 0x5891890) | 1.3 |
| 0x3A57270 | MeshBinding::UpdateSkinning (slot 4) | 1.3 |
| 0x3A13090 | timeline animation sampling with HeadCorrection on Head_M | 1.7 |
| 0x3A41700 | genome GetBoneWorldTransform Execute | 1.5 |
| 0x3A53910 | genome TransformBone Execute | 1.8 |
| 0x3A221D0 | register Granny genome node types | 1.8 |
| 0x20AABB0 | register IK genome node types | 1.7 |
| 0x20B0620 / 0x20B3B60 / 0x20B6880 | IK solver / look-at / foot placement Execute | 1.7 |
| 0x3C52E10 | apply stored pose to a visual's skeleton (world-pose setters + slot 30) | 1.4 |
| 0x3C6B900 | Visual post-pose update (attachments, blendshapes, bounds) | 1.3 |
| 0x3C6CCC0 | ls::Visual::UpdateBlendshapeWeightsFromSkeleton (SE) | 1.4 |
| 0x3C6B6F0 | Visual vf 14: per-render-view cull (0x200 / 0x400) | 2.2 |
| 0x3C6B790 | Visual vf 15: cull in both views (0x600) | 2.2 |
| 0x3C6B810 | Visual cull flag setter (+0xF4), recursive | 2.2 |
| 0x3C64730 | Visual vf 23: derive cull flags from VisualFlags | 2.2 |
| 0x3E00310 | main-scene gather (cull mask 0x220 / 0x420) | 2.2 |
| 0x57AAA0 | SDL event pump (keyboard/mouse/window) | 3.1 |
| 0x57DC00 | SDL joystick/game-controller event handler | 3.1 |
| 0x57E3D0 / 0x57E620 | controller added / removed | 3.1 |
| 0x40F1F00 | InputManager inject raw input (queue +0x3D0) | 3.1 |
| 0x40F0450 | InputManager::Update | 3.1 |
| 0x40F1FA0 | InputManager process one raw input -> events | 3.1 |
| 0x40F36F0 | InputManager register input event | 3.3 |
| 0x1B8E510 | registers all gameplay input events (ids table 3.3) | 3.3 |
| 0x3F770A0 | game-loop thread body (time, input update, app update) | 3.5 |
| 0x3CF0780 | gameplay camera controller OnInputEvent (slot 17) | 3.4 |
| 0x1E0B740 / 0x1E0EE20 | gameplay camera input handler / event switch | 3.4 |
| 0x738A70 / 0x75D250 / 0x1E0A160 | ecl::CameraSystem registration / callback / body | 3.4 |
| 0x1583C80 | CharacterTaskType enum registration | 4.1 |
| 0x727760 / 0x1937780 | ecl::MovementSystem registration / body | 4.1 |
| 0x727A40 / 0x193B550 | ecl::steering::SSSteerSystem | 4.1 |
| 0x727CB0 / 0x193DBA0 | ecl::steering::SyncSystem | 4.1 |
| 0x7299A0 / 0x1CC2F40 | ecl::PickingHelperManager registration / body | 4.1 |
| 0x1C43820 | PlayerPickingHelper update (forward from steering yaw) | 4.2 |
| 0x1C47610 / 0x1C48750 | controller / mouse picking | 4.2 |
| 0x3C78110 | set entity transform (used by root motion) | 4.1 |
| 0x4A0B000-0x4A11E00 | Granny API thunks (table 1.4) | 1.4 |
| 0x4A1F790 | Granny FindBoneByName | 1.4 |
| 0x4A752C0 | Granny BuildWorldPose | 1.4 |
| 0x4A75CF0 | Granny BuildIndexedCompositeBuffer | 1.4 |
