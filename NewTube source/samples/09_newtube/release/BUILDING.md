# Building NewTube from source

NewTube is plain C for the Xbox 360, built on Windows with Microsoft's Xbox 360 XDK, plus Python scripts that
prepare the libraries and art. The NewTube program itself is `samples/09_newtube/main.c`; its YouTube knowledge
is `samples/09_newtube/out/youtube.lua` (the same script the app updates from this repository).

## What you need (not included here)

| What | Where | Put it in |
|---|---|---|
| Windows + Python 3 with Pillow, numpy, fontTools | python.org, then `py -m pip install pillow numpy fonttools` | |
| Xbox 360 XDK 2.0.21256 (compiler, headers, libraries) | Microsoft (not redistributable) | `%USERPROFILE%\Desktop\X360 DevTools\Xbox 360 SDK\XDK`, or set `NEWTUBE_XDK` to its folder |
| xextool (turns the SDK's file into one RGH/JTAG run) | the usual Xbox 360 homebrew sources | `%USERPROFILE%\Desktop\X360 DevTools\Tools\XexTool\xextool.exe`, or set `NEWTUBE_XEXTOOL` |
| FFmpeg 3.4.14 source (LGPL 2.1) | https://ffmpeg.org/releases/ffmpeg-3.4.14.tar.xz (SHA-256 `a922fcfeb16df77a42eb90b9e795a551d52383d20dddd2091cb28df651278b57`) | `third_party/ffmpeg-3.4.14/` |
| c99-to-c89 1.0.3 (old-C converter for FFmpeg) | https://github.com/libav/c99-to-c89/releases (zip SHA-256 `5a060722777fe66253add5ee07316a48de1d22d7f73268ad10e4452ce2d7d556`) | `third_party/c99-to-c89/` (`c99conv.exe`) |
| Mbed TLS 2.28.10 (Apache 2.0) | https://github.com/Mbed-TLS/mbedtls/releases (tar.bz2 SHA-256 `19e5b81fdac0fe22009b9e2bdcd52d7dcafbf62bc67fc59cf0a76b5b5540d149`) | `third_party/mbedtls-2.28.10/` |
| Lua 5.1.5 (MIT) | https://www.lua.org/ftp/lua-5.1.5.tar.gz (SHA-256 `2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333`) | `third_party/lua-5.1.5/` |
| Liberation fonts 2.1.5 (SIL OFL 1.1) | https://github.com/liberationfonts/liberation-fonts/releases (ttf tar.gz SHA-256 `7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0`) | `third_party/liberation-fonts/` |
| Mozilla's trusted certificates (MPL 2.0) | https://curl.se/ca/cacert.pem | `samples/09_newtube/out/cacert.pem` |

`third_party/` sits next to `build.py` (the top of this source folder).

## Steps (from the top folder, in this order)

```
py ffmpeg360\make_config.py
py ffmpeg360\build_ffmpeg.py avutil h264 aac mp4 ts jpeg mkv sbdec
py ffmpeg360\link_test.py
py mbedtls360\build_mbedtls.py
py lua360\build_lua.py
py tools\make_ui_art.py samples\09_newtube\out\ui
py tools\make_pill.py samples\09_newtube\out\ui
py tools\make_font.py third_party\liberation-fonts\liberation-fonts-ttf-2.1.5\LiberationSans-Regular.ttf samples\09_newtube\out\fonts\liberation_sans.sbf --all
py tools\make_font.py third_party\liberation-fonts\liberation-fonts-ttf-2.1.5\LiberationSans-Bold.ttf samples\09_newtube\out\fonts\liberation_sans_bold.sbf --all
py build.py 09_newtube
py samples\09_newtube\make_release.py 1
```

- `make_config.py` writes FFmpeg's `config.h` (only the decoders / readers NewTube uses switched on).
- `build_ffmpeg.py` compiles each FFmpeg file in three steps (preprocess, c99-to-c89, compile), because the XDK
  compiler only knows old C. `link_test.py` packs the objects into `ffmpeg360\out\ffmpeg360.lib`.
- `mbedtls360/patched/` holds Mbed TLS files with declarations moved to the top of their blocks (old C); see
  `hoist.py`. They are Mbed TLS's code under its Apache 2.0 license.
- `build.py 09_newtube` compiles NewTube and writes `samples\09_newtube\out\default.xex`.
- `make_release.py` assembles the release folder and zip.

To rebuild NewTube with another FFmpeg (LGPL): change `third_party/ffmpeg-*`, re-run the FFmpeg steps and
`build.py`.

## Publishing a YouTube-script fix

Edit `samples/09_newtube/out/youtube.lua`, raise its `SCRIPT_VERSION`, run `py samples\09_newtube\make_update.py`
and upload the two files from `samples/09_newtube/update/` to the top of this repository.
