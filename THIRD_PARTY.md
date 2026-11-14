# Third-party software

| Component | Used for | License |
|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0 (patched, see `patches/`) | Recompiler, runtime, GPU emulation | BSD-3-Clause, © 2026 Tom Clay; portions © 2022 Ben Vanik and the Xenia project contributors |
| [Dear ImGui](https://github.com/ocornut/imgui) (from the SDK's `thirdparty/`) | Launcher and in-game menu UI | MIT |
| [stb](https://github.com/nothings/stb) (from the SDK's `thirdparty/`) | Image decoding for the partner-gear pictures | Public domain / MIT |
| [RenderDoc in-app API header](https://github.com/baldurk/renderdoc) (`renderdoc_app.h` in the SDK patch) | Optional graphics debugging | MIT |
| Embedded [Python](https://www.python.org/) 3.11 (release package only) | Co-op server for the host | PSF License |
| Microsoft Visual C++ runtime (release package only) | Runtime libraries | Microsoft redistributable terms |

The ReXGlue SDK license requires keeping its copyright notice, conditions and disclaimer with
redistributions of the SDK source or binaries. The release package includes the SDK's runtime
(`rexruntime.dll`, `rexgpu-xenos.dll`) and therefore its license notice.
