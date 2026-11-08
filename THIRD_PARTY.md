# Third-party components

The [MIT license](LICENSE) covers this project's own code. Each component below
keeps its own license. The PortMaster package ships the license texts in
`the_familia/licenses/`.

| Component | Use | License | Where |
|---|---|---|---|
| [PCRecomp](https://github.com/sp00nznet/pcrecomp) | x86 lifter (build time); recomp32 and win32hle runtime (vendored) | MIT | `third_party/pcrecomp/LICENSE`. Local changes: `third_party/pcrecomp/LOCAL_CHANGES.md` |
| [FFmpeg](https://ffmpeg.org) 6.1.2 | Video decoding (Indeo 5, MS ADPCM, Bink); shipped as shared libraries in `libs.aarch64` | LGPL 2.1 or later | Unmodified upstream source; configuration in `tools/build_ffmpeg.sh` |
| libogg, libvorbis, libvorbisfile ([Xiph.Org](https://xiph.org)) | The game's Ogg Vorbis music; shipped in `libs.aarch64` from Debian 12 | BSD-3-Clause | Package `licenses/` |
| gptokeyb ([PortMaster](https://portmaster.games)) | Controller to keyboard/mouse; supplied by PortMaster, not bundled | GPL 2.0 | Package `licenses/` |
| 7-Zip `7zzs` (PortMaster) | Unpacks the user's archives at import; supplied by PortMaster, not bundled | LGPL / BSD (7-Zip license) | PortMaster |
| SDL2, SDL2_ttf | Window, input, audio, GL loading; supplied by the firmware, not bundled | zlib | Firmware |
| GPU driver (Mali, Mesa…) | OpenGL ES; supplied by the firmware, not bundled | vendor | Firmware |

FFmpeg is linked dynamically, so it can be replaced with any compatible build of
the same major versions (`libavcodec.so.60`, `libavformat.so.60`,
`libavutil.so.58`, `libswscale.so.7`, `libswresample.so.4`).

[mafia-re](https://github.com/Marvisak/mafia-re) (no license) was used only as a
reference for function names and structure layouts. None of its code or data is
included.

The launcher's 8×8 font (`launcher/font.h`) was drawn for this project.

*Mafia: The City of Lost Heaven*, its code, artwork, audio and trademarks belong to
their respective rights holders. They are not covered by the MIT license, and no
game files are included in this repository or its releases.
