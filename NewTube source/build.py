r"""
build.py - builds one sample of the sebbz360 Runtime into a 360 program (default.xex).

  python build.py 00_hello

Steps (the same tools as gMod's build_module.py, but a whole PROGRAM instead of a DLL):
  cl.exe     samples\<name>\*.c -> .obj        (Microsoft's PowerPC compiler from the XDK)
  link.exe   -> default.xex                    (the Xbox linker runs imagexex itself: default.pe = plain
                                                program, default.xex = the 360 file; settings = xex.xml)
  xextool    -> retail format, unencrypted, uncompressed (the SDK makes dev-kit files; RGH/JTAG run
                                                retail-format ones, like gMod's modules)

Result: samples\<name>\out\default.xex  -> copy it into its own folder on the console, e.g.
        Hdd1:\Games\SB36 Hello\default.xex, and let Aurora scan it.
"""
import os, re, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")
XDK = os.environ.get("NEWTUBE_XDK") or os.path.join(os.path.expanduser("~"), "Desktop", "X360 DevTools", "Xbox 360 SDK", "XDK")   # (set NEWTUBE_XDK if your XDK is elsewhere)
XEXTOOL = os.environ.get("NEWTUBE_XEXTOOL") or os.path.join(os.path.expanduser("~"), "Desktop", "X360 DevTools", "Tools", "XexTool", "xextool.exe")
# libraries: kernel, Xbox system library, C library, graphics, controller
LIBS = ["xboxkrnl.lib", "xapilib.lib", "libcmt.lib", "d3d9.lib", "xgraphics.lib", "xinput2.lib"]

def run(args, what, cwd):
    env = dict(os.environ, PATH=os.path.join(XDK, "bin", "win32") + ";" + os.environ["PATH"],
               INCLUDE=os.path.join(XDK, "include", "xbox"), LIB=os.path.join(XDK, "lib", "xbox"))
    if not os.path.isabs(args[0]):          # SDK tools: full path (Windows ignores the changed PATH here)
        args = [os.path.join(XDK, "bin", "win32", args[0])] + args[1:]
    r = subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
    text = (r.stdout + r.stderr).strip()
    if r.returncode != 0:
        raise SystemExit(f"{what} FAILED:\n{text}")
    return text

def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: python build.py <sample name>   (e.g. 00_hello)")
    src = os.path.join(HERE, "samples", sys.argv[1])
    if not os.path.isdir(src):
        raise SystemExit(f"no sample folder {src}")
    out = os.path.join(src, "out")
    work = os.path.join(out, "work")
    os.makedirs(work, exist_ok=True)

    # GPU programs: every runtime\<name>_vs.hlsl / <name>_ps.hlsl -> work\<name>_vs.h (an array g_<name>_vs
    # of the compiled shader) with fxc.exe, the 360's shader compiler; the .c files #include those
    # (a sample can have its own shaders too, in its own folder: e.g. 03_ffmpeg's yuv_ps.hlsl)
    for folder in (RUNTIME, src):
        for f in sorted(os.listdir(folder)):
            if f.endswith(".hlsl"):
                stem = f[:-5]
                profile = {"_vs": "vs_3_0", "_ps": "ps_3_0"}[stem[-3:]]
                run(["fxc.exe", "/nologo", "/T" + profile, "/Emain", "/Vng_" + stem, "/Fh" + stem + ".h",
                     os.path.join(folder, f)], "shader " + f, work)

    # every .c of the shared runtime library (runtime\) + every .c / .cpp of the sample itself
    # (.cpp = C++: some Microsoft libraries, like the XAV video player, only come with C++ headers)
    sources = [os.path.join(d, f) for d in (RUNTIME, src) if os.path.isdir(d)
               for f in sorted(os.listdir(d)) if f.endswith(".c") or f.endswith(".cpp")]
    # /I runtime: so a sample's  #include "p360.h"  finds the runtime's header file
    # /EHsc: C++ exception handling the way Microsoft's C++ headers expect (no effect on .c files)
    # a sample can add its own compiler options: one per line in samples\<name>\cflags.txt (e.g. /I<folder> for a
    # library's headers; relative /I paths are taken from the sample's folder)
    extra_cflags = []
    cflags_file = os.path.join(src, "cflags.txt")
    if os.path.isfile(cflags_file):
        for l in open(cflags_file):
            l = l.strip()
            if not l or l.startswith("#"):
                continue
            if l.startswith("/I") and not os.path.isabs(l[2:]):
                l = "/I" + os.path.normpath(os.path.join(src, l[2:]))
            extra_cflags.append(l)
    text = run(["cl.exe", "/nologo", "/c", "/O2", "/W3", "/EHsc", "/D_XBOX", "/DNDEBUG", "/I" + RUNTIME,
                "/I" + work] + extra_cflags + sources, "compile", work)
    for line in text.splitlines():
        if "warning" in line:
            print("  " + line.strip())
    shutil.copy2(os.path.join(src, "xex.xml"), os.path.join(work, "xex.xml"))
    objs = [os.path.splitext(os.path.basename(f))[0] + ".obj" for f in sources]
    # a sample can ask for extra libraries: one name per line in samples\<name>\libs.txt
    extra = []
    libs_file = os.path.join(src, "libs.txt")
    if os.path.isfile(libs_file):
        extra = [l.strip() for l in open(libs_file) if l.strip() and not l.startswith("#")]
    run(["link.exe", "/nologo", "/SUBSYSTEM:XBOX", "/MAP:default.map", "/XEXCONFIG:xex.xml",
         "/OUT:default.xex", *objs, *LIBS, *extra], "link", work)
    run([XEXTOOL, "-m", "r", "-e", "u", "-c", "u", "default.xex"], "xextool", work)
    shutil.copy2(os.path.join(work, "default.xex"), os.path.join(out, "default.xex"))

    dump = run(["imagexex.exe", "/DUMP", "default.xex"], "dump", work)
    title = re.search(r"(\w+) title ID", dump)
    imps = re.findall(r"^\s+(\S+\.(?:exe|xex|dll))\s+version", dump, re.M)
    print(f"{sys.argv[1]}: out\\default.xex {os.path.getsize(os.path.join(out, 'default.xex') ) / 1024 :,.1f} Kilobytes, "
          f"title {title.group(1) if title else '?'}, imports {imps or 'none'}")
    print("build finished!")

if __name__ == "__main__":
    main()
