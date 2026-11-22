r"""
make_config.py - write FFmpeg's settings files for the Xbox 360 (instead of FFmpeg's Linux-only "configure").

  py make_config.py

FFmpeg's code is full of switches: CONFIG_H264_DECODER, HAVE_THREADS, ARCH_PPC ... They're used in #if lines AND
in plain C ("if (CONFIG_SMALL) ..."), so every one must exist. This script collects every switch name that
appears anywhere in libavutil / libavcodec / libavformat, sets them ALL to 0, then switches on only what we need
(the ON list below). Output: generated\config.h, generated\libavutil\avconfig.h, generated\libavutil\ffversion.h.
"""
import os, re

HERE = os.path.dirname(os.path.abspath(__file__))
FF = os.path.join(HERE, "..", "third_party", "ffmpeg-3.4.14")
OUT = os.path.join(HERE, "generated")

# what we build: the H.264 decoder (+ helpers configure would pick for it), later AAC + the MP4 reader
ON = {
    # the libraries
    "CONFIG_AVUTIL", "CONFIG_AVCODEC", "CONFIG_AVFORMAT",
    # H.264 decoder and what configure selects for it (h264_decoder_select)
    "CONFIG_H264_DECODER", "CONFIG_CABAC", "CONFIG_GOLOMB", "CONFIG_H264CHROMA", "CONFIG_H264DSP",
    "CONFIG_H264PARSE", "CONFIG_H264PRED", "CONFIG_H264QPEL", "CONFIG_VIDEODSP", "CONFIG_STARTCODE",
    # safety: the bit reader checks it doesn't read past the end of a packet (bad data can't crash us)
    "CONFIG_SAFE_BITSTREAM_READER",
    # the "null" bitstream filter: every decoder's input runs through a filter chain, "null" when the codec
    # names none (decode.c looks it up by name) -> must exist
    "CONFIG_NULL_BSF",
    # AAC sound decoder (aac_decoder_select: mdct15 mdct sinewin; mdct/mdct15 select fft)
    "CONFIG_AAC_DECODER", "CONFIG_MDCT", "CONFIG_MDCT15", "CONFIG_SINEWIN", "CONFIG_FFT",
    # the MP4 reader (mov_demuxer_select: iso_media riffdec). No protocols: the player hands it its own
    # read/seek functions (a file now, the network later)
    "CONFIG_MOV_DEMUXER", "CONFIG_ISO_MEDIA", "CONFIG_RIFFDEC",
    # LIVE streams: YouTube sends them as HLS = MPEG-TS pieces with picture + sound together. The TS reader
    # (mpegts_demuxer_select: iso_media) + the parsers that find picture/sound packet edges inside the TS stream
    "CONFIG_MPEGTS_DEMUXER", "CONFIG_H264_PARSER", "CONFIG_AAC_PARSER",
    # JPEG pictures (YouTube thumbnails): mjpeg_decoder_select = blockdsp hpeldsp exif idctdsp jpegtables
    "CONFIG_MJPEG_DECODER", "CONFIG_BLOCKDSP", "CONFIG_HPELDSP", "CONFIG_EXIF", "CONFIG_IDCTDSP",
    "CONFIG_JPEGTABLES",
    # OWN VIDEO FILES (the Offline Player's folder): the MKV reader (matroska_demuxer_select: iso_media riffdec; zlib /
    # bzlib compression stay off) + the Dolby Digital (AC-3 / E-AC-3) sound decoder most movie files use
    # (ac3_decoder_select: ac3_parser ac3dsp bswapdsp fmtconvert mdct; eac3_decoder_select: ac3_decoder)
    "CONFIG_MATROSKA_DEMUXER",
    "CONFIG_AC3_DECODER", "CONFIG_EAC3_DECODER", "CONFIG_AC3_PARSER", "CONFIG_AC3DSP", "CONFIG_BSWAPDSP",
    "CONFIG_FMTCONVERT",
    # the 360's CPU is big-endian (PowerPC)
    "HAVE_BIGENDIAN",
    # threads: FFmpeg decodes several pictures at once on worker threads; its pthread_* calls go to our own
    # sb_pthread.c (built on the 360's threads, critical sections and semaphores)
    "HAVE_THREADS", "HAVE_PTHREADS",
    # the Microsoft C library has _aligned_malloc (FFmpeg's mem.c uses it for its 16/32-byte aligned buffers)
    "HAVE_ALIGNED_MALLOC",
    # maths functions the XDK's math.h already has (else FFmpeg's libm.h adds its own copies = defined twice)
    "HAVE_ATAN2F", "HAVE_ATANF", "HAVE_COSF", "HAVE_EXPF", "HAVE_HYPOT", "HAVE_LDEXPF", "HAVE_LOG10F",
    "HAVE_POWF", "HAVE_SINF",
}
# switches with a VALUE rather than 0/1
VALUES = {
    "CONFIG_THIS_YEAR": "2026",
    "FFMPEG_CONFIGURATION": '"sebbz360 Xbox 360 build (make_config.py)"',
    "FFMPEG_LICENSE": '"LGPL version 2.1 or later"',
    "FFMPEG_DATADIR": '"game:"',
    "AVCONV_DATADIR": '"game:"',
    "CC_IDENT": '"Microsoft XDK cl (PowerPC) via c99-to-c89"',
    "SLIBSUF": '".dll"',
    "BUILDSUF": '""',
    "OS_NAME": '"xbox360"',
    "EXTERN_PREFIX": '""',
    "EXTERN_ASM": "",
    "SWS_MAX_FILTER_SIZE": "256",
    "CACHE_LINE_SIZE": "128",
}

# allcodecs.c / allformats.c build switch names by gluing words in macros: REGISTER_DECODER(H264, h264) uses
# CONFIG_H264_DECODER, which never appears written out -> read those lines too
REGISTER_KINDS = {"HWACCEL": ["HWACCEL"], "ENCODER": ["ENCODER"], "DECODER": ["DECODER"],
                  "ENCDEC": ["ENCODER", "DECODER"], "PARSER": ["PARSER"], "MUXER": ["MUXER"],
                  "DEMUXER": ["DEMUXER"], "MUXDEMUX": ["MUXER", "DEMUXER"], "PROTOCOL": ["PROTOCOL"]}

def collect():
    names = set()
    reg = re.compile(r"\bREGISTER_([A-Z]+)\s*\(\s*([A-Z0-9_]+)\s*,")
    for path in ("libavcodec/allcodecs.c", "libavformat/allformats.c", "libavformat/protocols.c"):
        full = os.path.join(FF, path)
        if os.path.isfile(full):
            for kind, name in reg.findall(open(full, errors="replace").read()):
                for suffix in REGISTER_KINDS.get(kind, []):
                    names.add(f"CONFIG_{name}_{suffix}")
    pat = re.compile(r"\b((?:CONFIG|HAVE|ARCH)_[A-Z0-9_]+)\b")
    for lib in ("libavutil", "libavcodec", "libavformat", "compat"):
        for root, _, files in os.walk(os.path.join(FF, lib)):
            for f in files:
                if f.endswith((".c", ".h")):
                    for m in pat.finditer(open(os.path.join(root, f), errors="replace").read()):
                        names.add(m.group(1))
    return names

def main():
    names = collect() | ON
    os.makedirs(os.path.join(OUT, "libavutil"), exist_ok=True)
    with open(os.path.join(OUT, "config.h"), "w") as f:
        f.write("/* generated by make_config.py - FFmpeg 3.4.14 for the Xbox 360 (do not edit by hand) */\n")
        f.write("#ifndef FFMPEG_CONFIG_H\n#define FFMPEG_CONFIG_H\n\n")
        f.write("/* the old Microsoft compiler: C99 words it spells differently */\n")
        # (NOT "restrict": the XDK's own stdlib.h uses __declspec(restrict); inline is also on the command line)
        f.write("#ifndef inline\n#define inline __inline\n#endif\n#define av_restrict __restrict\n\n")
        for k, v in VALUES.items():
            f.write(f"#define {k} {v}\n")
        f.write("\n")
        for n in sorted(names):
            if n in VALUES:
                continue
            f.write(f"#define {n} {1 if n in ON else 0}\n")
        f.write("\n#endif\n")
    with open(os.path.join(OUT, "libavutil", "avconfig.h"), "w") as f:
        f.write("/* generated by make_config.py */\n#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n"
                "#define AV_HAVE_BIGENDIAN 1\n#define AV_HAVE_FAST_UNALIGNED 0\n#endif\n")
    # the list of bitstream filters (configure normally writes it): only "null", see ON
    os.makedirs(os.path.join(OUT, "libavcodec"), exist_ok=True)
    with open(os.path.join(OUT, "libavcodec", "bsf_list.c"), "w") as f:
        f.write("/* generated by make_config.py */\nstatic const AVBitStreamFilter * const bitstream_filters[] = {\n"
                "    &ff_null_bsf,\n    NULL };\n")
    # the list of protocols (file:, http: ...): none - the player gives the MP4 reader its own read functions
    os.makedirs(os.path.join(OUT, "libavformat"), exist_ok=True)
    with open(os.path.join(OUT, "libavformat", "protocol_list.c"), "w") as f:
        f.write("/* generated by make_config.py */\nstatic const URLProtocol * const url_protocols[] = {\n    NULL };\n")
    with open(os.path.join(OUT, "libavutil", "ffversion.h"), "w") as f:
        f.write('/* generated by make_config.py */\n#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n'
                '#define FFMPEG_VERSION "3.4.14-sebbz360"\n#endif\n')
    print(f"config.h: {len(names)} switches, {len(ON)} on")

if __name__ == "__main__":
    main()
