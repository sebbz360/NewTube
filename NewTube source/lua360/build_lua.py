r"""
build_lua.py - compile Lua 5.1.5 for the Xbox 360 into out\lua360.lib (the language the YouTube app's
updatable script is written in; the same Lua gMod runs on the console).

  py build_lua.py

Lua 5.1 is plain ANSI C (C89), so the XDK's compiler takes it as is (LUA_ANSI: no popen / dlopen extras).
Left out: lua.c / luac.c / print.c (stand-alone programs), loadlib.c (no DLL loading), loslib.c (os.*),
ldblib.c (debug library). linit.c is in: luaL_openlibs opens base, table, string, math and io.
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LUA = os.path.normpath(os.path.join(HERE, "..", "third_party", "lua-5.1.5", "src"))
XDK = os.environ.get("NEWTUBE_XDK") or os.path.join(os.path.expanduser("~"), "Desktop", "X360 DevTools", "Xbox 360 SDK", "XDK")   # (set NEWTUBE_XDK if your XDK is elsewhere)
OUT = os.path.join(HERE, "out")
FILES = ["lapi", "lcode", "ldebug", "ldo", "ldump", "lfunc", "lgc", "llex", "lmem", "lobject", "lopcodes",
         "lparser", "lstate", "lstring", "ltable", "ltm", "lundump", "lvm", "lzio",
         "lauxlib", "lbaselib", "ltablib", "lstrlib", "lmathlib", "liolib", "linit_sb"]


def main():
    os.makedirs(os.path.join(OUT, "obj"), exist_ok=True)
    env = dict(os.environ)
    env["PATH"] = os.path.join(XDK, "bin", "win32") + ";" + env["PATH"]
    env["INCLUDE"] = os.path.join(XDK, "include", "xbox")
    env["LIB"] = os.path.join(XDK, "lib", "xbox")
    srcs = []
    for f in FILES:
        p = os.path.join(HERE, f + ".c") if f.endswith("_sb") else os.path.join(LUA, f + ".c")
        srcs.append(p)
    for f in os.listdir(os.path.join(OUT, "obj")):
        os.remove(os.path.join(OUT, "obj", f))
    r = subprocess.run([os.path.join(XDK, "bin", "win32", "cl.exe"), "/nologo", "/c", "/O2", "/W1", "/D_XBOX", "/DNDEBUG", "/DLUA_ANSI", "/I" + LUA,
                        "/Fo" + os.path.join(OUT, "obj") + "\\"] + srcs,
                       cwd=OUT, env=env, capture_output=True, text=True, errors="replace")
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("compile failed")
    objs = [os.path.join(OUT, "obj", f + ".obj") for f in FILES]
    r = subprocess.run([os.path.join(XDK, "bin", "win32", "lib.exe"), "/nologo", "/OUT:" + os.path.join(OUT, "lua360.lib")] + objs,
                       cwd=OUT, env=env, capture_output=True, text=True, errors="replace")
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("lib failed")
    print("out\\lua360.lib: %d files, %.0f KB" % (len(objs), os.path.getsize(os.path.join(OUT, "lua360.lib")) / 1024))


if __name__ == "__main__":
    main()

