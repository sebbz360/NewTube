/* sbhls.c - see sbhls.h. Compiled directly with cl (needs xtl.h: threads, critical sections, Sleep).

   A live HLS media playlist looks like:
       #EXT-X-TARGETDURATION:5            (pieces are at most 5 s long)
       #EXT-X-MEDIA-SEQUENCE:470524       (number of the first piece listed)
       #EXTINF:5.005,
       https://...googlevideo.com/videoplayback/.../sq/470524/...
       ... (YouTube lists the newest ~7 pieces; old ones drop off the top as new ones are added at the bottom)
       #EXT-X-ENDLIST                     (only once the broadcast is over)
   We start LIVE_BEHIND pieces from the newest (YouTube's own players stay ~3 pieces behind, so a slow download
   never runs dry), then fetch every piece in order. The downloader keeps up to AHEAD pieces ready in memory; the
   player's reading thread just takes bytes from the front. */
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sbhls.h"
#include "sbhttp.h"

#define LIVE_BEHIND 3
#define AHEAD 3
#define MAX_LISTED 64
#define GIVE_UP_MS 30000            /* nothing new for this long (and no end mark) = the stream is gone */
#define HEADERS "User-Agent: Mozilla/5.0\r\n"

typedef struct { unsigned char *data; int len; } Piece;

struct sbhls {
    char url[2048];                 /* (downloader thread only) */
    char new_url[2048];             /* a fresh address from sbhls_set_url, taken over by the downloader (lock) */
    void (*log)(const char *line);
    HANDLE thread;
    CRITICAL_SECTION lock;
    volatile int stop, ended, failed;
    Piece q[AHEAD];                 /* downloaded pieces waiting to be read, oldest first */
    int qn;
    Piece cur;                      /* the piece being read (owned by the reading thread) */
    int cur_pos;
    /* playlist state (downloader thread only) */
    char *listed[MAX_LISTED];
    int nlisted, first_seq, target_ms, endlist;
    int next_seq;                   /* -1 = not started yet */
    /* statistics */
    int pieces, waits, skipped;
    long long bytes;
};

static void say(sbhls *h, const char *fmt, ...)
{
    char line[600];
    va_list args;
    if (!h->log) return;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = 0;
    h->log(line);
}

static void free_listed(sbhls *h)
{
    int i;
    for (i = 0; i < h->nlisted; i++) free(h->listed[i]);
    h->nlisted = 0;
}

/* a relative address in the playlist is relative to the playlist's own folder */
static char *full_url(sbhls *h, const char *line, int n)
{
    char *out;
    if (n > 8 && !_strnicmp(line, "https://", 8)) {
        out = (char *)malloc(n + 1);
        if (out) { memcpy(out, line, n); out[n] = 0; }
        return out;
    } else {
        const char *slash = strrchr(h->url, '/');
        int base = slash ? (int)(slash - h->url) + 1 : 0;
        out = (char *)malloc(base + n + 1);
        if (out) { memcpy(out, h->url, base); memcpy(out + base, line, n); out[base + n] = 0; }
        return out;
    }
}

/* download + read the playlist. 1 = OK */
static int load_playlist(sbhls *h)
{
    int status, len;
    char *text, *p;
    if (sbhttp_fetch("GET", h->url, HEADERS, NULL, 0, &status, &text, &len) < 0)
        return 0;
    if (status != 200 || strncmp(text, "#EXTM3U", 7)) {
        say(h, "sbhls: playlist answer HTTP %d, not a playlist", status);
        sbhttp_free(text);
        return 0;
    }
    free_listed(h);
    h->first_seq = 0;
    h->endlist = 0;
    for (p = text; *p; ) {
        char *end = p + strcspn(p, "\r\n");
        int n = (int)(end - p);
        if (n > 0) {
            if (!strncmp(p, "#EXT-X-MEDIA-SEQUENCE:", 22)) h->first_seq = atoi(p + 22);
            else if (!strncmp(p, "#EXT-X-TARGETDURATION:", 22)) h->target_ms = atoi(p + 22) * 1000;
            else if (!strncmp(p, "#EXT-X-ENDLIST", 14)) h->endlist = 1;
            else if (p[0] != '#' && h->nlisted < MAX_LISTED) {
                char *u = full_url(h, p, n);
                if (u) h->listed[h->nlisted++] = u;
            }
        }
        p = end;
        while (*p == '\r' || *p == '\n') p++;
    }
    sbhttp_free(text);
    if (h->target_ms < 1000 || h->target_ms > 20000) h->target_ms = 5000;
    return 1;
}

static int queue_count(sbhls *h)
{
    int n;
    EnterCriticalSection(&h->lock);
    n = h->qn;
    LeaveCriticalSection(&h->lock);
    return n;
}

static void nap(sbhls *h, int ms)                  /* sleep, but wake up quickly when stopped */
{
    while (ms > 0 && !h->stop) { Sleep(ms > 50 ? 50 : ms); ms -= 50; }
}

static DWORD WINAPI downloader(LPVOID arg)
{
    sbhls *h = (sbhls *)arg;
    DWORD last_new = GetTickCount();
    int fails = 0;
    while (!h->stop) {
        int idx, status, len, fresh = 0;
        char *data;
        EnterCriticalSection(&h->lock);
        if (h->new_url[0]) { strcpy(h->url, h->new_url); h->new_url[0] = 0; fresh = 1; }
        LeaveCriticalSection(&h->lock);
        if (fresh) fails = 0;                      /* (a fresh address: earlier refusals don't count) */
        if (fresh && h->next_seq >= 0) {           /* re-read the playlist at the new address at once */
            if (load_playlist(h)) {
                if (h->next_seq < h->first_seq) {
                    int to = h->first_seq + (h->nlisted > LIVE_BEHIND ? h->nlisted - LIVE_BEHIND : 0);
                    say(h, "sbhls: fell behind (piece %d gone), jumping to %d", h->next_seq, to);
                    h->skipped += to - h->next_seq;
                    h->next_seq = to;
                }
            } else
                free_listed(h);                    /* (old addresses: dead soon; the normal path re-reads it) */
        }
        if (queue_count(h) >= AHEAD) { nap(h, 100); continue; }
        idx = h->next_seq - h->first_seq;
        if (h->next_seq < 0 || idx >= h->nlisted) {           /* need a newer playlist */
            if (h->next_seq >= 0 && h->endlist) { h->ended = 1; break; }
            if (h->next_seq >= 0) nap(h, h->target_ms / 2);   /* the next piece isn't out yet */
            if (h->stop) break;
            if (!load_playlist(h)) {
                if (++fails >= 6) { say(h, "sbhls: playlist keeps failing, stopping"); h->failed = 1; break; }
                nap(h, 1000);
                continue;
            }
            fails = 0;
            if (h->next_seq < 0) {                             /* first time: a few pieces behind the newest */
                h->next_seq = h->first_seq + (h->nlisted > LIVE_BEHIND ? h->nlisted - LIVE_BEHIND : 0);
                say(h, "sbhls: live playlist, %d pieces of ~%d s listed, starting at piece %d%s", h->nlisted,
                    h->target_ms / 1000, h->next_seq, h->endlist ? " (broadcast over)" : "");
            } else if (h->next_seq < h->first_seq) {           /* we fell behind: our piece already dropped off */
                int to = h->first_seq + (h->nlisted > LIVE_BEHIND ? h->nlisted - LIVE_BEHIND : 0);
                say(h, "sbhls: fell behind (piece %d gone), jumping to %d", h->next_seq, to);
                h->skipped += to - h->next_seq;
                h->next_seq = to;
            }
            if (h->next_seq - h->first_seq < h->nlisted)
                last_new = GetTickCount();
            else if (GetTickCount() - last_new > GIVE_UP_MS) {
                say(h, "sbhls: no new pieces for %d s, the stream seems to have stopped", GIVE_UP_MS / 1000);
                h->ended = 1;
                break;
            }
            continue;
        }
        if (sbhttp_fetch("GET", h->listed[idx], HEADERS, NULL, 0, &status, &data, &len) < 0 || status != 200) {
            if (status) say(h, "sbhls: piece %d answer HTTP %d", h->next_seq, status);
            if (data) sbhttp_free(data);           /* (sbhttp_fetch sets it to NULL when it fails) */
            /* 403 = the address expired: the program's next fresh address is usually seconds away, so wait longer
               for it (12 tries x 0.5 s) before giving the piece up */
            if (++fails >= (status == 403 ? 12 : 4)) {         /* give up on this piece, take the next */
                say(h, "sbhls: skipping piece %d", h->next_seq);
                h->skipped++;
                h->next_seq++;
                fails = 0;
            } else
                nap(h, 500);
            continue;
        }
        fails = 0;
        EnterCriticalSection(&h->lock);
        h->q[h->qn].data = (unsigned char *)data;
        h->q[h->qn].len = len;
        h->qn++;
        h->pieces++;
        h->bytes += len;
        LeaveCriticalSection(&h->lock);
        h->next_seq++;
    }
    return 0;
}

sbhls *sbhls_open(const char *url, void (*log)(const char *line))
{
    sbhls *h = (sbhls *)calloc(1, sizeof(sbhls));
    if (!h) return NULL;
    h->log = log;
    if (strlen(url) >= sizeof(h->url)) { free(h); return NULL; }
    strcpy(h->url, url);
    h->next_seq = -1;
    h->target_ms = 5000;
    InitializeCriticalSection(&h->lock);
    h->thread = CreateThread(NULL, 256 * 1024, downloader, h, CREATE_SUSPENDED, NULL);
    if (!h->thread) { DeleteCriticalSection(&h->lock); free(h); return NULL; }
    XSetThreadProcessor(h->thread, 2);             /* off the screen/sound thread (hw 0) */
    ResumeThread(h->thread);
    return h;
}

int sbhls_read(sbhls *h, unsigned char *buf, int len)
{
    int n;
    while (!h->cur.data || h->cur_pos >= h->cur.len) {   /* current piece used up: take the next one */
        if (h->cur.data) { sbhttp_free(h->cur.data); h->cur.data = NULL; }
        if (h->stop) return -1;
        EnterCriticalSection(&h->lock);
        if (h->qn) {
            h->cur = h->q[0];
            memmove(h->q, h->q + 1, (h->qn - 1) * sizeof(Piece));
            h->qn--;
            h->cur_pos = 0;
        }
        LeaveCriticalSection(&h->lock);
        if (h->cur.data) break;
        if (h->ended) return 0;
        if (h->failed) return -1;
        h->waits++;
        Sleep(20);
    }
    n = h->cur.len - h->cur_pos;
    if (n > len) n = len;
    memcpy(buf, h->cur.data + h->cur_pos, n);
    h->cur_pos += n;
    return n;
}

void sbhls_abort(sbhls *h) { if (h) h->stop = 1; }

void sbhls_set_url(sbhls *h, const char *url)
{
    if (!h || strlen(url) >= sizeof(h->new_url)) return;
    EnterCriticalSection(&h->lock);
    strcpy(h->new_url, url);
    LeaveCriticalSection(&h->lock);
}

void sbhls_stats(sbhls *h, int *pieces, long long *bytes, int *waits, int *skipped)
{
    *pieces = h->pieces; *bytes = h->bytes; *waits = h->waits; *skipped = h->skipped;
}

void sbhls_close(sbhls *h)
{
    int i;
    if (!h) return;
    h->stop = 1;
    WaitForSingleObject(h->thread, INFINITE);
    CloseHandle(h->thread);
    for (i = 0; i < h->qn; i++) sbhttp_free(h->q[i].data);
    if (h->cur.data) sbhttp_free(h->cur.data);
    free_listed(h);
    DeleteCriticalSection(&h->lock);
    free(h);
}
