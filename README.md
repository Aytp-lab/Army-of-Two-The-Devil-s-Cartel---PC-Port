# Army of Two: The Devil's Cartel – Native PC Port

A fan-made native Windows port of the Xbox 360 game, based on static recompilation with
[ReXGlue](https://github.com/rexglue/rexglue-sdk).
Not affiliated with Electronic Arts or Visceral Games. All trademarks belong to their owners.

**No game data is included.** You need your own copy of the game as an ISO image of the
Xbox 360 disc. On first start the launcher extracts the game files from your ISO.

Supported disc: *Army of Two: The Devil's Cartel*, Xbox 360, Europe (English/German/Italian).

## Download

Get the latest ZIP from the [Releases](../../releases) page.

## Requirements

- Windows 10 or 11, 64-bit
- DirectX 12 graphics card
- CPU with SSE4.1
- About 8 GB free space for the extracted game files

## First start

1. Extract the ZIP somewhere with enough space (not into "Program Files").
2. Run `AO4Launcher.exe`.
3. Click **Browse...**, select your ISO, then **Install**. The ISO is not needed afterwards.

## Features

- Single player, split-screen co-op and online/LAN co-op (same network or a virtual LAN
  such as Tailscale, ZeroTier or Hamachi)
- Mouse and keyboard with direct mouse aiming, rebindable keys, separate mounted-gun
  sensitivity
- Assign keyboard and controllers to player slots in split screen
- Player profiles and save import
- Bravo mode (play as Bravo) and custom AI partner gear
- Optional: masks, outfits and tattoos buyable right away (still cost in-game money)
- Experimental 60 FPS mode (off by default)

## Keys in game

| Key | Function |
|-----|----------|
| F4  | Port menu (sensitivity, audio, fixes) |
| F9  | Bravo mode |
| F6  | Custom AI partner gear on/off |
| F2  | Direct mouse / stick mode |
| F3  | Performance overlay |
| F12 | Mark a bug spot in the log (with the bug capture option: also saves a screenshot and the last seconds) |

## Known issues

- Very rarely the game crashes during startup. Just start it again.
- Online/LAN co-op between two PCs has barely been tested.
- 60 FPS mode: occasionally an enemy stays idle in cover.

## Reporting bugs

Press F12 where the problem happens, then use **Launcher > Help > Pack logs for a bug
report**, and open an issue with the ZIP and a short description.

---

## Source code

This repository contains only the port's own work:

| Folder | Content |
|---|---|
| `native/ao4-frontend` | Launcher (`AO4Launcher.exe`): first-run setup from the player's ISO, settings, profiles, key bindings, partner gear |
| `native/rexglue-main` | Game-side changes (`src/`): fixes, mouse/keyboard, split screen, online co-op, Bravo mode, partner gear … plus the recompiler configuration (`*.toml`) |
| `native/rexglue-launcher` | Game host (`ao4_launcher.exe`): in-game port menu (F4) and recompiler configuration of the startup modules |
| `native/diagnostics` | `ao4_bugcam` (optional bug capture) and a shared diagnostics header |
| `patches/` | Changes to the ReXGlue SDK v0.10.0 (graphics, audio, input, network, profiles) and to one generated header |
| `analysis/native-tools` | Build and packaging scripts, the small co-op server (Python) |

**No game code and no game data are included.** The recompiled game code (`generated/` folders) is
produced on your own machine from your own copy of the game.

### Building

The build scripts were written for one Windows development machine; adjust paths where needed.

Requirements: Windows 10/11 x64, Visual Studio 2022 (or Build Tools) with the C++ workload,
LLVM/clang-cl, CMake + Ninja (both ship with Visual Studio), Git, PowerShell 5.1, and your own
Xbox 360 disc image of the game (Europe, English/German/Italian).

1. **ReXGlue SDK (binary package)** v0.10.0 for Windows into `tools/rexglue-sdk-0.10.0/`
   (provides `rexglue.exe` for code generation and the headers/libraries for the game modules).
2. **ReXGlue SDK (source)** with the port's changes into `tools/rexglue-sdk-source-0.10.0/`:
   ```
   git clone https://github.com/rexglue/rexglue-sdk.git tools/rexglue-sdk-source-0.10.0
   cd tools/rexglue-sdk-source-0.10.0
   git checkout v0.10.0
   git apply ../../patches/rexglue-sdk-v0.10.0-ao4.patch
   ```
   Build `rexruntime` and `rexgpu-xenos` (Release, D3D12, clang-cl) with the SDK's CMake presets.
3. **Game files:** extract the disc to `extracted_disc/` in the repository root
   (`default.xex`, `ao4.main.xex`, `AO4.Splash.xex`, `Engine.BuildInfo.xex`, …). This folder is
   ignored by Git and must never be committed.
4. **Generate the recompiled code** (written to the ignored `generated/` folders):
   ```
   cd native/rexglue-main
   ../../tools/rexglue-sdk-0.10.0/win-amd64/bin/rexglue.exe codegen ao4_main_manifest.toml
   cd ../..
   git apply patches/ao4_main_pch.h.patch
   cd native/rexglue-launcher
   ../../tools/rexglue-sdk-0.10.0/win-amd64/bin/rexglue.exe codegen ao4_launcher_manifest.toml
   ```
5. **Build** with the scripts in `analysis/native-tools/` (`build-main-sse41.ps1`,
   `build-rexglue-launcher.ps1`, `build-frontend.ps1`, `build-bugcam.ps1`) and package with
   `make-release.ps1`. The release package never contains game data; the script also replaces
   local build paths in the binaries and refuses to package if personal paths remain.

## License

The port's own code is licensed under the MIT License (see `LICENSE`). The ReXGlue SDK and the
patch to it are under the SDK's BSD-3-Clause license (derived from Xenia); see `THIRD_PARTY.md`.
