[Português (BR)](README.md) | **English** | [Español](README.es.md)

# Cold Fear (Xbox) for Nintendo Switch

An experimental static recompilation port: the Xbox's x86 executable is turned into C and compiled for ARM64 with the [xboxrecomp](xboxrecomp/) toolkit and a Vulkan (NVK) renderer. This is not emulation: the game's code runs natively, and only the Xbox hardware (NV2A GPU, audio, kernel) is reimplemented.

Version **0.0.3** — by RECOMP-GUTO.

**This repository does not contain the game.** The disc files, the XBE and the C code generated from it are not distributed. To build or play you need your own copy of Cold Fear (Xbox).

## Support the project

The port is free and open source. If you would like to support the development time and the testing on real hardware, you can donate on Ko-fi:

[![Ko-fi](https://img.shields.io/badge/Ko--fi-support-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/gerissoncassiano)

From Brazil you can also use **PIX** (email key): `pocketneogames@gmail.com` — QR code in the [Portuguese README](README.md#apoie-o-projeto).

Donations are optional: nobody has to donate to play, and the project neither includes nor sells the game.

## Game version used

The port was made and tested with this version. Other regions or revisions may not work, because the code depends on the exact executable.

| Field | Value |
| --- | --- |
| Game | Cold Fear (Xbox, Ubisoft, 2005) |
| Region | NTSC-U (USA) |
| Title ID | `0x5553004B` (US-075) |
| Certificate version | 1 |
| XBE built | 2005-02-06 (XDK 5849) |
| `default.xbe` | 3,624,960 bytes |
| `default.xbe` SHA-256 | `7371e422ecd0ac5c95ebaa343a6a9c3d504f2f261a53ed8acf212f258ab377ef` |

To check your copy in PowerShell:

```powershell
Get-FileHash .\game\default.xbe -Algorithm SHA256
```

## Status

Playable on real Switch hardware, from the menu to gameplay on the ship.

- Audio: sound effects, voices and music (FMOD streams played on the host).
- Video: Vulkan/NVK with the NV2A's vertex and pixel shaders translated to GLSL, fog, stencil, lamp shadow maps, volume textures, polygon offset, the game's gamma ramp.
- NV2A fixed-function lighting (per-vertex lights, texture coordinate generation, texture matrices) in the vertex shader.
- In-game brightness: hold **−** and press the D-pad **up/down** (0 to 10, saved to the SD card).
- Performance: about 22–30 FPS on the ship without overclocking (the optional overclock adds ~5).
- Internal resolution 960×720 by default (`RECOMP_GL_SCALE`), sharper on the 720p screen.

Known issues:

- Specular highlights of the fixed-function lighting are not computed yet.
- Diagnostic logging is still compiled in.

## Upcoming fixes

- **Performance:** reach a steady 30 FPS by cutting the cost of calls between game functions (registers saved on every call), of TLS reads and of switching between the game's threads.

## Play (ready-made release)

1. Download `coldfear.nro` from [Releases](https://github.com/gerisson1996/COLD-FEAR-NX-PORT/releases).
2. Copy it to `sdmc:/switch/coldfear/`.
3. Copy your disc's files to `sdmc:/switch/coldfear/game/`.
4. Launch through title takeover (hold R while opening an installed game) to get the full memory.

```text
switch/coldfear/
  coldfear.nro
  coldfear_env.txt   (optional)
  game/
    default.xbe
    bigfile.bin
    data/
```

The log is written to `sdmc:/switch/coldfear/coldfear_log.txt`.

## Building

You need:

- devkitPro with devkitA64, libnx and SDL2 (MSYS2).
- The NVK SDK in `nvk_sdk/` (Mesa NVK for Switch; not included here because of its size).
- Python with `capstone`, to generate the C code.
- Your Cold Fear disc files in `game/`.

Generate the C code from your `default.xbe`:

```powershell
.\tools\regen.ps1 -Python 'C:\path\python.exe'
```

Build (MSYS2/devkitPro, in this folder):

```bash
bash switch/build.sh
```

Lay out the SD card folder in `switch_sd/`:

```powershell
.\tools\package.ps1
```

To check the disc files against an ISO:

```powershell
python .\tools\verify_disc.py 'C:\path\Cold Fear.iso'
```

## Options (`coldfear_env.txt`)

One per line, `KEY=value`; lines starting with `#` are ignored.

| Option | Effect |
| --- | --- |
| `COLDFEAR_LOG=0` | no log |
| `NV2A_BRIGHTNESS=n` | starting brightness (0–10) when there is no `brightness.txt` yet |
| `RECOMP_GL_SCALE=n` | internal resolution: 1 = 640×480, 1.5 = 960×720 (default), 2 = 1280×960 |
| `RECOMP_VK_INSTANCE=0` | turns off batching of repeated draws (on by default) |
| `NV2A_SHADOW_BIAS=n` | tolerance of the lamps' shadow comparison (default 128) |
| `COLDFEAR_CPU_MHZ` / `COLDFEAR_GPU_MHZ` / `COLDFEAR_MEM_MHZ` | optional overclock |

## Licenses and provenance

See `docs/PROVENANCE.md` for where each component comes from. The xboxrecomp toolkit keeps its licenses in `xboxrecomp/LICENSE`, `xboxrecomp/NOTICE` and `xboxrecomp/LICENSES/`; the rest follows `LICENSE` and `NOTICE` at the root. Do not distribute the game files, the ISO or the generated code.
