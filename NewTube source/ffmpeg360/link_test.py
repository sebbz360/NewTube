r"""
link_test.py - put all compiled FFmpeg objects into one library (ffmpeg360.lib) and link link_test.c against it
into a 360 program. Every "unresolved external symbol" = a function FFmpeg needs that we don't build yet.

  py link_test.py          (run build_ffmpeg.py first)
"""
import os, re, subprocess, sys
import build_ffmpeg as b

def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, env=b.env(), capture_output=True, text=True, errors="replace")
    return r.returncode, (r.stdout + r.stderr)

def main():
    src, what, msg = b.compile_one(os.path.join(b.HERE, "link_test.c"))
    if what != "ok":
        raise SystemExit(f"link_test.c [{what}] {msg}")
    obj = os.path.join(b.OUT, "obj")
    objs = [f for f in sorted(os.listdir(obj)) if f.endswith(".obj") and f != "own_link_test.obj"]
    lib = os.path.join(b.OUT, "ffmpeg360.lib")
    code, text = run([os.path.join(b.XDK, "bin", "win32", "lib.exe"), "/nologo", "/OUT:" + lib] + objs, obj)
    if code:
        raise SystemExit("lib FAILED\n" + text)
    print(f"ffmpeg360.lib: {len(objs)} objects, {os.path.getsize(lib) / 1024:,.0f} KB")
    xml = os.path.join(b.OUT, "work", "xex.xml")
    open(xml, "w").write('<?xml version="1.0"?><xex><titleid id="0x53423336"/></xex>')
    code, text = run([os.path.join(b.XDK, "bin", "win32", "link.exe"), "/nologo", "/SUBSYSTEM:XBOX",
                      "/XEXCONFIG:" + xml, "/OUT:" + os.path.join(b.OUT, "work", "link_test.xex"),
                      os.path.join(obj, "own_link_test.obj"), lib,
                      "xboxkrnl.lib", "xapilib.lib", "libcmt.lib"], os.path.join(b.OUT, "work"))
    missing = sorted(set(re.findall(r"unresolved external symbol (\S+)", text)))
    if code == 0:
        print("LINKED: link_test.xex built, no missing functions")
    else:
        print(f"link: {len(missing)} missing functions")
        for m in missing:
            print("   ", m)
        if not missing:
            print(text[-3000:])

if __name__ == "__main__":
    main()
