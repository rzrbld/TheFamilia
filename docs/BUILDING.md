# Building The Familia

The build turns your own copy of Mafia 1.2's executables into C (`build/gen/`),
cross-compiles it with the runtime for AArch64, and packages a PortMaster zip.
Game files and generated code stay in `build/` and are never committed.

## Requirements

- Linux x86-64 with Docker, Python 3, `curl`, `zip`
- Python packages: `pefile`, `capstone`
- A checkout of [PCRecomp](https://github.com/sp00nznet/pcrecomp). The translator
  uses its `tools/pe`, `tools/disasm` and `tools/lift`. The runtime part is
  vendored in `third_party/pcrecomp`.
- About 20 GB of free disk space. Translation and compilation take a while,
  around an hour on a fast desktop.

## Input files

Take these from an installed and 1.2-patched game, or copy them from a handheld's
`ports/the_familia/data/` after the port's first-run import:

| File | Version | SHA-256 |
|---|---|---|
| `Game.exe` | 1.2, unprotected | `f49809e22e0a927af670c19cc81ccd9bcab443abfc0e7737eceb8a7996dff11c` |
| `LS3DF.dll` | 1.2 patch | `e211c85009440d6c032f2074d79184f074a9a046574648745c2594b4cef178a6` |
| `rw_data.dll`, `IJoy.dll` | from the game | |

The retail Game.exe is SafeDisc-protected: its code is encrypted, so it cannot be
translated. Other unprotected 1.2 builds may work but are untested.

## Steps

```sh
# 1. aarch64 cross toolchain image (Debian 12, glibc 2.36)
docker build -t familia-cross:bookworm -f docker/Dockerfile.cross docker

# 2. minimal FFmpeg 6.1.2 for the videos (shared, LGPL): build/ffmpeg/
tools/build_ffmpeg.sh

# 3. translate the four modules to C: build/gen/
#    (this also applies the native replacements from config/native.json and
#    the loop splices from config/splices.json)
PCRECOMP=/path/to/pcrecomp python3 -P tools/translate.py /path/to/game

# 4. compile the game, the launcher and the importer
docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src familia-cross:bookworm \
    make ARCH=aarch64 -j$(nproc) build/aarch64/familia launcher import

# 5. PortMaster package: dist/the_familia.zip
tools/package.sh build/aarch64/familia
```

## Profile-guided build (optional, about 10–15% faster)

The release binary is built with GCC profile-guided optimisation. Training runs on
a real handheld:

1. Install the package on a muOS handheld, import the game once, and copy
   `packaging/dev-run.sh` into `ports/the_familia/`.
2. Make the handheld reachable over SSH and set `FAMILIA_DEV=root@<handheld-ip>`.
3. Run `tools/pgo.sh`. It builds an instrumented binary, plays a scripted
   100-second Free Ride session on the handheld, and builds `build/pgo/familia`
   from the collected profile.
4. Package with `tools/package.sh build/pgo/familia`.

## Debugging switches

Set these in `ports/the_familia/conf/dev.cfg` (a shell fragment that
`The Familia.sh` sources, for example
`FAMILIA_SPIKE=50; export FAMILIA_SPIKE`):

| Variable | Effect |
|---|---|
| `FAMILIA_NATIVE_CHECK=1` | Check every native replacement against its translated original at startup. |
| `FAMILIA_SPIKE=<ms>` | Report each frame slower than `<ms>`: the slowest functions, file I/O and GL waits. |
| `FAMILIA_PROBE=1` | Function timing probes (`config/probes.json`) in the 5-second stats lines. |
| `FAMILIA_PROF=1` | Sampling profiler. Read the dumps with `tools/prof_report.py`. |
| `FAMILIA_MOVIES=0` | Skip the videos. |
| `FAMILIA_STREAM=1` | GPU streaming ring (the launcher's GPU streaming): faster, but the Mali driver's memory grows. |
| `FAMILIA_STATS=1` | Performance lines in `log.txt` every 5 s, including where the memory is (`[mem]`). |
| `FAMILIA_READAHEAD=0` | No learned archive read-ahead (`conf/readahead.txt`); `FAMILIA_READAHEAD_MB` caps it per start (default 256). |
| `HLE_READTRACE=1` | Log every file read over 2 ms (file, offset, size, time). |
| `FAMILIA_PAD=0` | No controller (keyboard and mouse only). |
| `FAMILIA_PADDBG=1` | Log the player, car, menu and control-table state every 0.25 s. |

Crash reports in `log.txt` name the translated function. `tools/addr.sh`
resolves host addresses against the unstripped binary.
