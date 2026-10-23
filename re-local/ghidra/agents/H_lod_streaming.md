# BG3 LOD, streaming and model quality (4.1.1.7631656) - agent H report

Static analysis only (Ghidra project `BG3_H`, headless; raw decompiles and xref dumps in
`re-local\ghidra\agents\H\`, file names given per section). RVAs relative to image base 0x140000000.
"SE" = name/layout taken from the Script Extender source (`downloads\bg3se-src`). Builds on
A_rendering.md (renderer, views, stages), C_animation_systems.md (Visual layout, Granny) and
B_splitscreen.md (GlobalSwitches = `[0x604B1B8]`).

Status (2026-10-19): sections 1-8 complete as far as listed; open items in "Open leads".

## Hook points for the VR mod

Key fact: every quality option is already at its best value on this machine (graphicSettings.lsx has no
quality keys, the code defaults are all "best", 1.3), and **all options are clamped at load to the
in-game range; there is no "beyond Ultra" through settings**. What still lowers quality near the player
in VR is the **camera FOV and eye resolution feeding the LOD and texture metrics** (2.3, 4.2), the
**texture cache budget ceiling** (4.1) and the detail cull (3). The table lists the levers in order of
usefulness.

| need | RVA / address | how to use | confidence |
|---|---|---|---|
| Mesh LOD distances not shrunk by the wide eye FOV | 0x3CDA5D0 `lodFovFactor(fov) = tan(27.5deg)/tan(fov/2)`, called from the scene cull 0x3CD1683 with `[[view+0x21D8]+0x14]` | Either keep each eye-view camera's `+0x14` FOV at a desktop value (<= 55 deg; the stereo hook already rewrites the PerCamera matrices, A 1.7, so the camera's own FOV is free) or hook 0x3CDA5D0 and return a constant >= 1 (e.g. 4.0 = all LOD switch distances x2). The factor multiplies squared distances. | Confirmed |
| Force highest mesh LOD always | `GlobalSwitches = [0x604B1B8]`, byte `+0x126` | Write 0: the scene cull then keeps only renderables with MinLODDistance == 0 (LOD0) and drops the max-distance test, so every visual draws LOD0 at any distance. Per-frame safe (plain data, read in every query 0x3CD1657). Cost: no distance culling of LOD0 meshes at all. | Confirmed (code); visual effect Unconfirmed (needs a run) |
| Larger LOD bands without touching FOV | Visual slot 21 SetLODDistances 0x3C6AA20 (writes renderable `+0x70/+0x74`), ModelDetail factor 0x3C6A940 | Hook 0x3C6A940 to return `max(scale) * N` instead of `* 1.0`; or multiply `+0x70/+0x74` (Min/MaxLODDistance) after 0x3C6AA20 runs. Only applies to renderables (re)added after the hook. | Confirmed |
| Texture residency level not lowered by FOV / eye resolution | stage 0x3E job 0x3E51DE0: `k = clamp(sqrt(w^2+h^2)/tan(fov/2)/2200, 0.1, 10)`; level calc 0x3C48E90 uses `k * [cache+0x2404] * [cache+0x20]` | Write `TextureCache+0x2404` (float, default 1.0, only written by the ctor 0x750970) to 2..4: every level boundary moves out by that factor. Same effect from the FOV fix above (k uses camera vtable +0x18 FOV). | Confirmed |
| Texture cache budget ceiling | 0x3C48520 writes `TextureCache+0x10` = `(clamp(VRAM,2..12 GiB)/16 GiB + 0.5) * {1 GiB,512,384,256 MiB}[TextureDetail]` (max 1.25 GiB); 0x3C48690 shrinks `+0x20` x0.95 per frame while usage+16 MiB > budget (clamped 0.01..1.0) | Write `TextureCache+0x10` (int64 bytes) to e.g. 3-4 GiB after init (0x3C48390), or hook 0x3C48520. Keeps `+0x20` at 1.0. | Confirmed |
| TextureCache instance | `[[[0x6046F98]+0x1D8]+0x30] + [0x6059304]*0xF8` (ECS system slot; type index 0x6059304) | Read the pointer once after the world exists (same lookup as 0x3C6AC00 / 0x3C62190). | Confirmed |
| Small-object (detail) culling off | `GlobalSwitches+0x15` (byte, 1) and thresholds `+0x74/+0x78/+0x7C` (0.016/0.018/0.02) | Write `+0x15 = 0` to disable, or lower the thresholds. Also used (minus 0.002) by visual streaming 0x3D04730. | Confirmed |
| Animation (skeleton) LOD | Granny `GetBoneCountForLOD` via 0x3A051F0 / 0x3A05050; pixel error from AnimationLODDetail (0x3E: 0 -> 0 px) | Already full skeleton at option 0 (error 0 -> all bones, 0x4A1F960). For safety write `[0x604AA10+0x3E] = 0`. | Confirmed |
| Animation update time-slicing | 0x19E48A0 / 0x19E5110: process entity only if `id % N == frame % N`, N = 1/2/4 by AnimationLODDetail | Keep option 0 (N = 1). | Confirmed |
| Instanced scenery draw distance | 0x3ECCA30, table 0x58D3088 {60,65},{40,45},{20,25} m by InstanceDistance | Patch the table (data, .rdata) or hook 0x3ECCA30; option 0 is the 60-65 m row. | Confirmed |
| Settings object | `[0x604AA10]`: +0x36 TextureDetail, +0x37 TerrainTextureDetail, +0x38 TextureFiltering, +0x3C ModelDetail, +0x3D DetailDistance, +0x3E AnimationLODDetail, +0x3F InstanceDistance, +0x41 SlowHDD, +0x42 DynamicCrowds, +0x43 ShadowQuality | Loader 0x3ABDE40 (clamps), defaults 0x3ABD930, save 0x3ABFB90. Write fields after load; several consumers cache them (ModelDetail via 0x3E59680 / 0x3EAF5E0 change checks, ShadowQuality via stage 0x1E +0x2182). | Confirmed |
| Shadow map resolution | view config records `[view+0x2B70 + ShadowQuality*0x20]` +0x00 / +0x04 (stage 0x1E 0x3E038A0 / 0x3E03950), also stage 0x35 (A 9) | ShadowQuality 0 picks record 0; bigger maps need the view config patched before the view is built (0x582BD0 caller). | Confirmed (index); values Unconfirmed |

---------------------------------------------------------------------------------------------------------

## 1. Graphics options: storage, load, ranges

### 1.1 Files and the in-memory object

* File: `%LOCALAPPDATA%\Larian Studios\Baldur's Gate 3\graphicSettings.lsx` (on this machine a junction to
  `E:\Modding\LarianData\Baldur's Gate 3\graphicSettings.lsx`). LSX `region Config / node root / children
  ConfigEntry {MapKey FixedString, Type int32 (0 int, 2 float), Value}`. The file holds only 11 entries
  (AntiAliasing=3, CASEnabled, GraphicSettingsVersion=5, refresh rate, 1920x1080, VSync, TripleBuffering,
  UpscaleSharpness, ShowGammaCalibration); every quality option is therefore at its code default (1.3).
  Confirmed (file read).
* In memory: graphics options object `[0x604AA10]`; config map at `[app+0x170]` (hash map
  FixedString -> {type, value}).
* Option name FixedStrings are static globals registered by tiny initialisers; the pointer global for
  each name is what the code reads (`dec_optreg.c`, e.g. 0x4DFA90: `DAT_14613aee0 = "ModelDetail";
  DAT_145fccc08 = &DAT_14613aef8;`). Confirmed.

### 1.2 Functions (Confirmed, `dec_gfx_io.c`)

| rva | role |
|-----|------|
| 0x3ABD930 | write defaults into the config map (`0x411AAA0(map, name, int)`, `0x411AB80` for floats) |
| 0x3ABDE40 | load: config map -> `[0x604AA10]` fields, with clamps (1.3); sets `[+8] = 5` (settings version) |
| 0x3ABFB90 | save: `[0x604AA10]` fields -> config map |

These three are the only readers of the option-name globals (`xref_optfs.txt`).

### 1.3 Quality options (Confirmed, 0x3ABDE40 / 0x3ABD930 / `gfx_users.txt`)

All are "0 = best" except TextureFiltering (6 = best). Every value is clamped at load (0x3ABDE40), so
"beyond Ultra" values cannot come from the file; direct memory writes of out-of-range values are not
safe either because several consumers index tables with them (TerrainTextureDetail, InstanceDistance,
the texture budget table) or test `== 1 / == 2` only (ModelDetail: any other value = 1.0, i.e. no gain).

| option (name global) | field `[0x604AA10]+` | load clamp | default | consumers |
|---|---|---|---|---|
| TextureDetail (0x5FCCCD8) | byte +0x36 | 0..3 | 0 | 0x3C48390/0x3C48520 (cache budget), 0x3C49110 (cache entry quality), 0x3E53990, 0x407CA50/0x407D350/0x407D730 (texture load), 0x3AF3BB0, 0x209E820, 0x20A1C00, 0x1B79AC0, 0x1B79B70, 0x1B7A3A0, 0x1B7A960, 0x3CCB670..0x3CCC470 (view environment textures) |
| TerrainTextureDetail (0x5FCCCE0) | byte +0x37 | 0..3 (version < 5: copied from TextureDetail) | 0 | 0x3D4CDA0, 0x3D4D790, 0x3D4D9A0, 0x3D4DEA0, 0x3D4E240: tables 0x58B9A80 {1024,512,256,128} and 0x58B9A90 {11,10,9,8} (terrain texture size / mip count) |
| TextureFiltering (0x5FCCCE8) | int +0x38 | 0..6 | 6 | 0x3B183F0: selects sampler indices `[0x5EF2088..208B]` / `[0x5EF2098..209B]` into the sampler table 0x5EE0450 (6 -> 0x18..0x1B) |
| ModelDetail (0x5FCCC08) | byte +0x3C | 0..2 | 0 | 0x3C6A940, 0x3D0E730, 0x3ECCA30 (factor 1 / 0.66 / 0.5), 0x3D0CA10, 0x3F20790 (low-detail asset swap at 2), 0x3E59680, 0x3EAD950, 0x3EAF5E0 |
| DetailDistance (0x5FCCB50) | byte +0x3D | 0..2 | 0 | 0x3CD0FA0 (detail cull), 0x3D04730 (visual streaming) |
| AnimationLODDetail (0x5FCCAD0) | byte +0x3E | 0..2 | 0 | 0xE87D30 (Granny pixel error 0/3/6), 0x19E48A0, 0x19E5110 (update every 1/2/4 frames), 0x3F12AA0 (quality 3/2/1 passed to an animated-object vtable +0x138) |
| InstanceDistance (0x5FCCBF8) | byte +0x3F | 0..2 | 0 | 0x3ECCA30, 0x3AFC590, 0x3AFCFF0, 0x3AFF580 (table 0x58D3088) |
| SlowHDDMode (0x5FCCCB8) | bool +0x41 | | 0 | 0x19D9A60, 0x1D46880, 0x3D45040 (compared with +0x40) |
| DynamicCrowds (0x5FCCB68) | bool +0x42 | | 1 | 0x35384E0 (crowd radius^2 = `(2*[0x5ED9EB8])^2`, 0 when off), 0x3539510 |
| ShadowQuality (0x5FCCC98) | byte +0x43 | 0..2 | 0 | stage 0x1E (0x3E01560/0x3E015F0), stage 0x35 (A 9), 0x3CD1E80 (`< 2` gate on one shadow object list) |
| CloudsQuality (0x5FCCB00) | byte +0x49 | 0..3 | 0 | |
| FogQuality (0x5FCCB88) | byte +0x4A | 0..3 | 0 | |
| ClothGPUAcceleration (0x5FCCAF8) | bool +0x2A | | 1 | |
| (unnamed) | byte +0x76 | | | 0x3D0CA10, 0x3F20790: treated like ModelDetail == 2 (keep 0) |

`TexelDensityMin/Max/Ideal/Extreme` FixedStrings exist (0x4E1C70..0x4E1A30) but nothing reads them
(only the initialisers and atexit destructors reference them) - dead options. Confirmed.

The options are global (one object), not per view.

### 1.4 GlobalSwitches fields used by culling (Confirmed, base ctor 0x40F7620, `dec_gsbase.c`)

`[0x604B1B8]` (ls::GlobalSwitches). Defaults written by 0x40F7620 (first call of 0xEF32B0):

| field | default | meaning (from the readers) |
|---|---|---|
| +0x15 byte | 1 (`+0x14 = 0x0101`) | enables the small-object ("detail") cull (3) |
| +0x74 / +0x78 / +0x7C float | 0.016 / 0.018 / 0.02 | detail-cull threshold per DetailDistance 0/1/2 |
| +0x80 / +0x84 / +0x88 float | 50 / 50 / 50 | detail-cull fall-off distance per DetailDistance |
| +0x126 byte | 1 (`+0x124 = 0x01010000`) | enables the per-object LOD distance test (2.2); 0 = LOD-0 only, no max test |

Only 0x3CD0FA0 and 0x3D01720 read `+0x126` (`gs_disp.txt`). The GlobalSwitches loader 0x5D0360
(`dec_gs.c`) does not touch +0x15/+0x74..+0x88/+0x126, so they are not user-configurable. Confirmed.

---------------------------------------------------------------------------------------------------------

## 2. Mesh LOD selection

### 2.1 Model: every LOD is a separate renderable of the Visual with a distance band

* A `ls::Visual` (vtable 0x588CB30) holds every LOD mesh as its own renderable in `ObjectDesc[]`
  (`+0x98`, count `+0xA4`, 0x10 bytes: renderable, LOD byte +8, flags +9) and the LOD switch distances in
  `LODDistances` (SE `Array<float>` at `+0xA8`; data `+0xA8`, count `+0xB4`).
* Visual vtable (dumped with `vt.py`, decompiles in `dec_visual_vt.c`), Confirmed:
  slot 12 = 0x3C6AC00 texture LOD request (4.1), slot 13 = 0x3A08790 returns `[+0xF4]` (cull flags),
  slots 14/15 per-view hide (C 2.2), slot 20 = **AddObject 0x3C62A10** (appends `{r, LOD = [r+0xD9],
  flags}`, sets `r+0x90 = visual`, calls slot 21), slot 21 = **SetLODDistances 0x3C6AA00** ->
  0x3C6AA20, slot 22 = 0x3C67350 (ray pick), slot 23 = 0x3C64730 (UpdateCullFlags).
* 0x3C6AA20(r, &LODDistances), Confirmed:
  ```c
  lod   = *(byte*)(r + 0xD9);                       // RenderableObject::LOD (SE)
  scale = 0x3C6A940(r + 0x24);                      // max(1e-8, scale.xyz) * ModelDetailFactor
  r->MinLODDistance (+0x70) = lod == 0 ? 0 : scale * LODDistances[lod-1];
  r->MaxLODDistance (+0x74) = scale * LODDistances[lod];
  if (r+0xC0) 0x3ECCA30(r, LODDistances[count-1]);  // instanced: InstanceDistance clamp (2.5)
  ```
  `r+0x24` = WorldTransform.Scale (SE MoveableObject: vtable, Transform{quat +0x08, translate +0x18,
  scale +0x24}); `+0x70/+0x74` = SE `MinLODDistance/MaxLODDistance`.
* ModelDetailFactor (0x3C6A940, also inline in 0x3D0E730 / 0x3ECCA30): `[+0x3C] == 1 -> 0.66`,
  `== 2 -> 0.5`, else **1.0** (the maximum the code can produce). Confirmed.

So mesh LOD is not chosen by screen size per frame: each LOD renderable is visible only while the
camera-to-bounds distance lies in its [Min, Max) band; the band test is in the scene cull.

### 2.2 Band test: scene cull 0x3CD0FA0 (Confirmed, `dec_lod2.c` and disassembly)

Scene cull query 0x3CD1E80 -> per object type 0x3CD0FA0 (SIMD over the SE `SceneObjects` arrays). After
the frustum (query type `[q+0x34] == 1`) or sphere (`== 2`) test, when `(flags & 2) == 0`:

```c
if (GlobalSwitches[+0x126] == 0)            // LOD test disabled
    culled |= (MinLODDistSq[i] != 0);      // only LOD-0 renderables survive, no max test
else {
    f = 1.0;
    if ([[query+0x168] + 0x21D8] != 0)     // the query's render view -> its camera (A: view+0x21D8)
        f = 0x3CDA5D0([camera + 0x14]);    // FOV factor (2.3)
    d2 = squared distance from camera pos (query+0x28/+0x2C/+0x30) to the object's AABB (0 inside)
    culled |= d2 <  f * MinLODDistSq[i];
    culled |= f * MaxLODDistSq[i] <= d2;
}
```

Disassembly 0x3CD1650..0x3CD16A9: `mov rax,[0x604B1B8]; cmp [rax+0x126],bl; jz ...;
mov rax,[r15+0x168]; movss xmm0,[0x591CDE8] (=1.0); mov rcx,[rax+0x21D8]; test rcx,rcx; jz;
movss xmm0,[rcx+0x14]; call 0x3CDA5D0; ... movss xmm5,[r15+0x28]; xmm6,[r15+0x2C]; xmm7,[r15+0x30]`.

* **Camera position / view**: every caller fills query+0x28 with the camera's world position (camera
  vtable +0x48) and query+0x168 with the render view. Confirmed for the CullStage 0x3E246B0
  (`dec_cullstage.c`: `uStack_180 = *(cam->vt+0x48)()` at query+0x28, `local_40 = view` at +0x168,
  type 1, mask 0x201/0x401/1 by `[view+0x2268]`) and stage 0x3E (4.2). So **LOD bands are evaluated per
  render view with that view's camera**; a second (eye) view picks its own LODs from its own camera and
  FOV.
* **FOV**: `[camera+0x14]`, radians (halved and passed to `tanf`); it is the FOV that A 3.3 lists for the
  jittered projection 0x3CCCFD0 (used with an aspect, i.e. vertical; Unconfirmed that it is vertical).

### 2.3 The FOV factor 0x3CDA5D0 (Confirmed, `dec_cull1.c`)

```c
float lodFovFactor(float fov) {           // fov clamped to [1e-6, pi]
    return tanf(0.47996554f /*27.5 deg*/) / max(tanf(fov * 0.5f), 1e-8f);
}
```

Reference FOV 55 deg -> 1. It multiplies the **squared** band limits, so distances scale by `sqrt(f)`.
For an eye camera with `[camera+0x14]` = 100 deg: f = 0.5206/1.1918 = 0.437 -> LOD switch distances
x 0.66 (lower LODs appear 34 % closer than at 55 deg); 90 deg: x 0.72. The same function supplies PerView
`+0x18C` (A 3.3). If the mod sets the eye cameras' FOV to the HMD FOV, LOD quality drops; keeping the
camera FOV at <= 55 deg (or hooking this function) avoids it.

### 2.4 ModelDetail also swaps assets (Confirmed, `dec_lod2.c`)

0x3D0CA10 and 0x3F20790 (object/instance setup) pick an alternative resource GUID (`param+0x30/+0x38`
instead of `+0x20/+0x28`) when `ModelDetail == 2 || [0x604AA10+0x76] != 0`. At 0 the normal assets are
used. Which template field this is is Unconfirmed (callers of 0x3D0CA10 / 0x3F20790 would name it).

### 2.5 InstanceDistance (instanced scenery), 0x3ECCA30 (Confirmed; table read from the exe)

Table 0x58D3088 `{fadeStart, fadeEnd}`: ID 0 -> {60, 65}, 1 -> {40, 45}, 2 -> {20, 25} m. For renderables
with `r+0xC0` (instancing data): `range = table[ID]` (+5 m each when the bound extent
`[r+0x50]-[r+0x44] >= 5`); if the visual's last LOD distance `L != FLT_MAX`: `range = {L*k - 5, L*k}`
with k = 1 / 0.66 / 0.5; `0x3CE8740(r+0xC0, 0x5FCF4F0, &range)` stores it, then
`r->MaxLODDistance = min(r->MaxLODDistance, range.end)`. 0x3AFC590 / 0x3AFCFF0 / 0x3AFF580 read the same
table (`+5.0`) for other instance types (not decompiled in full).

### 2.6 HLOD / clusters (partial)

* 0x3D0E730 (Confirmed): HLOD level distance `level 0 -> 40, 1 -> 65, 2 -> 120, else 50` m times
  ModelDetailFactor. 0x3E59680 (Confirmed): when ModelDetail changes (cached at `+0x50`), recomputes per
  HLOD entity `MinSq = max(0, D(level-1) - 1.1*radius)^2`, `MaxSq = (D(level) + 1.1*radius)^2` into two
  component arrays (SE `ClusterDistMin/MaxComponent`), then 0x3E59A90, 0x3E5A410, 0x3E5AC10, 0x3E5A700,
  0x3E5BC80, 0x3E5C0C0 (not decompiled). 0x3EAF5E0 similarly rebuilds on ModelDetail change (0x3EB3040).
* ECS systems (type-name string -> type index global -> registration, `typeid_systems.txt`), all wired
  in 0x3AE0200: ls::CullSystem 0x3AE5E20 (update 0x3AE8880 -> 0x3EB9370), ls::VisualStreamSystem
  0x3AE2710 (update 0x3AE7800 -> 0x3D03800, 0x3D03BE0, 0x3D02CE0, 0x3D03160, **0x3D04730**, 0x3D05650),
  ls::VisualStreamCullDistanceChangeRequestSystem 0x3AE22F0 (update 0x3AE7760 -> 0x3E58B00),
  ls::VisualStreamRequestSystem 0x3AE29B0, ls::VisualStreamDataSystem 0x3AE24B0, ls::ClusterSystem
  0x3AE2B50, ls::ClusterVisibilitySystem 0x3AE2E50, ls::ClusterVisualLoaderSystem 0x3AE32C0,
  ls::HLODSystem 0x3AE66D0, ls::InstancingSystem 0x3AE62B0, ls::OcclusionSystem 0x3AE58D0,
  ls::VisualLoaderSystem 0x3AE1C30. Confirmed (addresses); bodies other than those cited not decompiled.
* Visual streaming 0x3D04730 (Confirmed, `dec_tex3.c`): builds load spheres from the stream cameras
  (per camera entry radius `[entry+0x128] + 5`), grid-hashes bounds, and passes the detail-cull
  threshold minus 0.002 (`th - 0.002`, from GlobalSwitches +0x74.., gated by +0x15) to 0x3D05FB0, i.e.
  visuals are streamed in slightly before the detail cull would show them. Where the stream radius
  comes from is Unconfirmed (writer of entry +0x128).

---------------------------------------------------------------------------------------------------------

## 3. Small-object ("detail distance") culling

### 3.1 Metric (Confirmed, 0x3CD0FA0 -> 0x3F18E40, `dec_lod2.c`, `dec_tex2.c`)

In 0x3CD0FA0, for frustum queries and when `GlobalSwitches[+0x15] != 0`:

```c
i  = clamp(DetailDistance [0x604AA10+0x3D], 0, 2);
th = GlobalSwitches[+0x74 + 4*i];          // 0.016 / 0.018 / 0.02
d0 = GlobalSwitches[+0x80 + 4*i];          // 50.0
if ((short)queryFlags < 0) th *= 4;        // flag 0x8000 (SE CullShadow4)
else if (queryFlags & 0x803E) th *= 2;     // shadow passes
```

0x3F18E40 culls an object (after the frustum planes) when
`radius / max(dist - radius, eps) <= th * min(dist / d0, c)^10` and the object's per-object byte != 1
(exempt). `c` is a .bss constant (0x6142D40, filled at run time; Unconfirmed value). The test is purely
angular (no FOV or resolution term), so the eye FOV does not change it; with the `^10` fall-off it only
bites near and beyond d0 = 50 m.

### 3.2 Override

`GlobalSwitches[+0x15] = 0` disables it; smaller `+0x74..+0x7C` thresholds reduce it. Plain data read per
query. DetailDistance 0 is already the smallest of the three built-in thresholds.

---------------------------------------------------------------------------------------------------------

## 4. Texture streaming (non-virtual textures)

Three mechanisms: (a) per-object texture residency levels (ls::TextureCache, driven by render stage
0x3E), (b) the mip drop applied when a texture is loaded (TextureDetail), (c) Granite virtual textures
(5).

### 4.1 ls::TextureCache (Confirmed, `dec_tex1.c`, `dec_tc.c`, `dec_tcupd.c`, `disp_texcache.txt`)

* Construction 0x750970 (system registration; object 0x2410 bytes, vtable 0x58B18D8 = {dtor 0x76D060,
  init 0x3C48390}); update callback 0x765180 -> **0x3C48690**. Instance lookup: `[[[0x6046F98]+0x1D8]+0x30]
  + [0x6059304]*0xF8`.
* Fields: `+0x10` budget bytes (ctor 256 MiB, then 0x3C48520), `+0x18` bytes in use (sum of entry
  `+0x48`, recomputed by 0x3C48690), `+0x20` float adaptive scale (ctor 1.0), `+0x2404` float scale (ctor
  1.0; the only writer is the ctor), `+0x23F0` request map, `+0x23F8` dirty list, `+0x50` stream manager.
* **Budget 0x3C48520** (bytes into `+0x10`):
  ```c
  vram = 0x4076A00();   // DXGI_ADAPTER_DESC.DedicatedVideoMemory (QI IDXGIDevice -> GetParent -> GetDesc, +0x110)
  v = clamp(vram, 2 GiB, 12 GiB);  (4 GiB if the query fails)
  budget = (v / 16 GiB + 0.5) * {1 GiB, 512 MiB, 384 MiB, 256 MiB}[TextureDetail];
  ```
  At TextureDetail 0: 1.0 GiB (8 GiB card) .. 1.25 GiB (>= 12 GiB card). Hard ceiling in code.
* **Update 0x3C48690**: per dirty texture, the wanted level is the **lowest (best) level 0..2 that has a
  non-zero refcount** (levels counted per requester in `entry+8+4*level`; max levels
  `[[0x6047958+0x78]+0x88] = 3`), streamed through 0x3F161D0 / 0x3F164A0; unreferenced textures are
  released (0x3EC0890). Then `usage = sum`, and **`+0x20 *= 0.95` when `budget < usage + 16 MiB`,
  `*= 1.01` when `usage + 64 MiB < budget`, clamped to [0.01, 1.0]**. So when the budget is too small,
  all texture distance thresholds shrink each frame until usage fits. Two eye views requesting
  different levels for the same texture: the better one wins (lowest non-zero refcount level).
* Request 0x3C49110(cache, textureKey, oldLevel, newLevel, {u32 prio, float}): moves the per-level
  refcount; a new entry stores `quality = (resource+0x71 & 1) ? 3 : TextureDetail` and is queued
  (0x3C49350). What exactly a level means in mips is decided in the stream manager (0x3F161D0 /
  0x3F14290) - Unconfirmed (open lead).

### 4.2 Who requests which level: render stage 0x3E, job 0x3E51DE0 (Confirmed, `dec_tex2.c`, `dec_tex3.c`, `dec_tex4.c`)

Stage 0x3E (A 4.2 "second cull stage") is the texture-streaming visibility pass, one per render view:

1. Gate: scene `[view+0x2B10]` enabled, global render flag, camera `[view+0x21D8]`.
2. Sphere query (0x3CEE530 type 2) **around the camera position, radius 40 m** (`0x42200000`), mask by
   split index `[view+0x2268]` (0x240 / 0x440 / 0x40), camera position from camera vtable +0x48 into
   query+0x28; 0x3CD1E80 + 0x3CD27F0. Then extra sphere queries around further points from a list
   (radius `min(r, 40)`, skipped within 20 m of an already queried point) - which points is Unconfirmed.
   The scene cull's LOD-band and detail tests (2.2, 3.1) also apply to these queries, so only the LOD
   renderables currently in band get texture requests.
3. Resolution/FOV factor:
   `k = clamp( sqrt(w^2 + h^2) / tanf(camFov * 0.5) / 2200, 0.1, 10 )`, w, h = the view's render
   viewport ints `[view+0x2180/+0x2184]` (A 3.2), camFov = camera vtable +0x18.
4. Per object in the cull buckets: 0x3E533C0(stage, obj, k, points) -> **0x3C48E90**:
   ```c
   r2 = max(0.0625, (AABB diagonal / 2)^2);
   K  = k * [cache+0x2404] * [cache+0x20];
   level = first i in {0,1,2} with dist2(camera, AABB centre) < (K * {1,2,4}[i])^2 * r2, else 3;
   // hysteresis table 0x58B1920 {0.1,0.2,0.4} before switching to a worse level
   ```
   Extra points use `k*0.5`; the best (lowest) level is kept. Budget hysteresis: if usage*2 < budget the
   level never gets worse than before; if usage > budget it never gets better. Outside the frustum
   (0x3CDA540) and not inside any extra sphere: level + 1. Result goes to the object's vtable +0x60
   (Visual slot 12 -> 0x3C49000 -> 0x3C49110 per material texture, `renderable+0xB0[i]+0x188`) with
   priority `500 + 500*closeness`.
5. 0x3E53990 does the same for scene list `+0x1190` objects with `level = TextureDetail` if the object's
   `+0x8C` matches the stage's current ids, else 3.
6. Objects not refreshed for 4 s of game time (`[0x614FB78] - 4.0`) drop to level `[0x5EF2CB9]`.

VR arithmetic (Confirmed formula):
* desktop 1920x1080, camFov 55 deg: k = 2203 / 0.5206 / 2200 = 1.92;
* one eye 1832x1920, camFov 100 deg: k = 2654 / 1.192 / 2200 = 1.01 -> every level boundary moves to
  about half the distance; camFov 55 deg with the eye resolution: k = 2.32 (better than desktop).

Overrides (cleanest first): write `[cache+0x2404]` (x2..x4); keep the eye cameras' FOV at a desktop
value (fixes this and 2.3 together); raise the budget `+0x10` (4.1) so `+0x20` stays at 1.0.

### 4.3 Mip drop at load (TextureDetail) (Partly confirmed)

0x407CA50 and 0x407D350 (texture creation from file data): a negative "detail" argument is replaced by
`TextureDetail` (`[0x604AA10+0x36]`) and passed on to the format reader table at 0x590A850 (slot +0x18).
View environment textures (0x3CCB670..0x3CCC470) and other loaders pass `TextureDetail` explicitly.
TextureDetail 0 = no drop. Unconfirmed: that the value is the number of top mips skipped (decompile the
reader reached through 0x590A850 to see how it trims mips).

---------------------------------------------------------------------------------------------------------

## 5. Virtual textures (Granite) (Unconfirmed beyond existence)

The exe links Graphine Granite 5.1 (`C:\depot_fw4_main\3rdParty\Granite\5.1\...` strings 0x52A5670..,
RTTI `GpuCache`, `CpuCache`, `MipmapStreamingTexture`, `ProceduralTileCache` at 0x5F26648..0x5F26E30;
`VirtualTextures`, `VirtualTextureBank`, `.gts`, `GTexFileName` strings). SE describes
`VirtualTextureManager` (Resources.h: `UpdateCacheSizes`, `ProcessFeedbackData`, `ResolveFeedbackData`,
`GPUCachesSize`, `RequestResizeGPUCaches`). Granite residency is driven by GPU feedback from the rendered
views, so it is inherently view-resolution-dependent; the GPU cache size is the quality ceiling. Not
traced here (no static path from a graphics option found). Lead: find the VirtualTextureManager vtable
(SE slot order) and its UpdateCacheSizes / GPUCachesSize writer.

---------------------------------------------------------------------------------------------------------

## 6. Animation / skinning LOD (Confirmed, `dec_anim.c`, `dec_animlod.c`, `dec_granny_lod.c`)

* Per frame 0xE87D30 writes the Granny pixel error into a singleton component (offset `[0x6046FA0]`)
  `+0x10`: AnimationLODDetail 0 -> **0.0**, 1 -> 3.0, 2 -> 6.0, and calls 0x3BD8760, which records for
  every active render view (ReadState.Enabled, render enabled, scene, camera, camera vtable +0x80 true)
  `{scene, camera position (vtable +0x48), camera FOV [cam+0x14], viewport height [view+0x2184]}`.
* Per animated entity, Granny pre-phase 0x3A05050: for every recorded view camera in the entity's scene,
  `d = |camPos - entityPos| - radius`, `allowedError = 0x4A3F560(pixelError, viewportH, fov, d * 39.37)
  = tan(fov/2) * pixelError * d / (viewportH * 0.5)` (= GrannyFindAllowedLODError), minimum over views,
  stored at entity component `[0x6045C04]`.
* Post-phase 0x3A051F0: `validBones = 0x4A10150(skel, allowedError)` (-> 0x4A1F960
  GetBoneCountForLOD: **error 0 -> all bones**), then `0x4A11D00` (-> 0x4A753A0 BuildWorldPoseLOD)
  `(skel, 0, boneCount [skel+0x98], 0, validBones, localPose, NULL, worldPose)` (call sites 0x3A05611,
  0x3A058B7, disassembly).
* So at AnimationLODDetail 0 the full skeleton is always evaluated, independent of FOV or distance.
  Options 1/2 would make it depend on the eye FOV/height of each view (best view wins).
* Time slicing: 0x19E48A0 and 0x19E5110 process an entity only when `id % N == frame % N`, N = 1/2/4 by
  AnimationLODDetail (which ECS systems they belong to: Unconfirmed, callers not resolved).
* 0x3F12AA0: passes quality 3/2/1 (AnimationLODDetail 0/1/2) to an animated object's vtable +0x138 (the
  object at `[skeletonSlot+8]+0x30`; cloth/physics or animation controller - Unconfirmed).

---------------------------------------------------------------------------------------------------------

## 7. Shadows (partial, Confirmed where cited, `dec_shadow.c`, `dec_shadow2.c`)

* Stage 0x1E (sun cascades) init 0x3E01560 copies ShadowQuality to `stage+0x2182`; 0x3E015F0 rebuilds
  the resources when ShadowQuality changes. Resource sizes come from the **view creation config**:
  0x3E038A0 uses `[view+0x2B70 + q*0x20]` (colour target, format 0x5120002), 0x3E03950 uses
  `[view+0x2B74 + q*0x20]` (depth target, format 0x5230002); 4 when the stage flag `+0x2181` is 0.
  The job 0x3E01E00 uses the same record for the viewport. Same records feed stage 0x35 (A 9).
* So ShadowQuality 0 = record 0 of the view's config (the largest maps); bigger maps need the config
  record patched before the view is constructed (the config passed to 0x582BD0 / 0x17F82F0, A 3.1).
  The main view's record values were not read (Unconfirmed).
* Cascade split distances: not located (open lead: stage 0x1E job 0x3E02440 / the cascade camera set-up
  noted by A 9).
* Shadow-pass culls double (flags 0x803E) or quadruple (0x8000) the detail threshold (3.1), and
  0x3CD1E80 skips one object list for shadow queries unless ShadowQuality < 2.

---------------------------------------------------------------------------------------------------------

## 8. Other distance systems (brief)

* DynamicCrowds 0x35384E0: crowd radius^2 = `(2 * [0x5ED9EB8])^2` when on, 0 when off (Confirmed).
* SlowHDDMode: 0x3D45040 / 0x1D46880 / 0x19D9A60 compare `+0x41` with `+0x40` (pending/applied) on the
  visual load path (Confirmed reads; semantics Unconfirmed).
* Effects: SE `Effect::CullingDistance` (Render.h) exists; not traced.
* Decals: deferred decal buckets go through the same scene cull (A 1.2), so the LOD band / detail tests
  apply to them via their renderables (Unconfirmed per decal type).

---------------------------------------------------------------------------------------------------------

## 9. Function / data index (this report)

| rva | what |
|---|---|
| 0x604AA10 (ptr) | graphics options object |
| 0x604B1B8 (ptr) | GlobalSwitches (+0x15, +0x74..+0x88, +0x126) |
| 0x3ABD930 / 0x3ABDE40 / 0x3ABFB90 | gfx options defaults / load (clamps) / save |
| 0x40F7620 | GlobalSwitches base ctor (defaults) |
| 0x3C62A10 / 0x3C6AA00 / 0x3C6AA20 | Visual::AddObject / SetLODDistances / band computation |
| 0x3C6A940 | max world scale x ModelDetail factor |
| 0x3CD1E80 / 0x3CD0FA0 | scene cull query / per-type cull (frustum, LOD band, detail) |
| 0x3CDA5D0 | LOD FOV factor |
| 0x3CEE530 / 0x3CEE3D0 | sphere query / frustum query builders |
| 0x3E246B0 | CullStage query (camera pos +0x28, view +0x168) |
| 0x3F18E40 | frustum + detail cull kernel |
| 0x3ECCA30 | InstanceDistance clamp |
| 0x3D0E730 / 0x3E59680 | HLOD level distances / HLOD band rebuild |
| 0x3D04730 | visual streaming spheres (VisualStreamSystem) |
| 0x750970 / 0x3C48390 / 0x3C48520 / 0x3C48690 | TextureCache ctor / init / budget / update |
| 0x3C49000 / 0x3C49110 | per-renderable / per-texture level request |
| 0x3E51DE0 / 0x3E533C0 / 0x3C48E90 / 0x3E53990 | stage 0x3E texture visibility job / per-object level / level formula / list variant |
| 0x4076A00 | dedicated VRAM query |
| 0x407CA50 / 0x407D350 / 0x407C320 | texture creation with TextureDetail |
| 0xE87D30 / 0x3BD8760 / 0x3A05050 / 0x3A051F0 | animation LOD error per view / per entity / BuildWorldPoseLOD call |
| 0x4A3F560 / 0x4A1F960 | Granny FindAllowedLODError / GetBoneCountForLOD |
| 0x19E48A0 / 0x19E5110 | animation time slicing by AnimationLODDetail |
| 0x3B183F0 | TextureFiltering -> sampler indices |
| 0x3E01560 / 0x3E015F0 / 0x3E038A0 / 0x3E03950 | stage 0x1E init / quality change / shadow target sizes |

Tools left in `agents\H\`: `HServer.java` (request server, copy of agent C's), `req.ps1`, `run.ps1`,
`GlobalUsers.java` (decompile every function using a global, print matching lines), `GlobalDisp.java`
(instructions with given displacements inside functions using a global), `strs.py` (strings with RVAs),
`vt.py` (dump qwords as RVAs / dwords as floats at an RVA).

---------------------------------------------------------------------------------------------------------

## Open leads

1. **Runtime check of the FOV the eye cameras carry**: log `[[eyeView+0x21D8]+0x14]` and the return of
   0x3CDA5D0 and of the k computed in 0x3E51DE0 (hook 0x3C48E90 and log param_4) for both eye views. If
   the camera FOV is the HMD FOV, apply the FOV fix from the hook table.
2. What a TextureCache level (0..2) means in mips: decompile the stream manager 0x3F161D0 -> 0x3F14290 /
   0x3F14AF0 and the job they queue. Runtime alternative: log `entry+0x15` (current level) and the
   D3D11 texture's MipLevels / most-detailed mip for a known texture.
3. Mip trimming by TextureDetail at load: decompile the format reader in the table at 0x590A850 (+0x18).
4. Granite virtual textures: GPU cache size and feedback resolution (SE VirtualTextureManager layout);
   whether the second eye view writes feedback.
5. Shadow cascade split distances and the main view's config records `+0x2B70..+0x2BCF` (read them at
   runtime from the main view, or decompile the caller of 0x582BD0 that builds the config).
6. Which points 0x3E51DE0 adds as extra texture-streaming spheres (the list at `local_400`).
7. `ls::CullSystem` body 0x3EB9370 and `VisualStreamCullDistanceChangeRequestSystem` body 0x3E58B00:
   per-entity cull distances (SE `CullComponent`, `VisualStreamChangeCullDistanceOneFrameComponent`) and
   whether they use the camera position of the main view only.
8. The constant `c` at 0x6142D40 (detail cull clamp): find its static initialiser or log it.
9. The asset swap in 0x3D0CA10 / 0x3F20790 at ModelDetail 2 or `[0x604AA10+0x76]`: identify `+0x76`
   (writer) to make sure it is never set.
10. Effect (particle) culling distance (SE `Effect::CullingDistance`) and decal distances.
