# Third-party components

Everything not written for this project, where it came from, and how it was checked. Downloads live in `downloads/` (git-ignored); vendored sources in `third_party/`.

| Component | Version | Source | Used for | Verification |
| --- | --- | --- | --- | --- |
| OpenXR loader + headers | 1.1.63 | [KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) release asset `OpenXR.Loader.1.1.63.nupkg` (NuGet zip; we only extract it, never install it through NuGet) | XR runtime access | SHA-256 matches the release digest; **PGP signature valid** by a subkey of `45207B2B1E53E1F2755FF63CC5A2D593A61DBC9D`, the key named in the release notes (both checked by `tools/fetch-openxr.ps1`; a tampered copy fails with BADSIG); package contains no scripts, and its MSBuild `.props`/`.targets` only set include/lib paths and copy the DLL; `openxr_loader.dll` is unsigned (normal for Khronos builds), imports only KERNEL32 and ADVAPI32, exports only the 56 `xr*` functions |
| SafetyHook (+ Zydis 4.0.0) | 0.7.0 | [cursey/safetyhook](https://github.com/cursey/safetyhook) release asset `safetyhook-amalgamated-zydis.zip`, vendored unmodified in `third_party/safetyhook` | Hooks, disassembler | SHA-256 matches the release digest |
| BG3 Script Extender updater | v32 (`DWrite.dll`), runs extender 33.0.0.0 | [Norbyte/bg3se](https://github.com/Norbyte/bg3se) release asset | Lua modding | SHA-256 matches the release digest; DLL unsigned (normal for this project); the updater downloads the extender itself, which validates update signatures (`ValidateSignature` defaults on) |
| BG3 Script Extender source | commit af85fd2 | shallow clone of Norbyte/bg3se | Reference only, not built | n/a |
| LSLib (Divine) | 1.20.4 | [Norbyte/lslib](https://github.com/Norbyte/lslib) release asset `ExportTool-v1.20.4.zip` | Packing the Lua mod | SHA-256 matches the release digest |

**Antivirus:** Sophos Intercept X (the machine's active product; Windows Defender is off because Sophos is registered) on-demand scan with archive expansion of `downloads/` and `third_party/` on 2026-10-12: 0 detections in 759 files.

When adding a component: pin the version, check the publisher's digest (and signature where one exists), scan it, and add a row here.
