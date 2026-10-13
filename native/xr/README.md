# xr

`bg3vr_xr`: OpenXR + D3D11 code shared by `tools/xr_test` and, from Phase 1 step 2, the core.

- `xr::open` creates the instance (`XR_KHR_D3D11_enable`) and finds the headset, its recommended view sizes and the GPU the runtime needs.
- `xr::Session` runs on a caller-supplied `ID3D11Device` (the game's, in the core): LOCAL reference space, one swapchain per eye (colour + transfer destination, so a game image can be copied in), frame loop (`begin_frame` → `acquire`/`release` → `end_frame`).

Built only when the OpenXR loader package is present (`tools/fetch-openxr.ps1`).
