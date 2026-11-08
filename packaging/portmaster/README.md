# The Familia (Mafia: The City of Lost Heaven)

**Beta.** Mafia (PC, 2002, version 1.2) running natively on ARM64 handhelds. The
game's own x86 code (Game.exe and the LS3D engine DLLs) is statically recompiled
to ARM64. Direct3D 8 runs on OpenGL ES 3.2, sound and input on SDL2, and the
videos play through a small bundled FFmpeg.

**No game files are included. You need your own copy of the game.** This build is
provided for testing purposes only.

Source code, build instructions and issues: <https://github.com/rzrbld/TheFamilia>

## Notes

Thanks to **Illusion Softworks** for creating *Mafia: The City of Lost Heaven*.
This is an unofficial community port; the game remains its owners' property.
Thanks also to PCRecomp, mafia-re, PortMaster, SDL, FFmpeg and Xiph.Org.

Tested on the Anbernic RG40XX H (H700, Mali-G31, muOS):

| Scene | FPS |
|---|---|
| Menus | 60 |
| Videos | 25 |
| Cutscenes | 57–60 |
| Daytime driving | 24–30 |
| Free Ride, maximum traffic | 27–28 |
| Night missions | 26–28, short dips into the low 20s |

Brief hitches can happen when a new part of the city streams in. Keep
**Options → Video → Shadows** at **Very Low** (the default on a new install).
Lower draw distance, LOD and traffic for more speed. A full playthrough has not
been verified yet.

## What you need (your own files)

Put these in `ports/the_familia/source/` (any letter case):

| File | Notes |
|---|---|
| The three Mafia CDs | `.bin` + `.cue` (MODE1/2352) or `.iso` images of CD 1, 2 and 3 |
| `mafia_patch_1.2_eng.exe` | the official English 1.2 patch |
| An unprotected **1.2** `Game.exe` | loose or inside a `.rar`, `.zip` or `.7z`. The retail Game.exe is SafeDisc-protected and cannot be recompiled |

The import checks `LS3DF.dll` and `Game.exe` against the files the port was built
from (SHA-256). A different unprotected 1.2 Game.exe is accepted, with a warning
in `log.txt`.

## Install

1. Copy `The Familia.sh` into your Ports folder (muOS: `ROMS/Ports`) and the
   `the_familia` folder into `ports` on the same card.
2. Copy your files into `ports/the_familia/source/`.
3. Start **The Familia**. The first start imports the game (about 2 GB, several
   minutes; keep the handheld powered). You can delete `source/` afterwards.

Every later start opens the launcher: FPS counter, frame limit (off/30/60), look
speed, CPU (1.5 GHz performance governor, restored on exit, or the system
setting), textures (32-bit: smoother streaming; 16-bit: about 150 MB less RAM),
controller bindings, **Save and Run**.

## Controls

The controller goes through PortMaster's gptokeyb; the game sees a keyboard and mouse.

| Button | Action |
|---|---|
| Left stick / D-pad | move, steer |
| Right stick | look, aim |
| A | use, enter |
| B | jump, handbrake |
| X | reload |
| Y | crouch |
| L1 / R1 | previous / next weapon |
| L2 | aim |
| R2 | fire |
| L3 | run / walk |
| R3 | horn |
| Start | pause menu |
| Select | map |

All bindings can be changed in the launcher.

## Files written

Everything stays in `ports/the_familia/`: `conf/` (saves, game and launcher
settings) and `log.txt`. `data/` is never modified. Keep `conf/` when updating,
and attach `log.txt` when you report a problem.

## Compile

See `docs/BUILDING.md` in the source repository. The build needs your own copy of
the game's executables.
