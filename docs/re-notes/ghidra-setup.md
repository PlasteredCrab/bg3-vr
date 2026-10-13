# Ghidra setup (BG3)

Same setup as the user's Burnout 3 project (`E:\Burnout 3 Rust\...\docs\re\ghidra-setup.md`): Ghidra
12.0.3 at `E:\Ghidra\ghidra_12.0.3_PUBLIC` with the **GhidrAssistMCP 2.8.0** extension already installed
(`Ghidra\Extensions\GhidrAssistMCP`), JDK 21. Nothing new to download.

## Project

- Ghidra project: `re-local\ghidra\BG3.gpr` (git-ignored; raw RE output stays out of git).
- Program: `E:\SteamLibrary\steamapps\common\Baldurs Gate 3\bin\bg3_dx11.exe` (4.1.1.7631656), loader
  "Portable Executable (PE)". No PDB. The exe has no RTTI (`/GR-`), so classes have no names.
- Auto-analysis of this exe (~100 MB of code) is long; run it once and save the project.

## MCP

- GhidrAssistMCP serves MCP at `http://127.0.0.1:8080/mcp` while the program is open in the CodeBrowser
  with the plugin enabled (File → Configure → Configure Plugins → Miscellaneous → GhidrAssistMCPPlugin;
  Window → GhidrAssistMCP shows the server state).
- Claude Code: `.mcp.json` in the repo root (server `ghidra`). A new Claude Code session picks it up
  (it asks once to approve the project's MCP server).
- Without a session restart: `tools/ghidra/ghidra_mcp.py` (copied from the Burnout 3 repo) is a small
  standard-library client: `Mcp().call("get_code", ...)`.
- Policy (as in Burnout 3): never `patch_bytes`, `import_file` or `export_program`; we don't modify the
  program, and raw output stays out of git.

## Headless (fallback)

`tools/ghidra/decompile.ps1 -Rva 0x...` decompiles named functions without an analysis pass. Its first
run failed: `analyzeHeadless` couldn't find Java from the agent's shell (JAVA_HOME pointed at
`C:\Program Files\Java\jdk-21`, which exists). Use the GUI + MCP instead.
