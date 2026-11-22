r"""
build_ffmpeg.py - compile FFmpeg 3.4.14's parts we need with the Xbox 360 XDK compiler.

  py build_ffmpeg.py [part ...]        parts: avutil, h264 (default: all), or single files like libavcodec/golomb.c

Why three steps per file: the XDK's Microsoft compiler (about 2010) only knows old C (C89). FFmpeg uses two newer
things everywhere ("designated initializers" .name = x, and "compound literals" (Type){...}). So:
  1. cl /P        expand every #include / #define into one plain file (file.i)
  2. c99conv      rewrite those two newer things into old C (file_conv.c)
  3. cl /c        compile that into an object file (file.obj) for the PowerPC
Errors per file go to out\errors\<file>.txt, the summary to the screen. Objects in out\obj\.
"""
import os, shutil, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
FF = os.path.normpath(os.path.join(HERE, "..", "third_party", "ffmpeg-3.4.14"))
C99CONV = os.path.normpath(os.path.join(HERE, "..", "third_party", "c99-to-c89", "c99conv.exe"))
XDK = os.environ.get("NEWTUBE_XDK") or os.path.join(os.path.expanduser("~"), "Desktop", "X360 DevTools", "Xbox 360 SDK", "XDK")   # (set NEWTUBE_XDK if your XDK is elsewhere)
GEN = os.path.join(HERE, "generated")
OUT = os.path.join(HERE, "out")

PARTS = {
    # libavutil: the basics (memory, buffers, frames, pixel formats, logging, maths); crypto/hash extras left out
    "avutil": ["libavutil/" + n + ".c" for n in (
        "avstring buffer bprint channel_layout cpu crc dict error eval fifo frame imgutils intmath integer "
        "log log2_tab mathematics mem opt parseutils pixdesc rational reverse samplefmt time utils "
        "display stereo3d spherical mastering_display_metadata hwcontext random_seed atomic sha "
        "slicethread").split()]
        + ["compat/strtod.c", "compat/msvcrt/snprintf.c", os.path.join(HERE, "sb_compat.c")],
    # libavcodec: the core + the H.264 decoder and the helpers configure selects for it
    "h264": ["libavcodec/" + n + ".c" for n in (
        "allcodecs avpacket bitstream bsf bitstream_filters codec_desc decode mathtables options parser "
        "profiles raw utils "
        "h264dec h264_cabac h264_cavlc h264_direct h264_loopfilter h264_mb h264_picture h264_refs h264_sei "
        "h264_slice h264data cabac golomb h264chroma h264dsp h264idct h264_parse h2645_parse h264_ps "
        "h264pred h264qpel videodsp startcode null_bsf pthread pthread_frame pthread_slice").split()],
    # AAC sound decoder + the maths it needs (MDCT, FFT, windows) + libavutil's float maths
    "aac": ["libavcodec/" + n + ".c" for n in (
        "aacdec aactab aacsbr aacps_float aacadtsdec mpeg4audio kbdwin sbrdsp aacpsdsp_float cbrt_data "
        "mdct15 mdct_float fft_float fft_init_table avfft sinewin").split()]
        + ["libavutil/float_dsp.c"],
    # the MP4 reader (libavformat core + mov demuxer)
    "mp4": ["libavformat/" + n + ".c" for n in (
        "allformats avio aviobuf cutils dump format id3v1 id3v2 metadata mux options os_support qtpalette "
        "protocols riff sdp url utils mov mov_chan replaygain isom riffdec").split()]
        # what the MP4 reader links to: AES (encrypted tracks), timecodes, the old bitstream-filter API, two tables
        + ["libavutil/aes.c", "libavutil/aes_ctr.c", "libavutil/timecode.c", "libavcodec/bitstream_filter.c",
           "libavcodec/ac3tab.c", "libavcodec/mpegaudiodata.c"],
    # live streams: the MPEG-TS reader + the H.264 / AAC parsers (cut the TS byte stream into whole packets)
    "ts": ["libavformat/mpegts.c"] + ["libavcodec/" + n + ".c" for n in (
        "aac_parser aac_ac3_parser h264_parser").split()],
    # own video files: the MKV reader + Dolby Digital (AC-3 / E-AC-3) sound
    "mkv": ["libavformat/" + n + ".c" for n in ("matroskadec matroska rmsipr").split()]
        + ["libavcodec/" + n + ".c" for n in (
            "ac3dec_float ac3dec_data ac3_parser ac3dsp ac3 bswapdsp fmtconvert eac3_data").split()]
        + ["libavutil/" + n + ".c" for n in ("lzo base64 downmix_info lfg md5").split()],
    # JPEG decoder (thumbnails) + its helpers: block / half-pel maths, the inverse DCT (picture blocks), EXIF tags
    "jpeg": ["libavcodec/" + n + ".c" for n in (
        "mjpegdec jpegtables blockdsp hpeldsp exif tiff_common idctdsp simple_idct jrevdct imgconvert").split()]
        + [os.path.join(HERE, "sbimage.c")],
    # our plain-C door into the decoder (sbdec.h) for 360 programs + our pthread layer (see PLAIN)
    "sbdec": [os.path.join(HERE, "sbdec.c"), os.path.join(HERE, "sb_pthread.c"), os.path.join(HERE, "sbplayer.c"),
              os.path.join(HERE, "sbhttp.c"), os.path.join(HERE, "sbhls.c")],
}
# our own files that need the 360's xtl.h: compiled DIRECTLY with cl (the c99-to-c89 converter crashes on xtl.h)
PLAIN = {"sb_pthread.c", "sbhttp.c", "sbhls.c"}
# sbhttp.c uses mbedTLS (mbedtls360\): its headers + our settings for it
MBEDTLS_FLAGS = ["/I" + os.path.normpath(os.path.join(HERE, "..", "mbedtls360")),
                 "/I" + os.path.normpath(os.path.join(HERE, "..", "third_party", "mbedtls-2.28.10", "include")),
                 '/DMBEDTLS_CONFIG_FILE="sb_mbedtls_config.h"']

# inttypes.h stand-in: stdint.h (the XDK has it) + the printf formats FFmpeg uses. Microsoft's C library prints a
# 64-bit number with "I64" where newer C libraries use "ll".
INTTYPES_H = r"""/* inttypes.h stand-in for the 2010 Microsoft C library (generated by build_ffmpeg.py) */
#ifndef SB_INTTYPES_H
#define SB_INTTYPES_H
#include <stdint.h>
#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "I64d"
#define PRIi64 "I64i"
#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "I64u"
#define PRIx8 "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "I64x"
#define PRIX8 "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "I64X"
#define PRIi8 "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIo32 "o"
#define PRIdPTR "d"
#define PRIuPTR "u"
#define PRIxPTR "x"
#define SCNd64 "I64d"
#define SCNu64 "I64u"
#define SCNx64 "I64x"
#endif
"""

# inline: C99 keyword; the old compiler calls it __inline (on the command line so it also reaches files that
# include FFmpeg's headers before config.h)
# __MINGW32CE__ (FFmpeg's old Windows CE build): with _WIN32 set (the XDK compiler sets it, and its own headers
# refuse to work without it), FFmpeg would use Windows-desktop code the 360 lacks - Unicode file names
# (MultiByteToWideChar / MB_ERR_INVALID_CHARS) in libavformat, console colours in log.c. Every FFmpeg use of this
# switch only turns that code OFF (checked: file_open.c, log.c, wchar_filename.h, os_support.h).
DEFINES = ["_XBOX", "NDEBUG", "HAVE_AV_CONFIG_H", "_USE_MATH_DEFINES", "_CRT_SECURE_NO_WARNINGS",
           "inline=__inline", "__MINGW32CE__",
           "strtod=avpriv_strtod", "snprintf=avpriv_snprintf", "_snprintf=avpriv_snprintf",
           "vsnprintf=avpriv_vsnprintf"]
# NOT FFmpeg's compat\atomics\win32: its stdatomic.h includes <windows.h> -> the 360's <xtl.h>, which the
# c99-to-c89 converter can't parse (it crashes: "Assertion failed: cache->n[0] == 1"). Our own stdatomic.h below.
INCLUDES = [GEN, os.path.join(GEN, "shim"), FF, os.path.join(XDK, "include", "xbox")]

# stdatomic.h stand-in: only what FFmpeg's files use (init/load/store/fetch_add/fetch_sub). On the 360 a pointer is
# 32 bits, so the atomic types (intptr_t) fit the LONG-based Interlocked functions (compiler intrinsics);
# __lwsync = the PowerPC memory barrier (= MemoryBarrier on the 360).
STDATOMIC_H = r"""/* stdatomic.h stand-in for the Xbox 360 (generated by build_ffmpeg.py) */
#ifndef SB_STDATOMIC_H
#define SB_STDATOMIC_H
#include <stddef.h>
#include <stdint.h>
#include <ppcintrinsics.h>
long _InterlockedExchangeAdd(long volatile *addend, long value);
long _InterlockedExchange(long volatile *target, long value);
long _InterlockedCompareExchange(long volatile *dest, long exchange, long comperand);
#pragma intrinsic(_InterlockedExchangeAdd, _InterlockedExchange, _InterlockedCompareExchange)

#define ATOMIC_VAR_INIT(value) (value)
#define atomic_init(obj, value) (*(obj) = (value))
typedef intptr_t atomic_flag, atomic_bool, atomic_char, atomic_int, atomic_uint, atomic_long, atomic_ulong,
                 atomic_llong, atomic_ullong, atomic_intptr_t, atomic_uintptr_t, atomic_size_t;

static __inline intptr_t sb_atomic_load(volatile intptr_t *p) { intptr_t v; __lwsync(); v = *p; __lwsync(); return v; }
static __inline void sb_atomic_store(volatile intptr_t *p, intptr_t v) { __lwsync(); *p = v; __lwsync(); }
#define atomic_load(obj)                          sb_atomic_load((volatile intptr_t *)(obj))
#define atomic_load_explicit(obj, order)          atomic_load(obj)
#define atomic_store(obj, v)                      sb_atomic_store((volatile intptr_t *)(obj), (intptr_t)(v))
#define atomic_store_explicit(obj, v, order)      atomic_store(obj, v)
#define atomic_fetch_add(obj, v)                  _InterlockedExchangeAdd((long volatile *)(obj), (long)(v))
#define atomic_fetch_add_explicit(obj, v, order)  atomic_fetch_add(obj, v)
#define atomic_fetch_sub(obj, v)                  _InterlockedExchangeAdd((long volatile *)(obj), -(long)(v))
#define atomic_fetch_sub_explicit(obj, v, order)  atomic_fetch_sub(obj, v)
#define atomic_exchange(obj, v)                   _InterlockedExchange((long volatile *)(obj), (long)(v))
#endif
"""

def env():
    e = dict(os.environ)
    e["PATH"] = os.path.join(XDK, "bin", "win32") + ";" + e["PATH"]
    e["INCLUDE"] = os.path.join(XDK, "include", "xbox")
    e["LIB"] = os.path.join(XDK, "lib", "xbox")
    return e

def tag(src):
    if os.path.isabs(src):          # our own files (sb_compat.c, link_test.c)
        return "own_" + os.path.splitext(os.path.basename(src))[0]
    return src.replace("/", "_").replace(".c", "")

def compile_one(src):
    name = tag(src)
    work = os.path.join(OUT, "work")
    i_file, conv, obj = (os.path.join(work, name + ".i"), os.path.join(work, name + "_conv.c"),
                         os.path.join(OUT, "obj", name + ".obj"))
    cl = os.path.join(XDK, "bin", "win32", "cl.exe")
    flags = ["/nologo", "/TC"] + ["/D" + d for d in DEFINES] + ["/I" + i for i in INCLUDES]
    if os.path.isfile(obj):          # an old object must never stand in for a file that now fails to compile
        os.remove(obj)
    if os.path.basename(src) in PLAIN:              # straight cl, no preprocess/convert steps
        r = subprocess.run([cl, "/nologo", "/c", "/TC", "/O2", "/W3", "/D_XBOX", "/DNDEBUG", "/I" + HERE]
                           + MBEDTLS_FLAGS + ["/Fo" + obj, src],
                           cwd=work, env=env(), capture_output=True, text=True, errors="replace")
        if r.returncode:
            text = (r.stdout + r.stderr).strip()
            with open(os.path.join(OUT, "errors", name + ".txt"), "w") as f:
                f.write(text)
            return src, "compile", next((l for l in text.splitlines() if "error" in l.lower()), "")[:220]
        return src, "ok", ""
    steps = [
        ("preprocess", [cl] + flags + ["/P", "/Fi" + i_file, src if os.path.isabs(src) else os.path.join(FF, src)]),
        ("convert", [C99CONV, i_file, conv]),
        ("compile", [cl, "/nologo", "/c", "/TC", "/O2", "/W1", "/wd4996", "/Fo" + obj, conv]),
    ]
    for what, cmd in steps:
        r = subprocess.run(cmd, cwd=work, env=env(), capture_output=True, text=True, errors="replace")
        text = (r.stdout + r.stderr).strip()
        if r.returncode != 0 or (what == "convert" and not os.path.isfile(conv)):
            with open(os.path.join(OUT, "errors", name + ".txt"), "w") as f:
                f.write(f"{what} FAILED (exit {r.returncode})\n{' '.join(cmd)}\n\n{text}\n")
            first = next((l for l in text.splitlines() if "error" in l.lower()), text.splitlines()[0] if text else "")
            return src, what, first.strip()[:220]
    return src, "ok", ""

def main():
    wanted = sys.argv[1:] or list(PARTS)
    files = []
    for w in wanted:
        files += PARTS.get(w, [w])
    for d in ("work", "obj", "errors"):
        os.makedirs(os.path.join(OUT, d), exist_ok=True)
    os.makedirs(os.path.join(GEN, "shim"), exist_ok=True)
    with open(os.path.join(GEN, "shim", "windows.h"), "w") as f:   # FFmpeg's win32 atomics include <windows.h>
        f.write("/* the 360 has no windows.h: its Win32-style API comes from xtl.h */\n#include <xtl.h>\n")
    with open(os.path.join(GEN, "shim", "inttypes.h"), "w") as f:  # missing in the 2010 Microsoft C library
        f.write(INTTYPES_H)
    with open(os.path.join(GEN, "shim", "stdatomic.h"), "w") as f:  # missing too (C11)
        f.write(STDATOMIC_H)
    shutil.copy2(os.path.join(HERE, "sb_pthread.h"), os.path.join(GEN, "shim", "pthread.h"))   # our threads
    for f in os.listdir(os.path.join(OUT, "errors")):
        os.remove(os.path.join(OUT, "errors", f))
    t = time.time()
    with ThreadPoolExecutor(8) as pool:
        results = list(pool.map(compile_one, files))
    ok = [r for r in results if r[1] == "ok"]
    bad = [r for r in results if r[1] != "ok"]
    for src, what, msg in bad:
        print(f"  FAIL {src:40s} [{what}] {msg}")
    print(f"\n{len(ok)} of {len(results)} compiled in {time.time() - t:.0f} s"
          + (f"; errors in out\\errors\\" if bad else ""))

if __name__ == "__main__":
    main()
