r"""
build_mbedtls.py - compile mbedTLS 2.28.10 (TLS / HTTPS) for the Xbox 360 into out\mbedtls360.lib.

  py build_mbedtls.py

Compiled directly with the XDK's cl (mbedTLS 2.28 is old-style C: no c99-to-c89 step needed). Settings:
sb_mbedtls_config.h (passed as MBEDTLS_CONFIG_FILE). The XDK compiler sets _WIN32, so a few mbedTLS files include
<windows.h> (only for things we switched off): an EMPTY windows.h in shim\ satisfies them.
"""
import os, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
MB = os.path.normpath(os.path.join(HERE, "..", "third_party", "mbedtls-2.28.10"))
XDK = os.environ.get("NEWTUBE_XDK") or os.path.join(os.path.expanduser("~"), "Desktop", "X360 DevTools", "Xbox 360 SDK", "XDK")   # (set NEWTUBE_XDK if your XDK is elsewhere)
OUT = os.path.join(HERE, "out")
SHIM = os.path.join(HERE, "shim")

def env():
    e = dict(os.environ)
    e["PATH"] = os.path.join(XDK, "bin", "win32") + ";" + e["PATH"]
    e["INCLUDE"] = os.path.join(XDK, "include", "xbox")
    e["LIB"] = os.path.join(XDK, "lib", "xbox")
    return e

PATCHED = os.path.join(HERE, "patched")     # hoist.py's fixed copies (the downloaded originals stay untouched)

def source_for(name):
    p = os.path.join(PATCHED, name + ".c")
    return p if os.path.isfile(p) else os.path.join(MB, "library", name + ".c")

def compile_with_hoisting(src, rounds=60):
    """compile; while the compiler rejects a declaration in the middle of a block, let hoist.py move it up (in a
    copy under the patched folder), then compile again"""
    import re, shutil, hoist
    name = os.path.splitext(os.path.basename(src))[0]
    for _ in range(rounds):
        n, err = compile_one(source_for(name))
        if not err:
            return name, None
        text = open(os.path.join(OUT, "errors", name + ".txt"), errors="replace").read()
        m = re.search(r"\\" + re.escape(name) + r"\.c\((\d+)\) : error (C2143: syntax error : missing ';' before "
                      r"'(type|const)'|C2275)", text)
        if not m:
            return name, err                                    # a different kind of error: report it
        if not os.path.isfile(os.path.join(PATCHED, name + ".c")):
            os.makedirs(PATCHED, exist_ok=True)
            shutil.copy2(os.path.join(MB, "library", name + ".c"), os.path.join(PATCHED, name + ".c"))
        path = os.path.join(PATCHED, name + ".c")
        lines = open(path, errors="replace").readlines()
        if not hoist.hoist_line(lines, int(m.group(1))):
            return name, f"hoist.py couldn't move line {m.group(1)}: {lines[int(m.group(1)) - 1].strip()[:120]}"
        open(path, "w").writelines(lines)
    return name, "too many rounds"

def compile_one(src, lang="/TC"):
    name = os.path.splitext(os.path.basename(src))[0]
    obj = os.path.join(OUT, "obj", name + ".obj")
    if os.path.isfile(obj):
        os.remove(obj)
    cmd = [os.path.join(XDK, "bin", "win32", "cl.exe"), "/nologo", "/c", lang, "/O2", "/W1", "/wd4996",
           "/D_XBOX", "/DNDEBUG", '/DMBEDTLS_CONFIG_FILE="sb_mbedtls_config.h"',
           "/I" + SHIM, "/I" + HERE, "/I" + os.path.join(MB, "include"), "/I" + os.path.join(MB, "library"),
           "/Fo" + obj, src]
    r = subprocess.run(cmd, cwd=OUT, env=env(), capture_output=True, text=True, errors="replace")
    if r.returncode:
        text = (r.stdout + r.stderr).strip()
        with open(os.path.join(OUT, "errors", name + ".txt"), "w") as f:
            f.write(text)
        return name, next((l for l in text.splitlines() if "error" in l.lower()), text[:200]).strip()[:220]
    return name, None

def main():
    for d in ("obj", "errors"):
        os.makedirs(os.path.join(OUT, d), exist_ok=True)
    for f in os.listdir(os.path.join(OUT, "errors")):
        os.remove(os.path.join(OUT, "errors", f))
    os.makedirs(SHIM, exist_ok=True)
    with open(os.path.join(SHIM, "windows.h"), "w") as f:
        f.write("/* empty: mbedTLS only includes windows.h for features switched off in sb_mbedtls_config.h */\n")
    sys.path.insert(0, os.path.join(HERE, "..", "ffmpeg360"))
    import build_ffmpeg                                     # the same inttypes.h stand-in as for FFmpeg
    with open(os.path.join(SHIM, "inttypes.h"), "w") as f:
        f.write(build_ffmpeg.INTTYPES_H)
    lib_dir = os.path.join(MB, "library")
    files = [os.path.join(lib_dir, f) for f in sorted(os.listdir(lib_dir)) if f.endswith(".c")]
    t = time.time()
    with ThreadPoolExecutor(8) as pool:
        results = list(pool.map(compile_with_hoisting, files))
    if os.path.isdir(PATCHED):
        print("patched copies (declarations moved up):", ", ".join(sorted(os.listdir(PATCHED))))
    bad = [(n, m) for n, m in results if m]
    for n, m in bad:
        print(f"  FAIL {n:28s} {m}")
    print(f"\n{len(results) - len(bad)} of {len(results)} compiled in {time.time() - t:.0f} s")
    if bad:
        return
    objs = sorted(os.listdir(os.path.join(OUT, "obj")))
    lib = os.path.join(OUT, "mbedtls360.lib")
    r = subprocess.run([os.path.join(XDK, "bin", "win32", "lib.exe"), "/nologo", "/OUT:" + lib] + objs,
                       cwd=os.path.join(OUT, "obj"), env=env(), capture_output=True, text=True, errors="replace")
    print("lib:", "OK" if r.returncode == 0 else r.stdout + r.stderr, f"{os.path.getsize(lib) / 1024:,.0f} KB" if r.returncode == 0 else "")

if __name__ == "__main__":
    main()
