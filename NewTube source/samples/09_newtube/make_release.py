r"""make_release.py - put NewTube's release together.

  py make_release.py [version, default 1]

-> Desktop\sebbz360 SDK\NewTube Release\NewTube v<version>\  (NewTube\ = the files for the console, README.txt,
   LICENSES\) and NewTube v<version>.zip next to it. Only what NewTube needs to run goes in (out\ minus the work\
   build folder); nobody's personal files (history, settings, subscriptions, saved videos: those live only on the
   console) and never the old Arial letter sheets.
"""
import os, shutil, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.abspath(os.path.join(HERE, "..", ".."))
TP = os.path.join(RUNTIME, "third_party")
OUT = os.path.join(HERE, "out")
ver = sys.argv[1] if len(sys.argv) > 1 else "1"
REL = os.path.join(os.path.dirname(RUNTIME), "NewTube Release")
DST = os.path.join(REL, "NewTube v" + ver)

KEEP_TOP = ["default.xex", "youtube.lua", "cacert.pem"]
KEEP_DIRS = ["fonts", "ui"]
PERSONAL = {"history.txt", "settings.txt", "subscriptions.txt", "subs_seen.txt", "playlists.txt", "searches.txt",
            "offline.txt", "log.txt", "update_url.txt", "youtube.lua.bak", "youtube.lua.new", "videos.txt"}
LICENSES = [
    (os.path.join(TP, "ffmpeg-3.4.14", "COPYING.LGPLv2.1"), "FFmpeg - LGPL 2.1.txt"),
    (os.path.join(TP, "ffmpeg-3.4.14", "LICENSE.md"), "FFmpeg - license notes.md"),
    (os.path.join(TP, "mbedtls-2.28.10", "LICENSE"), "Mbed TLS - Apache 2.0.txt"),
    (os.path.join(TP, "lua-5.1.5", "COPYRIGHT"), "Lua - MIT.txt"),
    (os.path.join(TP, "liberation-fonts", "liberation-fonts-ttf-2.1.5", "LICENSE"), "Liberation Sans - SIL OFL 1.1.txt"),
]

if os.path.exists(DST):
    shutil.rmtree(DST)
app = os.path.join(DST, "NewTube")
os.makedirs(app)
for name in KEEP_TOP:
    shutil.copy2(os.path.join(OUT, name), os.path.join(app, name))
for d in KEEP_DIRS:
    for f in sorted(os.listdir(os.path.join(OUT, d))):
        if f.lower().startswith("arial"):
            continue                                    # (Windows' Arial: not ours to share)
        os.makedirs(os.path.join(app, d), exist_ok=True)
        shutil.copy2(os.path.join(OUT, d, f), os.path.join(app, d, f))
os.makedirs(os.path.join(app, "offline"))               # (an empty folder: saved videos / your own files go here)
open(os.path.join(app, "offline", "put your own videos here.txt"), "w").write(
    "Saved videos go here by themselves. You can also put your own .mp4 / .mkv / .mov / .m4v / .ts files here\n"
    "(H.264 8-bit video, AAC / AC-3 sound); they show up in NewTube's Offline Player.\n")
for f in os.listdir(app):
    assert f not in PERSONAL, f
shutil.copy2(os.path.join(HERE, "release", "README.txt"), os.path.join(DST, "README.txt"))
os.makedirs(os.path.join(DST, "LICENSES"))
for src, name in LICENSES:
    shutil.copy2(src, os.path.join(DST, "LICENSES", name))
with open(os.path.join(DST, "LICENSES", "cacert.pem - Mozilla Public License 2.0.txt"), "w") as f:
    f.write("NewTube\\cacert.pem is Mozilla's list of trusted certificate authorities, as packaged by curl\n"
            "(https://curl.se/docs/caextract.html). It is used under the Mozilla Public License 2.0:\n"
            "https://www.mozilla.org/MPL/2.0/\n")

zpath = DST + ".zip"
if os.path.exists(zpath):
    os.remove(zpath)
n = 0
with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk(DST):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(root, f)
            z.write(full, os.path.relpath(full, REL))
            n += 1
        if not files and not dirs:
            z.write(root, os.path.relpath(root, REL) + "/")
print("%s: %d files, %.1f MB (zip %.1f MB)" % (DST, n, sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in
      os.walk(DST) for f in fs) / 1e6, os.path.getsize(zpath) / 1e6))
