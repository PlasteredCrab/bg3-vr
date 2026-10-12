# Renderer notes (DX11)

Fill in from RenderDoc captures of `bg3_dx11.exe`. Keep captures under `re-local/` (git-ignored).

## Frame outline

| Pass | Draw/dispatch range | Render targets | Notes |
| --- | --- | --- | --- |
| Shadows | | | |
| G-buffer / scene | | | |
| Lighting | | | |
| Transparents | | | |
| Post-FX (TAA, SSAO, SSR, DoF, tonemap) | | | |
| UI (Noesis) | | | |
| Present | | | |

## Camera constants

- Constant buffer holding view / projection: slot, offset, layout
- Is projection reversed-Z? Near and far planes
- Jitter (TAA) applied where?

## Questions

- Where could the scene be rendered a second time with another view?
- Which passes keep history between frames (TAA, motion vectors)?
