/* sb_compat.c - C library functions FFmpeg uses that the Xbox 360's (2010 Microsoft) C library doesn't have.
   Compiled together with FFmpeg by build_ffmpeg.py. */
#include <stdlib.h>

/* a console has no environment variables: FFmpeg only asks for optional settings (e.g. log colours) */
char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

/* the MKV reader calls this for FLAC sound's tag block (title, artist...). The real one lives in FFmpeg's Ogg reader,
   which we don't build; we have no FLAC decoder either, so the tags are simply skipped (0 = no error) */
int ff_vorbis_comment(void *fmt, void *dict, const unsigned char *buf, int size, int parse_picture)
{
    (void)fmt; (void)dict; (void)buf; (void)size; (void)parse_picture;
    return 0;
}

/* C99's 64-bit string -> number; the Microsoft C library calls it _strtoi64 */
long long strtoll(const char *s, char **end, int base)
{
    return _strtoi64(s, end, base);
}
