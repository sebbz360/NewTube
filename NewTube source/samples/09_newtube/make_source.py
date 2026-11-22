r"""make_source.py - NewTube's source, ready to upload to GitHub.

  py make_source.py

-> Desktop\sebbz360 SDK\NewTube Release\NewTube source\  (the folders keep the Runtime layout, so BUILDING.md's steps
   work from its top folder). Left out: Microsoft's XDK and the third-party sources (BUILDING.md says where to get
   them), build output, test tools, logs and personal files.
"""
import os, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
RT = os.path.abspath(os.path.join(HERE, "..", ".."))
DST = os.path.join(os.path.dirname(RT), "NewTube Release", "NewTube source")

FILES = ["build.py",
         "tools/make_font.py", "tools/make_ui_art.py", "tools/make_pill.py",
         "design/btn_x.png", "design/btn_y.png",
         "lua360/build_lua.py", "lua360/linit_sb.c",
         "mbedtls360/build_mbedtls.py", "mbedtls360/hoist.py", "mbedtls360/sb_mbedtls_config.h",
         "ffmpeg360/build_ffmpeg.py", "ffmpeg360/make_config.py", "ffmpeg360/link_test.py", "ffmpeg360/link_test.c",
         "samples/09_newtube/main.c", "samples/09_newtube/cflags.txt", "samples/09_newtube/libs.txt",
         "samples/09_newtube/xex.xml", "samples/09_newtube/make_release.py", "samples/09_newtube/make_update.py",
         "samples/09_newtube/make_source.py", "samples/09_newtube/release/README.txt",
         "samples/09_newtube/release/BUILDING.md", "samples/09_newtube/out/youtube.lua"]
DIRS = ["runtime", "mbedtls360/patched", "mbedtls360/shim", "ffmpeg360/generated"]
FFMPEG360_GLOB = ("sb", )                   # ffmpeg360\sb*.c / sb*.h = NewTube's player, downloads, HTTPS, threads

if os.path.exists(DST):
    shutil.rmtree(DST)
copied = []


def put(rel):
    src = os.path.join(RT, rel)
    dst = os.path.join(DST, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    copied.append(rel)


for rel in FILES:
    put(rel)
for d in DIRS:
    for root, _, files in os.walk(os.path.join(RT, d)):
        for f in sorted(files):
            put(os.path.relpath(os.path.join(root, f), RT).replace("\\", "/"))
for f in sorted(os.listdir(os.path.join(RT, "ffmpeg360"))):
    if f.startswith(FFMPEG360_GLOB) and f.endswith((".c", ".h")):
        put("ffmpeg360/" + f)

# (the owner's personal build message stays on their PC)
b = os.path.join(DST, "build.py")
s = open(b, encoding="utf-8").read().replace('print("hooray ' + 'no' + 'ah!")', 'print("build finished!")')
open(b, "w", encoding="utf-8").write(s)

# the repository's front page
shutil.copy2(os.path.join(HERE, "release", "BUILDING.md"), os.path.join(DST, "BUILDING.md"))
with open(os.path.join(DST, "README.md"), "w", encoding="utf-8") as f:
    f.write("# NewTube - source\n\n"
            "A YouTube app for the Xbox 360 in the style of 2012 YouTube, by sebbz360.\n"
            "Not affiliated with, endorsed by or connected to YouTube or Google.\n\n"
            "- How to build it: [BUILDING.md](BUILDING.md)\n"
            "- What it does, controls, credits: [samples/09_newtube/release/README.txt](samples/09_newtube/release/README.txt)\n"
            "- Third-party code keeps its own license (FFmpeg LGPL 2.1, Mbed TLS Apache 2.0, Lua MIT, Liberation fonts "
            "SIL OFL 1.1); those sources are downloaded separately, see BUILDING.md.\n")

# nothing personal / private may go out
bad = ("no" + "ah", "192." + "168.", "C:\\" + "Users", "@" + "gmail")   # (written in pieces: this file is published too)
for root, _, files in os.walk(DST):
    for f in files:
        if f.endswith((".png",)):
            continue
        t = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
        for w in bad:
            assert w not in t, (f, w)
print("%s: %d files, %.1f MB" % (DST, len(copied) + 2,
      sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(DST) for f in fs) / 1e6))
