# VR performance: what other projects do, and what fits BG3 VR

Research notes, 2026-10-26. Sources at the end. Figures quoted from other projects are their own claims unless marked
"measured"; our own numbers come from the 2026-10-26 runs (bg3vr.log `perf:` lines, RTX A4000, Quest 2 at 72 Hz).

## Where BG3 VR stands (measured, 2026-10-26 12:33 run)

- GPU: 11-16 ms per stereo frame (both eyes, DLSS Balanced, 80% resolution).
- Render thread (the game's D3D11 submission): ~7 ms per frame after the shared sun shadows (was ~28 ms before).
- Frame time: a steady 27.8 ms (half of 72 Hz) most of the time; stretches alternating 20 / 54 ms that feel like
  stutter; slow frames show 16-90 ms inside the mod's present work while xrWaitFrame itself averaged 0.3 ms.
- So the frame is not GPU-bound and not render-thread-bound any more. The candidates are the game's main thread
  (simulation, ~28 ms frames with ~12 ms GPU) and stalls in the present path (instrumented in the next run).

## Techniques other projects use

### 1. Steady frame pacing first (Valve, Oculus, every flat-to-VR mod)
Valve's position (Vlachos, GDC 2015/2016): reprojection / timewarp is a "safety net", the primary goal is never to
miss frames; their adaptive-quality system scales fidelity from measured GPU time so frames stay on time, and drops
detail at the image edges first. SteamVR Motion Smoothing (and Meta's ASW / Virtual Desktop's SSW) lock the app to
half rate and synthesise every other frame; users report that a rate that keeps changing makes it stutter more than
a steady lower rate. A steady 36 fps with good reprojection looks better than a jittery 36-50.
**For us:** the alternating 20 / 54 ms frames are the visible problem. Pin the stall (present-path breakdown now
logged), then hold a steady half rate.

### 2. Skip SteamVR's compositor: Meta's or Virtual Desktop's OpenXR runtime
Virtual Desktop's VDXR runtime bypasses SteamVR and its developer claims ~10% better performance (beta testers agreed;
no independent benchmark). Meta's own OpenXR runtime (Link / Air Link) likewise avoids the SteamVR layer and uses
ASW. The mod is OpenXR, so it runs on whichever runtime is active. Cost: no SteamVR overlay/desktop in-game.
**For us:** zero code; worth a test run on Meta's runtime (Link) or VDXR.

### 3. Upscaling per eye (DLSS / FSR / XeSS)
Luke Ross's flat-to-VR mods and the Skyrim VR / Fallout 4 VR upscaler plugins run DLSS per eye; reports say even
DLSS Performance looks good in VR once sharpened, and frame-time consistency matters more in VR than peak quality.
**For us:** already done (DLSS Balanced per eye, `[render] vr_upscaler`); DLSS Performance is the next step if GPU
time becomes the limit.

### 4. Foveated rendering (fixed, without eye tracking)
OpenXR Toolkit, vrperfkit, the Skyrim/Fallout VR plugins: full rate in the centre, coarser towards the edges.
Two families: variable-rate shading (same pixel count, fewer pixel-shader runs; community reports 10-40%) and
foveated resolution / quad views (fewer pixels in the periphery; bigger gains, seams at the boundary). Fixed
foveation is more visible than eye-tracked because the eyes can look at the periphery.
**For us:** VRS foveation is built (foveation.cpp, agent AU's plan: G-buffer passes, 25/35 deg zones) but its
self-test drew nothing on the last run; the new control draw tells why. Only worth it while GPU-bound.

### 5. Don't shade what the lens can't show (hidden-area mesh, radial density masking)
Valve stencils out the area outside the lens view (hidden-area mesh, OpenXR `XR_KHR_visibility_mask`) and
patented radial density masking (checkerboard skipping at the edges, filled in afterwards). No published % figure;
one Unity report even measured a 0.2 ms regression in an empty scene, so it only pays off with heavy fill cost.
**For us:** agent AU rejected radial density masking (every stencil bit used, deferred renderer); the hidden area as
4x4 VRS tiles is AU phase 4. Low priority while not GPU-bound.

### 6. Share per-eye work (single culling frustum, shared shadows, instanced stereo)
Engines with native VR cull once with a frustum enclosing both eyes and render instanced stereo (Unity: multi-pass
"almost doubles" CPU work against single-pass instanced). NVIDIA's Single Pass Stereo / Lens Matched Shading do
the GPU side on Pascal+ but need the engine's shaders changed. Flat-to-VR mods can't do instanced stereo in the
game's own shaders; they share what can be shared between two views.
**For us:** done: sun shadows shared, blend shapes shared, right eye skips streaming queries, both eyes record in
parallel. Open (agent AT): local-light shadow atlas (0x1E/0x35), cull feedback (0.72 ms per eye on workers), one cull
for both eyes (high risk).

### 7. Render the far field once (Oculus hybrid mono rendering)
Near objects in stereo, far objects once for both eyes (disparity is negligible beyond a few dozen metres). Shipped
in UE4 mobile (later removed); benefit is scene-dependent.
**For us:** would need a third view and a depth split through the game's deferred pipeline: high effort, unclear gain.

### 8. Reuse one eye's shading for the other (stereo shading reprojection, YORO)
Oculus's Stereo Shading Reprojection reprojects the first eye's shaded pixels into the second and fills the gaps:
"up to 20+%" GPU in pixel-heavy scenes, weak on specular. Research (YORO) claims over 50% less GPU, 62 to 82 fps
on Quest 2, with fallbacks for near objects.
**For us:** GPU-side only and complex in a deferred renderer; not our bottleneck now.

### 9. Alternate-eye / sequential rendering (Luke Ross's AER, UEVR's sequential modes)
Render one eye per game frame and reproject (AER), or both eyes on the same tick one after the other (UEVR's
"synchronized sequential"). AER roughly halves rendering cost per displayed frame at the price of ghosting
(improved in AER v2); UEVR's docs call native stereo the fastest when it works and plain AFR the one to avoid.
**For us:** the mod has an `[xr] mode = alternate` path from before same-frame stereo; with the main thread as the
likely limit, halving rendering wouldn't raise the frame rate. Keep same-frame stereo.

### 10. CPU: more worker threads, fewer per-frame extras
BG3 (like Fallout 4 VR in cities) is often CPU-bound. The game caps its job workers at 6; `[perf] worker_threads`
(opt-in) raises it. The mod's own per-frame work is now timed per module (`perf: the mod's game-thread work`).

## Recommended order for BG3 VR
1. Read the next run's present-path breakdown and the mod's per-module game-thread timing; fix the stall behind the
   20 / 54 ms alternation (measured problem).
2. Try Meta's OpenXR runtime (or VDXR) instead of SteamVR: free, claimed ~10%, steadier pacing with ASW.
3. If the main thread is the limit: `[perf] worker_threads` 10-12 test; trim the mod's own per-frame cost where the
   new timing shows it.
4. If the GPU becomes the limit (higher resolution, LOD/texture distances): fix VRS foveation, then DLSS Performance.
5. Later: share more right-eye work (AT's list), hidden-area tiles.

## Sources
- UEVR rendering modes: https://github.com/praydog/uevr (README / docs mirrors: https://git.sim1222.com/sim1222-mirror/UEVR)
- Valve, Advanced VR Rendering (GDC 2015): https://media.steampowered.com/apps/valve/2015/Alex_Vlachos_Advanced_VR_Rendering_GDC2015.pdf
- Valve, Advanced VR Rendering Performance (GDC 2016): https://gdcvault.com/play/1023522/Advanced-VR- and coverage https://roadtovr.com/oculus-touts-new-vr-rendering-feature-valve-calls-it-an-ideal-safety-net/
- Valve radial density masking patent: https://patents.google.com/patent/US10311540
- SteamVR Motion Smoothing: https://roadtovr.com/steamvr-motion-smoothing-asw-alex-vlachos/
- VDXR runtime: https://www.uploadvr.com/virtual-desktops-vdxr-runtime/
- Varjo / OpenXR Toolkit foveation: https://varjo.com/blog/dynamic-foveated-variable-shader-rate-with-openxr-toolkit-and-aero-a-k-a-foveated-rendering
- Pimax quad views vs VRS: https://pimax.com/quad-views-foveated-rendering-for-pimax-crystal/
- Unity OpenXR foveated rendering: https://docs.unity3d.com/Packages/com.unity.xr.openxr@1.12/manual/features/foveatedrendering.html
- vrperfkit: https://github.com/cedriclmenard/vrperfkit ; Skyrim VR upscaler: https://www.nexusmods.com/skyrimspecialedition/mods/82179 ; Fallout 4 VR: https://www.nexusmods.com/fallout4/mods/73715
- Luke Ross R.E.A.L. mods (AER, DLSS): https://mixed-news.com/en/real-vr-mods-dlss-update/
- Oculus Stereo Shading Reprojection: https://developers.meta.com/horizon/blog/introducing-stereo-shading-reprojection-for-unity/
- YORO: https://www.uploadvr.com/you-only-render-once-vr-frame-rate-improving-technique/
- Oculus Hybrid Mono Rendering: https://developers.meta.com/horizon/blog/hybrid-mono-rendering-in-ue4-and-unity/ ; UE4 mono far field: https://docs.unrealengine.com/4.26/en-US/SharingAndReleasing/XRDevelopment/VR/DevelopVR/MonoFarFieldRendering
- NVIDIA VRWorks Single Pass Stereo: https://developer.nvidia.com/vrworks/graphics/singlepassstereo
- OpenXR visibility mask: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrVisibilityMaskKHR.html ; Unity regression report: https://issuetracker.unity.com/issues/16942/xr-visibility-mesh-causes-02ms-regression-in-gpu-frametime-on-standalone-openxr-setups
- Unity single-pass instanced: https://docs.unity3d.com/2018.3/Documentation/Manual/SinglePassStereoRenderingHoloLens.html
- Single culling frustum discussion: https://answers.unrealengine.com/questions/575014/view.html
