/* sbhttp.h - read a file from an https:// address like a local file (read / seek / size), for the player.
   Built on our own sockets (with the 360's "insecure socket" switch 0x5801) + mbedTLS with REQUIRED certificate
   checking. No xtl.h / mbedTLS types here, so FFmpeg-side code (sbplayer.c) can use it. See sbhttp.c.

     sbhttp_init(cacert, log)   once: network start, random generator, trusted roots from a PEM file. 0 = OK
     sbhttp_open(url)           connects, follows redirects; NULL = failed (reason in the log)
     sbhttp_read(h, buf, n)     >0 bytes read, 0 = end of file, <0 = error
     sbhttp_seek(h, pos)        jump to a byte position (the next read starts there); returns pos or -1
     sbhttp_size(h)             total size in bytes (-1 = unknown)
     sbhttp_close(h) */
#ifndef SBHTTP_H
#define SBHTTP_H

typedef struct sbhttp sbhttp;

#ifdef __cplusplus
extern "C" {
#endif

int sbhttp_init(const char *cacert_path, void (*log)(const char *line));
sbhttp *sbhttp_open(const char *url);
int sbhttp_read(sbhttp *h, unsigned char *buf, int len);
long long sbhttp_seek(sbhttp *h, long long pos);
long long sbhttp_size(sbhttp *h);
long long sbhttp_tell(sbhttp *h);           /* the position the next read starts at */
void sbhttp_close(sbhttp *h);
void sbhttp_stats(sbhttp *h, long long *bytes, int *requests, int *connections);

/* one-shot request (API calls): method "GET"/"POST", extra header lines ("Name: value\r\n..." or NULL), optional
   body. Follows redirects, understands chunked replies. 0 = OK: *status = HTTP status, *reply = the whole reply
   (NUL-terminated, free with sbhttp_free), *reply_len its length. -1 = failed (reason in the log) */
int sbhttp_fetch(const char *method, const char *url, const char *headers, const char *body, int body_len,
                 int *status, char **reply, int *reply_len);
void sbhttp_free(void *p);
/* secure-connection set-ups so far: full ones / resumed ones (a kept session offered), with their average ms */
void sbhttp_handshake_stats(int *full, int *full_ms, int *resumed, int *resumed_ms);

#ifdef __cplusplus
}
#endif

#endif
