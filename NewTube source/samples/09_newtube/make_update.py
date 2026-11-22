r"""make_update.py - get NewTube's YouTube-script fix ready to publish.

  py make_update.py [oldest NewTube version, e.g. 1.0]

Copies out\youtube.lua into update\ and writes update\version.txt (line 1 = the script's SCRIPT_VERSION, line 2 =
the oldest NewTube it works with). Upload BOTH files to the folder whose web address is in NewTube's update address
(game:\update_url.txt or UPDATE_URL_DEFAULT in main.c), e.g. a GitHub repository viewed through
https://raw.githubusercontent.com/<you>/<repo>/main/<folder>. Every console with "Automatic YouTube fixes" on picks
the new script up the next time NewTube starts.

Remember: bump SCRIPT_VERSION in out\youtube.lua for every fix, or consoles think they already have it.
"""
import os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(HERE, "out", "youtube.lua")
dst = os.path.join(HERE, "update")
os.makedirs(dst, exist_ok=True)
text = open(src, encoding="utf-8").read()
m = re.search(r'SCRIPT_VERSION\s*=\s*"([^"]+)"', text)
if not m:
    sys.exit("no SCRIPT_VERSION in out\\youtube.lua")
oldest = sys.argv[1] if len(sys.argv) > 1 else "1.0"
shutil.copyfile(src, os.path.join(dst, "youtube.lua"))
with open(os.path.join(dst, "version.txt"), "w", newline="\n") as f:
    f.write(m.group(1) + "\n" + oldest + "\n")
print("update\\youtube.lua + update\\version.txt ready: script", m.group(1), "for NewTube", oldest, "and newer")
