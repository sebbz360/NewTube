/* sbhls.h - read a LIVE stream (HLS: a playlist of ~5-second MPEG-TS pieces that keeps growing) as one endless
   byte stream, for the player. A background thread downloads the pieces ahead (sbhttp_fetch) and re-reads the
   playlist while the stream goes on. See sbhls.c.

     sbhls_open(url, log)     url = an https:// MEDIA playlist (one quality). NULL = failed
     sbhls_read(h, buf, n)    >0 bytes, 0 = the stream ended, <0 = error / stopped. Waits while the next piece downloads
     sbhls_abort(h)           makes a waiting sbhls_read return at once (call before stopping the reading thread)
     sbhls_close(h) */
#ifndef SBHLS_H
#define SBHLS_H

typedef struct sbhls sbhls;

#ifdef __cplusplus
extern "C" {
#endif

sbhls *sbhls_open(const char *url, void (*log)(const char *line));
int sbhls_read(sbhls *h, unsigned char *buf, int len);
void sbhls_abort(sbhls *h);
/* a fresh playlist address for the same stream + quality. YouTube's live addresses stop working ~40 s after they
   were handed out (unless the app proves it's official), so the program asks for a new one every ~25 s; the piece
   numbers carry on, so the download just continues with the new address */
void sbhls_set_url(sbhls *h, const char *url);
void sbhls_close(sbhls *h);
void sbhls_stats(sbhls *h, int *pieces, long long *bytes, int *waits, int *skipped);

#ifdef __cplusplus
}
#endif

#endif
