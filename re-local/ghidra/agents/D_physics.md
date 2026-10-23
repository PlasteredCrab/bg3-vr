# BG3 physics reverse-engineering reference (agent D)

Build: `bg3_dx11.exe` 4.1.1.7631656, image base 0x140000000. All addresses are RVAs unless written `base+...`.
Method: static only (Ghidra project `BG3_A`, headless query server `agents/D/scripts/DServer.java`; raw outputs in
`re-local/ghidra/agents/D/req/*.out`, decompiles in `agents/D/dec/*.c`, helper scripts in `agents/D/py/`).
"Confirmed" = shown by the decompiled code, disassembly or data cited here. "Unconfirmed" = inference; each one says
what would confirm it. Status: complete for engine identification, scene/step/threading, actor and object APIs,
queries, ragdoll lifecycle (physics side) and client/server split. Open items are in §12.

Contents
- §0 Index of functions, vtables and globals
- §1 Engine identification
- §2 PhysX vtable maps (PxPhysics, PxScene, PxRigidDynamic)
- §3 Scene classes, frame flow, threading, contact callback
- §4 Physics objects: factory, container, per-actor wrapper
- §5 Scene queries (raycast / sweep / overlap)
- §6 Ragdolls
- §7 Client vs server
- §8 Runtime raycast probe (confirmed recipe)
- §9 Objects in the world: items / scenery physics and group flags
- §10 Recommended approach for VR hand interaction
- §11 Method notes and how vtables were identified
- §12 Open items

---

## 0. Index

### 0.1 PhysX SDK (statically linked NVIDIA PhysX 4.1)
| RVA | What | Evidence |
|---|---|---|
| 0x5F82468 (data) | `PxPhysics*` singleton | `FUN_144a9bbd0(){ return DAT_145f82468; }` |
| 0x4A9BBD0 | `PxGetPhysics()` | above |
| 0x5427778 | `NpPhysics` vtable | createScene +0x88, createMaterial +0xE8 (string), getNbScenes +0x90, getScenes +0x98 (decompiled) |
| 0x542E9D0 | `NpScene` vtable | 9 slots anchored by assertion strings (§2.2) |
| 0x5433838 | `NpRigidDynamic` vtable | +0x98 / +0x198 / +0x1A0 anchored by strings, +0x1E8 decompiled (§2.3) |
| 0x4AAF290 / 0x4AAF520 | NpScene::simulate / fetchResults | strings |
| 0x4AADCF0 / 0x4AADF20 | NpScene::addActor / removeActor | strings |
| 0x4AB0B40 / 0x4AB0BE0 | NpScene::lockRead / unlockRead | "unlockRead() called without matching call to PxScene::lockRead()" in 0x4AB0BE0 |
| 0x4AC01B0 | NpRigidDynamic::setKinematicTarget | decompiled |
| 0x4AC1280 / 0x4AC1310 | setLinearVelocity / setAngularVelocity | decompiled |
| 0x4AC1560 | addForce(force, mode, autowake) | decompiled |
| 0x4AC5140 / 0x4AC5630 | setRigidBodyFlag / setRigidBodyFlags | strings |
| 0x4AC4100 | setActorFlag | special-cases flag 8 (eDISABLE_SIMULATION) |
| 0x4AC19F0 / 0x4AC1A20 / 0x4AC18E0 | wakeUp / putToSleep / setWakeCounter | decompiled |
| 0x4CF7740 | `PxRigidBodyExt::setMassAndUpdateInertia` | unconfirmed name; argument shape (body, mass, const PxVec3*, bool) fits |

### 0.2 Larian phx layer: scenes and step
| RVA | What |
|---|---|
| 0x5891B30 | main simulated scene vtable (`phx::PhysXScene`); PxScene* at +0x98 |
| 0x3A60790 | main scene constructor (called once, from client world setup 0x70EC90 @0x710FC8) |
| 0x3A60CF0 | main scene Init (slot +0x130): PxScene, sim-event callback, ragdoll manager |
| 0x184FDC0 | the only caller of main-scene Init (args 0x4004, 0xE14, 0xE14, 0x1A7D2100) |
| 0x75BA10 | `phx::PhysXSceneUpdateSystem` update (accumulate dt, ragdoll update, schedule job) |
| 0x3A74AC0 | physics job execute → step |
| 0x3A61B70 | main scene step: ≤3 fixed 1/60 s substeps |
| 0x5893C28 | `phx::SimplePhysXScene` vtable (query-only scene; PxScene* at +0x88); ctor 0x2396A10, Init slot +0x118 = 0x3A769F0 |
| 0x23318B0 | registers `phx::SimplePhysXScene` (type id 0x606673C) in the **server** world (called from 0x232A860 ← 0x21B7EB0) |
| 0x2396B80 | SimplePhysXScene update: schedules its job; job execute 0x3A76B30 = simulate(dt) + fetchResults |
| 0x5893BD8 | small per-entity scene (0x30 bytes, PxScene at +0x10), built by 0x1DAB2A0, stepped by 0x3A768C0 (purpose unconfirmed) |
| 0x5894948 / 0x3A91A50 | PxSimulationEventCallback vtable / onContact |
| 0x60490E8 (data) | phx global struct (16 bytes): [0] active scene, [1] scene registered after Init |

### 0.3 Larian phx layer: objects
| RVA | What |
|---|---|
| 0x6047B98 (data) | physics resource manager: +0x08 template loader, +0x18 object factory, +0x20 resource releaser |
| 0x5498F30 | object factory vtable; slot +0x08 = 0x3A5F510 create, slot +0x18 = release |
| 0x3B24310 / 0x3B24150 / 0x3B24690 | resMgr Instantiate(resourceFS, scale, kind) / Instantiate(templates[], scale*, kind) / FindTemplate(FS) |
| 0x3A5F510 | factory: build container + per-actor wrappers |
| 0x3A78850 | create PxRigidStatic / PxRigidDynamic / kinematic body from a template actor |
| 0x5893AD8 (base 0x58B40F8) | default container vtable (`phx` physics object; 0x70 bytes) |
| 0x3C95B10..0x3C962C0 | container base methods (§4.2) |
| 0x58940D0 / 0x5893F48 / 0x5894258 (base 0x58C3058) | per-actor wrapper vtables: dynamic/kinematic, static, articulation link |
| 0x3A7C380 | wrapper +0xB8 `SetWorldTransform(pose, bool asKinematicTarget)` |
| 0x3DE4680 | wrapper +0xA0 `GetOwnerEntityHandle()` → `&container->entity (+0x20)` |
| 0x2D6D200 | builds a static physics object from an object template, composes its group word (§9) |

### 0.4 Larian phx layer: ragdolls
| RVA | What |
|---|---|
| 0x5894878 | ragdoll manager vtable (instance at mainScene+0x90) |
| 0x3A8AF90 | AddRagdoll(desc) → id |
| 0x3A8B780 / 0x3A8B810 | RemoveRagdoll(id) / MarkForRemoval(id) |
| 0x3A8B880 | GetRagdollPose(id, out) |
| 0x3A8CA10 | GetRagdollFlags(out, id) |
| 0x3A8CAA0 | per-frame ragdoll update (follow / freeze) |
| 0x58948A8 | ragdoll container vtable (dtor 0x3A90F30) |
| 0x3A8E320 / 0x3A8FD70 | ragdoll body builder (poses + initial velocities) / joint builder |
| 0x3A62960 | main scene slot +0x118: create ragdoll handle (0x80 bytes) |
| 0x3DCD200 | animation node runtime `PhysicsNode_AddRagdoll` (ground probes, create, fire `PhysicsEvent_RagdollAdded`) |
| 0x3DD4FC0 | build ragdoll descriptor from skeleton, call AddRagdoll |
| 0x3DCDD40 | node event handler: on `PhysicsEvent_RagdollRemovedInternal` → MarkForRemoval, free handle |
| 0x3A4E0B0 | `RagdollPose` node evaluate: GetRagdollFlags + GetRagdollPose → skeleton |
| 0x3A4DF80 | registers node events `PhysicsEvent_RagdollAdded` / `RagdollRemovedInternal` |
| 0x38096F0 / 0x3809840 | animation-graph node type registration for AddRagdoll / RemoveRagdoll |

### 0.5 Globals and ECS type ids
| Global | Meaning | Evidence |
|---|---|---|
| 0x6007A28 | client level manager (BG3SE: `ecl::gLevelManager`); **same object** as 0x6048898 | 0x1818E40 stores `FUN_1417f06e0(alloc 0xC0)`; that ctor writes `DAT_146048898 = param_1` |
| 0x603B2B0 | server level manager (BG3SE: `esv::gLevelManager`) | used by server world setup 0x21B7EB0 |
| 0x5FDA240 | ecl client; +0x1A8 = client ECS world | agent B report; picking ctor 0x184CE60 |
| 0x6047B98 | phx resource manager | §0.3 |
| 0x60490E8 | phx scene global struct | §8.1 C |
| 0x5FC3054 | ragdoll id counter | 0x3A8AF90 |
| 0x604B1B8 +0x124 | global "pause physics time" byte (ragdoll timers and new ragdoll bodies honour it) | 0x3A8CAA0, 0x3A8E320 |
| 0x58C3050 | static null EntityHandle (0xFFC0000000000000) | bytes |

| ECS type | id global | GetTypeName fn |
|---|---|---|
| phx::PhysXScene | 0x6058CEC | 0x937E0 |
| phx::SimplePhysXScene | 0x606673C | 0xF5340 |
| phx::NVClothScene | 0x6058CF0 | 0x93920 |
| phx::PhysXSceneUpdateSystem | 0x605902C | 0xA0BC0 |
| phx::PhysXDisableSimulationSystem | 0x6059028 | 0xA0A80 |
| phx::PhysXSceneAttacherSystem | 0x6059024 | 0xA0940 |
| phx::PhysXSceneManualSpringSystem | 0x6059030 | 0xA0D00 |
| phx::PhysXScenePreupdateSkinnedObjectsSystem | 0x6059034 | 0xA0E40 |
| phx::PhysXSceneClothLinkSystem / ClothSolverUpdateSystem | 0x605901C / 0x6059020 | 0xA06C0 / 0xA0800 |
| ls::PhysicsRequestSystem / ls::PhysicsECSSystem / ls::PhysicsLoaderSystem | 0x6058CFC / 0x6058D34 / 0x604E4F4 | 0x93CE0 / 0x94E60 / 0x2F580 |
| ls::ClusterPhysicsLoaderSystem / ClusterPhysicsVisibilitySystem | 0x605938C / 0x613B8E4 | 0xB19C0 / 0x4E2D90 |
| ls::PhysicsComponent | 0x605BA04 | 0xB8810 |
| ls::PhysicsCreateOneFrame / Load / LoaderRegistered / PathLoadDesc / ResourceLoadDesc / StreamLoad / Loaded / ModificationOneFrame / ReloadOneFrame | 0x605BB88 / BB8C / BB90 / BB94 / BB98 / BB9C / BBA0 / BBA4 / BBA8 | 0xBFA70 .. 0xC0470 |
| ls::ClusterPhysicsLoadedComponent / ls::PhysicsSkinnedConnectionComponent | 0x605BEA0 / 0x605BEB4 | 0xCDDF0 / 0xCE430 |
| eoc::RagdollSoundComponent | 0x605C024 | 0xD5550 |
| ecl::controller::PhysicsSystem / ecl::ragdoll::SoundSystem | 0x605904C / 0x60590C8 | 0xA15C0 / 0xA3C80 |
| esv::StaticPhysicsManager / esv::PhysicsSkinnedConnectionSystem | 0x60D9594 / 0x60D96D4 | 0x332130 / 0x338530 |
| ecl::PickingHelperManager | 0x6058F94 | 0x9DC40 |
Systems whose queries include `ls::PhysicsComponent` (from their registrations): ecl::death::CharacterSystem (0x6058F80),
ecl::CharacterManager, ecl::hit::HitSystem, ecl::ProjectileManager, ecl::item::BoundsSystem, ecl::CameraSystem,
ecl::camera::CombatTargetSystem, ecl::spell::ImpactSoundSystem, ecl::TerrainManager, esv::placement::ValidationSystem,
esv::VisualSystem, esv::TerrainManager, esv::inventory::* and others (list in `dec/pc_callbacks.txt`).

---

## 1. Physics engine identification (confirmed)

* **NVIDIA PhysX 4.1**, statically linked (no PhysX DLL in `bin\`). Assertion strings carry source paths
  `...\3rdParty\src\PhysX\4.1\physx\source\physx\src\NpScene.cpp` (e.g. 0x5429FF0). RTTI type descriptors exist
  (`.?AVNpScene@physx@@` 0x5F21B98, `.?AVNpRigidDynamic@physx@@`, `.?AVNpArticulationReducedCoordinate@physx@@`,
  `.?AVD6Joint@Ext@physx@@`, `.?AVSphericalJoint@Ext@physx@@`, ...), but there are no complete object locators, so RTTI
  does not lead to vtables. The GPU loader code is linked (`PhysXGpu_64.dll` 0x5498610) but no GPU DLL ships.
* **NvCloth** is linked as well (`...\PhysXSDK\4.1\Win64\v143\NvCloth\...`, `phx::NVClothScene`).
* Larian's integration namespace is **`phx::`** plus `ls::Physics*` ECS components. Larian classes have no RTTI.

---

## 2. PhysX vtable maps (confirmed by multiple anchors)

### 2.1 PxPhysics (`NpPhysics` vtable 0x5427778)
| off | method | evidence / users |
|---|---|---|
| +0x88 | createScene(desc) | result stored as PxScene: 0x3A60CF0, 0x3A769F0, 0x1DAB2A0 |
| +0x90 | getNbScenes() | 0x4A9C5E0: returns `*(phys+0x10)` under mutex `phys+0xA0` |
| +0x98 | getScenes(buf, size, start) | 0x4A9C630: copies from array `phys+0x08` |
| +0xA0 | createRigidStatic(pose) | 0x3A78850 type 1, 0x3A7E0B0 |
| +0xA8 | createRigidDynamic(pose) | 0x3A78850 types 2/4, 0x3A7E3E0, 0x3A7E740 |
| +0xD8 | createArticulation() | 0x3A79120 |
| +0xE8 | createMaterial(sf, df, restitution) | 0x4A9CA00 has "createMaterial: limit of 64K materials reached" |

### 2.2 PxScene (`NpScene` vtable 0x542E9D0)
Anchors: +0x40 addArticulation 0x4AAE030, +0x48 removeArticulation 0x4AAE170, +0x50 addActor 0x4AADCF0,
+0x68 removeActor 0x4AADF20, +0x78 addAggregate 0x4AAE280, +0xA0 getActiveActors 0x4AAEBE0,
+0x100 setSimulationEventCallback 0x4AAED40, +0x188 simulate 0x4AAF290, +0x1B0 fetchResults 0x4AAF520
(each referenced its own assertion string). Other slots follow the PhysX 4.1 declaration order:
| off | method | | off | method |
|---|---|---|---|---|
| +0x08 | release | | +0x190 | advance |
| +0x50 | addActor | | +0x198 | collide |
| +0x68 | removeActor | | +0x1A8 | fetchCollision |
| +0x100 | setSimulationEventCallback | | +0x1B0 | fetchResults |
| +0x188 | simulate(dt, task, scratch, size, control) | | +0x2B0 | raycast (used by 0x3A85AA0 with matching args) |
| +0xE8 | getCpuDispatcher | | +0x308/+0x310 | lockRead / unlockRead (0x4AB0B40/0x4AB0BE0) |
| | | | +0x318/+0x320 | lockWrite / unlockWrite |
`PxScene+0x08` holds the Larian scene object (Larian writes it: 0x3A60CF0, 0x3A769F0).

**Locking rule used by the game:** every Larian mutation of a PhysX actor is bracketed by `lockWrite(0,0)`/`unlockWrite()`
on `actor->getScene()` (PxActor slot +0x30); reads use lockRead/unlockRead. lockRead (0x4AB0B40) keeps a per-thread depth
counter (TLS index at scene+0x2624) and on the outermost call takes the scene RW lock at scene+0x2630 as a reader
(`FUN_144d6e750(scene+0x2630, writer != thisThread)`).

### 2.3 PxRigidDynamic (`NpRigidDynamic` vtable 0x5433838)
Anchors: +0x98 = 0x4ABFD40 ("PxRigidDynamic::setGlobalPose: ..."), +0x198/+0x1A0 = 0x4AC5140/0x4AC5630
("RigidBody::setRigidBodyFlag: kinematic bodies with CCD ..."), +0x1E8 = 0x4AC01B0 (kinematic target, decompiled),
+0x50 = 0x4AC4100 (flag 8 special case). Every Larian call site's argument shape matches.
| off | method | | off | method |
|---|---|---|---|---|
| +0x20 | isKindOf(name) | | +0x138 | setLinearVelocity(v, autowake) |
| +0x30 | getScene | | +0x140 / +0x148 | get/setAngularVelocity |
| +0x48 | getWorldBounds(inflation) | | +0x150/+0x158 | set/getMaxAngularVelocity |
| +0x50 | setActorFlag(flag, bool) | | +0x160/+0x168 | set/getMaxLinearVelocity |
| +0x58/+0x60 | set/getActorFlags | | +0x170 / +0x178 | addForce / addTorque (f, PxForceMode, autowake) |
| +0x90 / +0x98 | getGlobalPose / setGlobalPose(pose, autowake) | | +0x180/+0x188 | clearForce / clearTorque |
| +0xA0..+0xB8 | attach/detachShape, getNbShapes, getShapes | | +0x198 / +0x1A8 | setRigidBodyFlag / getRigidBodyFlags |
| +0xD0/+0xD8 | set/getCMassLocalPose | | +0x1C0/+0x1C8 | set/getMaxDepenetrationVelocity |
| +0xE0/+0xE8 | setMass / getMass | | +0x1E8 / +0x1F0 | setKinematicTarget / getKinematicTarget |
| +0x110..+0x128 | set/get linear and angular damping | | +0x1F8 | isSleeping |
| +0x130 | getLinearVelocity | | +0x200/+0x208 | set/getSleepThreshold |
| | | | +0x238 / +0x248 / +0x250 | setWakeCounter / wakeUp / putToSleep |
| | | | +0x258/+0x260 | set/getSolverIterationCounts |
| | | | +0x268/+0x270 | get/setContactReportThreshold |
Concrete types tested by Larian (`*(short*)(actor+8)`): 5 PxRigidDynamic, 6 PxRigidStatic, 13 PxArticulationLink,
0x105 D6 joint. `PxActor::userData` (actor+0x10) = Larian per-actor wrapper. eKINEMATIC = 1, eDISABLE_SIMULATION = 8.

---

## 3. Scenes, frame flow, threading

### 3.1 Scene classes (confirmed)
1. **Main simulated scene** — vtable 0x5891B30 (secondary 0x5891AF8 at +0x80, job sub-object 0x5891AC0 at +0x1A8).
   Built by 0x3A60790 in the client world setup 0x70EC90 and registered as the ECS system `phx::PhysXScene`
   (the registered pointer is `scene+0x80`). Init (slot +0x130, 0x3A60CF0), called only by 0x184FDC0 with
   (boundsMin, boundsMax, 0x4004, 0xE14, 0xE14, 0x1A7D2100):
   ```c
   FUN_143a66920(desc, {1.0f, 10.0f});  FUN_143a85980(desc, boundsMin);          // tolerance scale, bounds
   *(param_1 + 0x98) = physics->createScene(desc);                                 // PxScene*
   cb = alloc(0x10) { vtbl 0x5894948, 1/60 }; *(param_1 + 0x158) = cb;
   pxScene->userData(+8) = this;  pxScene->setSimulationEventCallback(cb);
   mgr = alloc(0x50) { vtbl 0x5894878, [1]=this, +0x10=0x4004, +0x14=0xE14, crit-sec +0x18 }; *(param_1+0x90) = mgr;
   *(param_1 + 0x170) = 0xE14;  *(param_1 + 0x16C) = 0x1A7D2100;
   ```
   Fields: +0x10 ragdoll listener array (9 qwords each, count +0x1C), +0x90 ragdoll manager, +0x98 PxScene*,
   +0x118..+0x148 hash map {wrapper* → queued pose} (kinematic interpolation, §3.3), +0x158 event callback,
   +0x160 CPU dispatcher, +0x16C mask: static actors whose container group (`owner+0x60`) intersects it are added
   with eDISABLE_SIMULATION (slot +0x28 = 0x3A65060), +0x174/+0x180 the two Init vec3s, +0x198 job handle,
   +0x1A8 post-step job object.
2. **`phx::SimplePhysXScene`** — vtable 0x5893C28 (0x90 bytes, ctor 0x2396A10), registered by 0x23318B0 in the
   **server** world (§7). PxScene* at +0x88. Its addActor (slot +0x28 = 0x3A77640) always sets
   `actor->setActorFlag(eDISABLE_SIMULATION, true)` before `addActor`, so nothing in it moves; it exists for queries.
   Its update 0x2396B80 still schedules a job (object at scene+8, vtable 0x5893D80) whose execute 0x3A76B30 runs
   `simulate(dt)` + `fetchResults(true)` under lockWrite. Slot +0x100 returns 0 (no kinematic interpolation).
3. **Small per-entity scene** — vtable 0x5893BD8 (0x30 bytes), created per entity by ECS function 0x1DAB2A0,
   stepped by slot +0x30 0x3A768C0 (`simulate(1.0f)`, `fetchResults(true)`). Purpose unconfirmed (confirm by naming the
   ECS queries 0x1DAB2A0 iterates: ids at DAT_1460C03B8.. ).

### 3.2 Frame flow and threading (confirmed)
`phx::PhysXSceneUpdateSystem` (registration 0x7330A0, system object vtable 0x54A92C0, update 0x75BA10):
```c
// game/ECS thread
if (scene->pxScene) {
  sys->accum += dt;
  if (scene->ragdollMgr) FUN_143a8caa0(ragdollMgr, &ctx, dt);    // ragdoll follow / freeze (§6.3)
  sys->jobDt = sys->accum;  FUN_14410e5d0(DAT_1460486a0, sys + 0x20);   // schedule job (vtable 0x5893870)
  sys->accum -= floorf(sys->accum / (1/60)) * (1/60);
}
// worker: 0x3A74AC0
wait(scene+0x198);  FUN_143a61b70(scene, {jobDt, 1/60});  schedule(scene+0x1A8);
```
Step 0x3A61B70: `n = min(3, floor(accum/(1/60)))`; per substep: lockWrite, `collide(1/60)`, unlockWrite; on the first
substep every queued kinematic pose is interpolated (position lerp, rotation slerp, fraction `(1/60)/accum`) and applied
with `wrapper->vtbl[+0xB8](pose, true)` (disassembly 0x3A624FC `mov r8b,1; call r9`); then lockWrite,
`fetchCollision(true)`, `advance(0)`, `fetchResults(true)`, unlockWrite.
* Fixed 60 Hz, at most 3 substeps per frame (below 20 fps the simulation slows down), on a job-system worker.
* The write lock is held only during collide and during fetchCollision/advance/fetchResults.

### 3.3 Contact callback (confirmed)
Vtable 0x5894948: onConstraintBreak/onWake/onSleep/onTrigger/onAdvance are `ret`; **onContact = 0x3A91A50**:
for every contact pair whose events contain 0x40 (eNOTIFY_THRESHOLD_FORCE_FOUND) it builds an event
`{vtbl 0x54A40F0, actor0->userData, actor1->userData, which, pairHeader, pair}` and calls
`wrapper->vtbl[+0xA8]()` (the container) `->vtbl[+0x98](&ev)` for both actors. The default container's +0x98 is `ret`;
ragdoll/other containers override it (impact sounds: strings `Movement_Ragdoll_Impact`, `PhysicsCollisionSound`;
consumer unconfirmed). Contacts are reported only above each actor's contact-report threshold (template value,
wrapper +0x140; ragdoll bodies get 0.01, 0x3A8E320).

---

## 4. Physics objects

### 4.1 Creation path (confirmed)
`resMgr = *(void**)(base+0x6047B98)`. `FUN_143b24310(resMgr, &resourceFS, scale, kind)` looks up the template
(`FUN_143b24690`) and calls `FUN_143b24150(resMgr, templates[], &scale, kind)`, which calls the factory
`(*(resMgr+0x18))->vtbl[+0x08]` = **0x3A5F510** and then stores the scale in container+0x54 and calls wrapper +0xC0
on each actor.
Factory 0x3A5F510 `(factory, templates[] {begin,end}, const vec3* scale, int kind)`:
* kind 1 → container vtable 0x58943F8 (0xE0 bytes); every wrapper is then made kinematic (0x3A7E740).
* kind 2 → container from 0x3A60200 (vtable 0x58944E8); also wraps articulation links (wrapper 0x5894258) and joints.
* other → default container vtable 0x5893AD8 (0x70 bytes): `+0x20 = 0xFFC0000000000000` (null EntityHandle),
  +0x50 1.0, +0x54..+0x5C −1, +0x64 0xFFFF, +0x68 −1.
For each template actor of category 2, 4, 1 (template vtbl+0x08 = count, +0x40 = instantiate actor,
+0x10 = name FS), it creates a wrapper, sets `actor->userData = wrapper`, `wrapper->vtbl[+0x150](actor)`,
`wrapper+0x38 = container`, and appends it to `container+0x28` (count +0x34).
Actor creation 0x3A78850: type 1 → createRigidStatic; 2 → createRigidDynamic; 4 → createRigidDynamic +
`setRigidBodyFlag(eKINEMATIC, true)`; copies max velocities, damping, max depenetration velocity, sleep threshold,
contact-report threshold, mass (scaled by sx·sy·sz) and solver iteration counts from the template actor.
Larian physics type values: **1 static, 2 dynamic, 4 kinematic** (8 also enumerated).

### 4.2 Container (base vtable 0x58B40F8; default 0x5893AD8) — confirmed fields and methods
Fields: +0x08 template list (count +0x14), +0x18 scene (main or simple scene object), **+0x20 owning EntityHandle**
(written by creators: `plVar6[4] = param_2[5]` in 0x2D6D200, `plVar14[4] = *(entity+0x38)` in 0x2D6D670,
`plVar4[4] = *param_2` in 0x3A62D00 / 0x3A640B0), +0x28 wrapper array (count +0x34), +0x38 position, +0x44 rotation,
+0x54 scale, **+0x60 group word, +0x64 collision mask, +0x6C extra word** (0x3C95F30).
| vtbl slot (0x5893AD8) | RVA | behaviour |
|---|---|---|
| +0x10 / +0x18 | 0x3C95B60 / 0x3C95BD0 | remove wrapper |
| +0x20 | 0x3C95C40 | AddToScene(scene, batch): calls own +0x28 first, then `scene->vtbl[+0x18]` (main: 0x3A64F10 → 0x3A8A190, bulk add through PxScene +0x60 under lockWrite, applying the static mask of §3.1) |
| +0x28 | 0x3C95CF0 | RemoveFromScene: `scene->vtbl[+0x20]` (main: 0x3A64F40 drops queued kinematic poses, then 0x3A8A4B0 → PxScene +0x70 removeActors) |
| +0x30 | 0x3C95ED0 | SetGroup(group) (keeps +0x64/+0x6C) |
| +0x50 | 0x3C95F30 | SetPhysicsGroup(group, mask, extra) → +0x60/+0x64/+0x6C |
| +0x58 / +0x60 | 0x3C95F50 / 0x3C95F90 | disable / enable simulation on every wrapper |
| +0x70 | 0x3C95D50 | wake every wrapper |
| +0x98 | 0x579D90 (`ret`) | contact event (§3.3) |
Non-virtual: 0x3C95FD0 SetTransform (wrapper +0x08 on each), 0x3C95860 SetTranslation, 0x3C958F0 SetRotation,
0x3C95970 SetScale, 0x3C95D90 StopMotion, 0x3C96240 MakeStatic, 0x3C96280 MakeKinematic (wrapper +0xF0/+0xF8 on each),
0x3C95B10 AddWrapper.

### 4.3 Per-actor wrapper (0x58940D0; static 0x5893F48; articulation link 0x5894258)
Fields: +0x0C quat, +0x1C position (cached world transform), +0x28 scale, +0x38 container, +0x40 `PxRigidActor*`,
+0x48..+0x60 cached initial pose.
| slot | RVA | behaviour |
|---|---|---|
| +0x08 | 0x3A7C220 | **Teleport**(pose): store, lockWrite, `setGlobalPose(pose, autowake=1)` if changed |
| +0x10 / +0x18 / +0x20 | 0x3A7C500 / 0x3A7C550 / 0x3A7C5A0 | set translation / rotation / scale |
| +0x50 | 0x3A7D130 | world bounds (`getWorldBounds(1.01)`) |
| +0x60 | 0x3A7D780 | **StopMotion**: zero linear + angular velocity, clearForce, clearTorque |
| +0x68 / +0x70 | 0x3A7D8A0 / 0x3A7D930 | WakeUp / PutToSleep (dynamic only) |
| +0xA0 | 0x3DE4680 | **GetOwnerEntityHandle** → `&container->+0x20` (static null 0x58C3050 when unowned) |
| +0xA8 | 0x146C980 | GetContainer → `*(this+0x38)` |
| +0xB8 | 0x3A7C380 | **SetWorldTransform(pose, bool asKinematicTarget)** (below) |
| +0xC8 / +0xD0 / +0xD8 / +0xE8 | 0x3A7D340 / 0x3A7D3D0 / 0x3A7D460 / 0x3A7D520 | IsDynamic / IsKinematic / IsStatic / IsArticulationLink |
| +0xF0 | 0x3A7E0B0 | **MakeStatic** (replaces actor with a new PxRigidStatic carrying the same shapes) |
| +0xF8 | 0x3A7E740 | **MakeKinematic** (dynamic: set eKINEMATIC; static: creates a PxRigidDynamic at the same pose, sets eKINEMATIC, copies actor flags, moves every shape old→new (detachShape +0xA8 / attachShape +0xA0), releases the old actor, stores the new one in wrapper+0x40 and swaps them in the scene with removeActor/addActor — all confirmed in the decompile) |
| +0x100 | 0x3A7E3E0 | **MakeDynamic** (dequeue from interpolation map 0x3A628F0; `setRigidBodyFlag(eKINEMATIC,false)`, disasm 0x3A7E47A; static: new dynamic actor) |
| +0x108 / +0x110 | 0x3A7D540 / 0x3A7D5B0 | disable / enable simulation (setActorFlag 8 true/false; disasm 0x3A7D5ED) |
| +0x120 / +0x128 | 0x3A7EA80 / 0x3A7EB40 | **Set/GetLinearVelocity** (autowake 1) |
| +0x130 / +0x138 | 0x3A7EC10 / 0x3A7ECD0 | **Set/GetAngularVelocity** |
| +0x140 | 0x3A7EDA0 | SetContactReportThreshold |
| +0x150 / +0x158 | 0x3A80620 / 0x3A7C1E0 | SetActor / SyncFromActor (getGlobalPose → +0x08) |
`SetWorldTransform` (0x3A7C380):
```c
if (asKinematicTarget) { store quat/pos; build PxTransform (slot +0x168);
                         lockWrite; if (actor type 5) actor->setKinematicTarget(&px); unlockWrite; }
else { scene = container->scene(+0x18);
       if (scene && scene->vtbl[+0x100]())            // main scene returns 1, SimplePhysXScene 0
           { scene->map(+0x118)[this] = {this, pos, quat}; return; }   // applied per substep by the job (§3.2)
       Teleport(pose); }
```
Main scene slot +0x178 (0x3A627C0) writes the same map entry directly: `QueueKinematicPose(wrapper, pos*, quat*)`.
No Larian wrapper method for addForce was found; `PxRigidDynamic::addForce` (+0x170) is reachable through wrapper+0x40.

---

## 5. Scene queries

Query slots (same layout in 0x5891B30 and 0x5893C28); each passes the scene's `PxScene*` to a free function:
| slot | main | simple | implementation | geometry |
|---|---|---|---|---|
| +0x38 | 0x3A64990 | 0x3A77710 | 0x3A85AA0 | raycast closest (§5.1) |
| +0x40 | 0x3A64AB0 | 0x3A77830 | 0x3A86000 | raycast variant (any/all: unconfirmed) |
| +0x48 | 0x3A64B30 | 0x3A778B0 | 0x3A865A0 | raycast variant (unconfirmed) |
| +0x50 | 0x3A64BA0 | 0x3A77920 | 0x3A86DE0 | geometry type 0 = sphere (radius arg) |
| +0x58 | 0x3A64C30 | 0x3A779B0 | 0x3A87170 | (capsule, unconfirmed) |
| +0x60 | 0x3A64C40 | 0x3A779C0 | 0x3A86DE0 | type 3 = box (half extents) |
| +0x70 / +0x78 / +0x80 | 0x3A64CF0 / 0x3A64D80 / 0x3A64D90 | | 0x3A868B0 / 0x3A872A0 / 0x3A868B0 | sphere / (capsule) / box, second family |
| +0x90 .. +0xC0 | jmp 0x3A87390, 0x3A88CC0, 0x3A88B00, 0x3A897C0, 0x3A89110, 0x3A88FC0, 0x3A88E60 | | | overlap / test family |
Server example of +0xC0: 0x360A300 calls `scene->vtbl[+0xC0](scene, pos, radius, outArray, 0xF, 0x400, 0)` and turns each
result into an entity with wrapper +0xA0 (sphere overlap → entities).

### 5.1 RaycastClosest (0x3A85AA0 via slot +0x38) — see §8.2 for the call recipe
Builds `PxQueryFilterData` (word0 = word0, word1 = groupMask & ~excludeMask, word2 = excludeMask, word3 = word3;
flags eSTATIC if queryFlags&1, eDYNAMIC if &2 or &4, ePREFILTER always), `PxHitFlags` 0x403 (0x483 when context is
8 or 0x40), calls `PxScene::raycast` (+0x2B0) under lockRead, and fills `PhysicsHit` (0x30 bytes).

---

## 6. Ragdolls

### 6.1 Activation (death → ragdoll), confirmed chain
1. A character's animation graph contains the node type `PhysicsNode_AddRagdoll` (FixedString global 0x5EF26DC,
   registered 0x38096F0). Its runtime update is **0x3DCD200** (client):
   * finds bone `Root_M` in the skeleton (`skel->vtbl[+0x88](skel,"Root_M",&idx)`), records its position on the first
     frame, and next frame computes the root motion;
   * picks the scene: per-entity override `*(entityData + DAT_146046fcc + 0x68)` or `[[0x6048898]+0x90]+0x30`;
   * does up to two RaycastClosest ground probes (args 5, −1, 0xC, 1, word0, −1); if neither hits it returns;
   * calls **main scene slot +0x118** (0x3A62960) → allocates the 0x80-byte ragdoll handle `{mgr*, ragdollResourceFS,
     ..., short id = 0xFFFF, ...}`;
   * **0x3DD4FC0** builds the descriptor (template from `resMgr->FindTemplate(handle+8)`, entity transform, root pose,
     scale, per-bone arrays) and calls `id = mgr->vtbl[+0x08](&desc)`; stores id at handle+0x10;
   * raises graph event `PhysicsEvent_RagdollAdded` (FS 0x5EF2670).
2. Removal: graph event `PhysicsEvent_RagdollRemovedInternal` (FS 0x5EF2674) → 0x3DCDD40 calls
   `mgr->vtbl[+0x18](id)` (MarkForRemoval) and frees the handle. Node `PhysicsNode_RemoveRagdoll` = FS 0x5EF26E0.
3. Pose feed back to the skeleton: node evaluate **0x3A4E0B0** (`RagdollPose`, FS 0x5EF1C2C `GrannyNode_RagdollPose`)
   calls `mgr->vtbl[+0x28](&flags, id)` then `mgr->vtbl[+0x20](id, handle+0x28)` (disassembly 0x3A4E13D..0x3A4E151:
   `movzx edx, word [r8+0x10]; ... add r8, 0x28; call [rax+0x20]`) and maps the body transforms onto the skeleton
   (bone count from skeleton+0x98). Details of the bone mapping belong to the animation report (agent C).
The `ecl::death::CharacterSystem` (update 0x1956D70 → 0x1957FD0 etc.) also queries `ls::PhysicsComponent` and does
two RaycastClosest probes (5, −1, 0xC, 1, r13d, −1) in 0x1957FD0; its role in placement is unconfirmed.

### 6.2 Ragdoll manager (mainScene+0x90, vtable 0x5894878, 6 slots)
| slot | RVA | function |
|---|---|---|
| +0x00 | 0x3A8AD30 | dtor |
| +0x08 | 0x3A8AF90 | AddRagdoll(desc) → short id (counter 0x5FC3054) |
| +0x10 | 0x3A8B780 | RemoveRagdoll(id) |
| +0x18 | 0x3A8B810 | MarkForRemoval(id): flags |= 2 |
| +0x20 | 0x3A8B880 | GetRagdollPose(id, outArray) |
| +0x28 | 0x3A8CA10 | GetRagdollFlags(out, id) → entry+0x6C |
Entries: array mgr+0x40, 0x70 bytes each, count mgr+0x4C, critical section mgr+0x18.
Entry: +0x00 ragdoll container*, +0x10 entity handle, +0x30..+0x54 last entity transform, +0x58 time limit,
+0x60 elapsed, +0x68 id, +0x6C flags (bit0 frozen, bit1 remove, bit2 re-add to scene).
AddRagdoll builds a 0x98-byte container (vtable 0x58948A8), bodies via 0x3A8E320 and joints via 0x3A8FD70; if `desc+0x70`
is set it calls joint vtbl+0xE8 for axes 4, 5, 3 on every D6 joint (type 0x105) (PxD6Axis eSWING1, eSWING2, eTWIST;
the motion value is in a register not shown by the decompiler — unconfirmed whether it frees or limits them).
Body builder 0x3A8E320: per body `setGlobalPose`, **`setLinearVelocity(desc.linVel[i])`, `setAngularVelocity(desc.angVel[i])`**
(the ragdoll inherits the animated motion), `setContactReportThreshold(0.01)` on the reference body, and if the global
pause byte is set `setActorFlag(eDISABLE_SIMULATION, true)`; finally `container->SetPhysicsGroup(0x4004, 0xE14, 0)`
(the values passed to the scene Init). Ragdoll bodies are rigid dynamics joined by joints and/or articulation links,
depending on the template (both paths exist in 0x3A8E320; which one shipping ragdolls use is unconfirmed).

### 6.3 Per-frame update 0x3A8CAA0 (confirmed)
For each entry (under the manager lock):
* bit2 → `container->vtbl[+0x20](scene, 0)` (re-add), clear bit2.
* `elapsed += dt` unless the pause byte is set.
* Read the entity's world transform (`FUN_140e59570(world, entity)`); if the reference body (index =
  count(type 8) + count(type 2) in the template) differs from it, teleport it to the entity transform and move every
  **sleeping** body by the same delta (lockWrite + setGlobalPose). So an externally moved corpse drags its sleeping bodies.
* AND of `isSleeping()` over all bodies, sum of linear velocities.
* **If all bodies sleep, or elapsed ≥ limit: call MakeKinematic (wrapper +0xF8) on every body once and set bit0.**
  Otherwise call the scene's listener list (scene+0x10) with the average linear velocity.
So corpses settle and freeze into kinematic bodies. To grab one again: MakeDynamic (+0x100) + WakeUp (+0x68) on the
bodies; because bit0 stays set the update does not re-freeze it, and while it stays awake the freeze branch is not taken
(bit0 only guards the MakeKinematic call). The time limit value placed in the descriptor is unconfirmed.

---

## 7. Client vs server (confirmed)

* **Client world** (`*(0x5FDA240)+0x1A8`): world setup 0x70EC90 (via 0x5EBED0 ← vtable slot 0x549A008) registers
  `phx::PhysXScene` (the main simulated scene) and the phx systems (update, ragdolls, cloth, attacher, manual spring).
  The only Init call (0x184FDC0) is client code. Picking (0x184CE60), the animation graph (ragdolls), the death system,
  and the level object `[[0x6007A28]+0x90]+0x30` all use this scene.
* **Server world**: setup 0x21B7EB0 (uses `esv` LevelManager 0x603B2B0, string "ls.thoth.server.condition") →
  0x232A860 → 0x23318B0 registers **`phx::SimplePhysXScene`**, whose actors are always added with
  eDISABLE_SIMULATION. Server code queries it (e.g. 0x360A300 overlap via `[[0x603B2B0]+0x90]+0x30`, then
  wrapper +0xA0 → EntityHandle). No main-scene Init call exists on the server side (searched the constant 0x1A7D2100:
  one hit, 0x184FEED).
* Consequence: **rigid-body dynamics and ragdolls are client-only and cosmetic**. The server never simulates bodies;
  gameplay positions of items and corpses are owned by the server and replicated (replication itself is agent E's area).
  A client-side mod can freely move, throw and ragdoll objects in the client PhysX scene, but the result is not seen by
  the server (or other players) unless the mod also moves the entity through a server path (Osiris / BG3SE server API).
  Whether the client re-snaps a moved item when a server transform update arrives is unconfirmed (look at the client
  transform-replication handler; agent E).

---

## 8. Runtime raycast probe (confirmed recipe; coordinator request)

### 8.1 Finding the main scene at run time
Each path is confirmed to hold a main-scene-class object; whether A, B and C return the same pointer in normal play is
unconfirmed (compare at run time and check `*(void**)obj == base+0x5891B30`).
* **A (what game code uses):** `lm = *(void**)(base+0x6007A28)`; `level = *(void**)(lm+0x90)` (null while loading);
  `scene = *(void**)(level+0x30)`. Evidence: the ragdoll node calls slot +0x38 and +0x118 on it, and +0x118 (0x3A62960)
  copies `*(scene+0x90)` (the ragdoll manager) — only the main class has that. Callers: 0x20B6880, 0x3F47340, 0x3DCD200,
  0x1FE35C0, 0xE42D20. Per-entity variants: `FUN_140f31fb0(lm, entity+0x18)` returns the entity's level (0x198B680).
* **B:** `world = *(void**)(*(void**)(base+0x5FDA240)+0x1A8)`;
  `p = *(void**)(*(void**)(world+0x30) + *(uint32*)(base+0x6058CEC) * 0xF8)`; `scene = p ? p-0x80 : 0` (0x184CE60).
* **C:** `g = *(void***)(base+0x60490E8)`; `g[0]` = active scene (written by 0x184E850 from the object in B; also by
  0x1864E80/0x424E1F0, cleared on unload); `g[1]` written by 0x184FDC0 after Init, cleared by scene dtors.
* **D:** `phys = *(void**)(base+0x5F82468)`; `n = phys->vtbl[+0x90](phys)`; `phys->vtbl[+0x98](phys, buf, n, 0)`;
  Larian scene = `*(void**)(pxScene+8)`; pick vtable base+0x5891B30.

### 8.2 RaycastClosest call
```c
// rcx = scene, rdx = from[3], r8 = to[3], r9 = hit; stack: [rsp+0x20] queryFlags, +0x28 groupMask,
// +0x30 excludeMask, +0x38 context, +0x40 word0, +0x48 word3, +0x50 filter (non-null!)
bool RaycastClosest(void* scene, const float* from, const float* to, PhysicsHit* hit, uint32 queryFlags,
                    uint32 groupMask, uint32 excludeMask, int32 context, uint32 word0, uint32 word3, void* filter);
// = (*(fn**)(*(void**)scene + 0x38))(...)
```
* `filter` must point to 0x40 zero bytes (dereferenced; non-zero first qword = callable manager, destroyed by callee).
* Returns 0 without querying if from == to.
Observed argument sets (disassembly):
| caller | queryFlags | groupMask | excludeMask | context | word0 | word3 |
|---|---|---|---|---|---|---|
| 0x1FE35C0 @0x1FE37D0 (vertical probe through a point) | 0xF | 0xCE30 | 0x80010080 | 1 | −1 | −1 |
| 0x1957FD0 @0x1958B8A, @0x1958D9B (client death system) | 5 | 0xFFFFFFFF | 0xC | 1 | r13d | −1 |
| 0x3DCD200 (AddRagdoll ground probe) | 5 | 0xFFFFFFFF | 0xC | 1 | `*(FUN_143cbf0b0(entity)+0x68)` or −1 | −1 |
| 0x20B6880 (ground probe, accepts normal.y ≥ 0.25) | 0xF | 0x20 | 0x80 | 1 | −1 | −1 |
| 0x3F47340 (anim-graph raycast node) | 0xF | graph var | graph var | 1 | −1 | −1 |
Group-bit names are unconfirmed. A cursor-picking raycast call was not traced (the picking object 0x184CE60 caches the
scene at +0x30).
`PhysicsHit` (0x30 bytes; zero it, set +0x18 = 0x7F7FFFFF):
| off | meaning |
|---|---|
| +0x00 | normal (vec3) |
| +0x0C | position (vec3) |
| +0x18 | distance / |to−from| (0..1); FLT_MAX on miss |
| +0x1C | container group word (+0x60) |
| +0x20 | container extra word (+0x6C) |
| +0x28 | hit wrapper (actor userData); 0 on miss |

### 8.3 Threads
The callee takes the scene read lock itself (0x4AB0B40). The physics job holds the write lock only around collide and
around fetchCollision/advance/fetchResults, so a reader blocks during those phases. The game raycasts from
animation-graph nodes and gameplay code with no other synchronisation. Calling slot +0x38 from the game thread (e.g. the
camera update 0x4085150) uses the same protection. Whether PhysX is fully safe for queries in the window between
collide and fetchCollision (no write lock held) is PhysX-internal and not verified here.

### 8.4 From a hit to the entity
`wrapper = hit+0x28`; `container = *(void**)(wrapper+0x38)`; `entity = *(uint64*)(container+0x20)` (equivalently
`*(uint64*)wrapper->vtbl[+0xA0](wrapper)`). Confirmed by the writers listed in §4.2 and by server code 0x360A300, which
calls `+0xA0` on overlap results and validates the value as an EntityHandle (`h>>0x36 < 0x41 && h != 0xFFC0000000000000`).

---

## 9. Objects in the world: physics creation and group flags

* 0x2D6D200 (`FUN_142d6d200(template, out)`): `container = Instantiate(resMgr, template+0x7C /*PhysicsTemplate FS*/,
  maxScale, kind 0)`, `container->entity = out[5]`, SetTransform, **MakeStatic** (0x3C96240), then composes the group
  word: base 0x200000 if `template+0x112` else 0x800, plus 0x1000020 (+0x102), 0x20 (+0x108), 0x1000 (+0xDC),
  0x2000 (+0x10A), 0x10000 (+0x10C), 0x20000 (+0xDE), 0x800000 if `+0x100 == 0`, 0x40000000 (+0x116), 0x40000 (+0x10E),
  0x400000 for one enum value; `container->vtbl[+0x30](group)`; an extra word from a 0..4 enum (+0x11A) → 2/4/8/0x10/0x20,
  plus 0x40 (+0x118) via `vtbl[+0x48]`; stores the container into the output slot. Field names are unconfirmed
  (the template is a game-object template; BG3SE exposes it if names are needed).
* 0x2D6D670 creates one or two objects (second from `FUN_143dc47f0(...)`) and sets entity, translation, rotation.
* Visual-attached physics (cloth / skinned collision): main scene slots +0x140 (0x3A62D00), +0x148, +0x150, +0x158,
  +0x160 (0x3A640B0), +0x168 walk an `ls::Visual` (+0x88 object list, +0x91 count, +0xC8 parent, +0xE0 scene back-pointer)
  and instantiate physics resources for it.
* Statics whose group word intersects the scene mask 0x1A7D2100 are inserted with eDISABLE_SIMULATION (query-only).
* **Items lying in the world have static (query) bodies created this way; no client system was found that turns an item
  into a simulated body.** Item "drop / throw" arcs are therefore not PhysX simulations (projectile code
  ecl::ProjectileManager queries `ls::PhysicsComponent` but no rigid-body call was found in its update; unconfirmed in
  detail).
* No code path was found that copies a dynamic body's pose back into an item's ECS Transform (only ragdolls feed poses
  back, through the animation graph). A mod that simulates an item must move the item's visual/transform itself.

---

## 10. Recommended approach for VR hand interaction

Hand-held objects (HIGGS style):
1. Find the target: RaycastClosest / sphere overlap on the main client scene (§8). From the hit wrapper get the
   container and the entity (§8.4).
2. Hold: for each wrapper of the container call **MakeKinematic** (+0xF8) once; then every frame call
   **`SetWorldTransform(handPose, false)`** (+0xB8) from the game thread — the main scene queues it and the physics job
   turns it into per-substep `setKinematicTarget`s, so the held body pushes dynamic bodies correctly. Alternative:
   `SetWorldTransform(pose, true)` = immediate kinematic target (single target per frame).
3. Release / throw: **MakeDynamic** (+0x100), **SetLinearVelocity** (+0x120) and **SetAngularVelocity** (+0x130) with the
   hand's velocity, **WakeUp** (+0x68). For gameplay-static objects (items) the factory created PxRigidStatic actors;
   MakeKinematic replaces them with a new dynamic actor carrying the same shapes (fully confirmed in 0x3A7E740);
   MakeDynamic's static branch (0x3A7E3E0) starts the same way (createRigidDynamic, copy actor flags, shape list) —
   test on scenery before items.
4. Visual sync: physics does not drive the item's ECS Transform (§9); copy the body pose (wrapper +0x0C/+0x1C or
   `actor->getGlobalPose`) to the item's transform/visual each frame, and commit the final position through a server API
   so the game state matches.
5. Threading: do all of this on the game thread; Larian's wrapper methods take the PhysX write lock themselves. Raw
   PhysX calls must take `lockWrite` on `actor->getScene()`.

Ragdoll grabbing:
1. Find a body: raycast; `hit+0x28` is the ragdoll body wrapper (its container is the ragdoll container 0x58948A8).
   Map container → ragdoll id by walking `mgr->entries` (mgr = mainScene+0x90, entries +0x40, stride 0x70, container
   at +0x00, id at +0x68) under the manager's critical section (mgr+0x18).
2. If flags bit0 is set (frozen), call MakeDynamic (+0x100) + WakeUp on every body of the container.
3. Drive the grabbed bone: either make that one body kinematic and drive it with `SetWorldTransform(pose,false)`, or
   (Bonelab-like) leave it dynamic and set its linear/angular velocity toward the hand target each frame (+0x120/+0x130).
   The animation graph keeps reading the pose through GetRagdollPose (0x3A4E0B0), so the visible corpse follows.
4. Throw: set velocities on release. The time-limit freeze (§6.3) will eventually make it kinematic again; reset
   `entry+0x60` (elapsed) under the lock to extend it (field confirmed, effect of writing it unconfirmed by test).
5. Corpse position on the server will not change; moving the corpse entity itself requires a server-side call.

Sounds/damage: impacts above the contact-report threshold reach `container->vtbl[+0x98]` (§3.3); no physics-to-damage
path was found (impact damage would have to be implemented by the mod via game APIs).

---

## 11. Method notes
* Vtables were located by string → referencing function → `.rdata` data reference → slot index checked against the PhysX
  4.1 declaration order; every vtable has at least two independent anchors.
* Function-level helpers: `py/strs.py` (string scan), `py/sig.py` (byte signature), `py/vcall.py` (functions calling given
  vtable offsets, via .pdata), `py/leascan.py` (RIP-relative refs into an address range), `py/rtti.py`.
* The BG3SE DLL in `E:\Modding\BG3ScriptExtender\Cache` was only used for its published byte signatures of the two
  LevelManager globals and as a list of names (e.g. `RaycastClosest`, `PhysicsGroupFlags`); every fact above was
  confirmed in `bg3_dx11.exe` itself.

## 12. Open items
* Meaning/names of group bits (0x800, 0x200000, 0x20, 0x4004, 0xE14, 0x1A7D2100, 0xCE30, 0x80010080).
* Exact semantics of query slots +0x40/+0x48 (raycast any/all) and the overlap family +0x90..+0xC0.
* `ls::PhysicsComponent` memory layout (which field holds the container) — creators found (§9), the component field not.
* The client's reaction to server transform updates for items (agent E).
* Ragdoll descriptor time-limit value; D6 joint motion values in AddRagdoll; which ragdoll templates use articulations.
* The cursor-picking raycast call site; owner of the small per-entity scene 0x5893BD8.
