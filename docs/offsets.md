# Offsets and signatures

Every confirmed address, per game build. The signature is the source of truth; the RVA is only for cross-checking.

## Game builds

| Date checked | Exe | Product version | Steam build | Notes |
| --- | --- | --- | --- | --- |
| 2026-10-12 | bg3_dx11.exe (104,354,368 bytes) | 4.1.1.7631656 | 25605617 | FileVersion is always 1.0.0.0; use ProductVersion |
| 2026-10-12 | bg3.exe (107,018,304 bytes) | 4.1.1.7631656 | 25605617 | Vulkan build |

Script Extender in use: 33.0.0.0 (auto-updated by the v32 updater).

## bg3_dx11.exe 4.1.1.7631656 layout

| Section | RVA | Size |
| --- | --- | --- |
| .text | 0x00001000 | 0x5248EC7 |
| .rdata | 0x0524A000 | 0xC54986 |
| .data | 0x05E9F000 | 0x2B8230 |
| .pdata | 0x06158000 | 0x3BCE98 |
| _RDATA | 0x06515000 | 0x2208 |
| .rsrc | 0x06518000 | 0x23B62 |
| .reloc | 0x0653C000 | 0x78C80 |

## Signatures

| Name | Purpose | Signature (signatures.txt line) | RVA | Build | Found by |
| --- | --- | --- | --- | --- | --- |
| | | | | | |
