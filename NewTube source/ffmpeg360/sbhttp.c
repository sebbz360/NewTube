/* sbhttp.c - see sbhttp.h. Compiled DIRECTLY with cl (needs xtl.h + mbedTLS; PLAIN set in build_ffmpeg.py).

   How a read works: the file is fetched in CHUNKS of CHUNK bytes ("Range: bytes=from-to" requests, answered
   with 206 Partial Content), one after another on the same encrypted connection (keep-alive). YouTube throttles
   very large single requests, so yt-dlp uses chunks too. A seek just sets the position; the next read starts a new
   request there (small jumps forward are read past instead). A dropped connection is reopened at the same place. */
#include <xtl.h>
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/x509_crt.h"
#include "sbhttp.h"

#define SO_INSECURE_HOMEBREW 0x5801
#define CHUNK (8 * 1024 * 1024)         /* bytes per request */
#define SKIP_FORWARD (256 * 1024)       /* a forward jump up to this is read past instead of a new request */

struct sbhttp {
    char host[256], path[2048];
    long long size;                     /* total file size (-1 unknown) */
    long long pos;                      /* where the program will read next */
    long long stream_pos;               /* where the open response body currently is */
    long long body_left;                /* bytes left in the current response body (0 = none open) */
    int connected;
    SOCKET s;
    mbedtls_ssl_context ssl;
    long long bytes;                    /* statistics */
    int requests, connections;
};

static void (*g_log)(const char *line);
static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_roots;
static mbedtls_ssl_config g_conf;
static int g_ready;

static void say(const char *fmt, ...)
{
    char line[700];
    va_list args;
    if (!g_log) return;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = 0;
    g_log(line);
}

static int net_send(void *ctx, const unsigned char *buf, size_t len)
{
    int n = send(*(SOCKET *)ctx, (const char *)buf, (int)len, 0);
    return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : n;
}
#define NET_TIMEOUT_MS 12000
static int net_recv(void *ctx, unsigned char *buf, size_t len)
{
    int n = recv(*(SOCKET *)ctx, (char *)buf, (int)len, 0);
    if (n < 0) {
        int e = WSAGetLastError();
        say(e == WSAETIMEDOUT ? "sbhttp: server went quiet for %d s, giving up on this connection"
                              : "sbhttp: connection error %d", e == WSAETIMEDOUT ? NET_TIMEOUT_MS / 1000 : e);
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;
}
static int xnet_random(void *data, unsigned char *out, size_t len, size_t *olen)
{
    (void)data;
    XNetRandom(out, (UINT)len);
    *olen = len;
    return 0;
}

/* several threads make requests at once (thumbnails, the script, the player): the one shared random generator
   must only be used by one of them at a time */
static CRITICAL_SECTION g_rng_lock;
static CRITICAL_SECTION g_hs_lock;      /* see connect_host */

/* SESSION RESUMPTION: a full handshake = heavy certificate + key maths (~1/4 s of processor time on the 360). After
   the first one with a server we keep its session (ticket); the next connection offers it and both sides skip that
   maths. Scrolling through 20 thumbnails used to cost 20 full handshakes on the cores the video decoder needs
   (stutter). A few servers, replaced least-recently-used; only touched while holding g_hs_lock */
#define N_SESS 8
static struct { char host[256]; mbedtls_ssl_session s; int ok; unsigned used; } g_sess[N_SESS];
static unsigned g_sess_clock;
static int g_full_hs, g_resumed_hs;     /* counted for the log (sbhttp_handshake_stats) */
static DWORD g_full_ms, g_resumed_ms;

void sbhttp_handshake_stats(int *full, int *full_ms, int *resumed, int *resumed_ms)
{
    *full = g_full_hs; *full_ms = g_full_hs ? (int)(g_full_ms / g_full_hs) : 0;
    *resumed = g_resumed_hs; *resumed_ms = g_resumed_hs ? (int)(g_resumed_ms / g_resumed_hs) : 0;
}
static int locked_random(void *drbg, unsigned char *out, size_t len)
{
    int r;
    EnterCriticalSection(&g_rng_lock);
    r = mbedtls_ctr_drbg_random(drbg, out, len);
    LeaveCriticalSection(&g_rng_lock);
    return r;
}

int sbhttp_init(const char *cacert_path, void (*log)(const char *line))
{
    XNetStartupParams xnsp;
    WSADATA wsa;
    FILE *f;
    unsigned char *pem;
    long n;
    int r;

    g_log = log;
    if (g_ready)
        return 0;
    ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    XNetStartup(&xnsp);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_entropy_add_source(&g_entropy, xnet_random, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, (const unsigned char *)"sebbz360", 8))
        return -1;
    f = fopen(cacert_path, "rb");
    if (!f) { say("sbhttp: can't open %s", cacert_path); return -2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    pem = (unsigned char *)malloc(n + 1);
    fread(pem, 1, n, f);
    fclose(f);
    pem[n] = 0;
    mbedtls_x509_crt_init(&g_roots);
    r = mbedtls_x509_crt_parse(&g_roots, pem, n + 1);
    free(pem);
    if (r < 0) { say("sbhttp: cacert.pem unreadable (-0x%04X)", -r); return -3; }
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&g_conf, &g_roots, NULL);
    InitializeCriticalSection(&g_rng_lock);
    InitializeCriticalSection(&g_hs_lock);
    mbedtls_ssl_conf_rng(&g_conf, locked_random, &g_drbg);
    g_ready = 1;
    return 0;
}

static int parse_url(sbhttp *h, const char *url)
{
    const char *p = url, *slash;
    size_t n;
    if (_strnicmp(p, "https://", 8))
        return 0;
    p += 8;
    slash = strchr(p, '/');
    n = slash ? (size_t)(slash - p) : strlen(p);
    if (n == 0 || n >= sizeof(h->host)) return 0;
    memcpy(h->host, p, n);
    h->host[n] = 0;
    _snprintf(h->path, sizeof(h->path) - 1, "%s", slash ? slash : "/");
    h->path[sizeof(h->path) - 1] = 0;
    return 1;
}

static void disconnect(sbhttp *h)
{
    if (!h->connected)
        return;
    mbedtls_ssl_free(&h->ssl);
    closesocket(h->s);
    h->connected = 0;
    h->body_left = 0;
}

static int connect_host(sbhttp *h)
{
    XNDNS *dns = NULL;
    struct sockaddr_in addr;
    BOOL on = TRUE;
    DWORD tmo = NET_TIMEOUT_MS;
    int r, waited = 0;

    if (XNetDnsLookup(h->host, NULL, &dns) || !dns) return 0;
    while (dns->iStatus == WSAEINPROGRESS && waited < 10000) { Sleep(20); waited += 20; }
    if (dns->iStatus || !dns->cina) { say("sbhttp: DNS failed for %s", h->host); XNetDnsRelease(dns); return 0; }
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    addr.sin_addr = dns->aina[0];
    XNetDnsRelease(dns);
    h->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    setsockopt(h->s, SOL_SOCKET, SO_INSECURE_HOMEBREW, (const char *)&on, sizeof(on));
    /* never wait forever: a server that goes quiet makes send/recv give up after NET_TIMEOUT_MS (the request then
       fails or is retried) instead of freezing the thread that asked */
    setsockopt(h->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
    setsockopt(h->s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
    if (connect(h->s, (struct sockaddr *)&addr, sizeof(addr))) {
        say("sbhttp: connect to %s failed (%d)", h->host, WSAGetLastError());
        closesocket(h->s);
        return 0;
    }
    mbedtls_ssl_init(&h->ssl);
    mbedtls_ssl_setup(&h->ssl, &g_conf);
    mbedtls_ssl_set_hostname(&h->ssl, h->host);
    mbedtls_ssl_set_bio(&h->ssl, &h->s, net_send, net_recv, NULL);
    /* ONE handshake at a time: checking the server's certificate uses the trusted roots' keys (shared by every
       connection), and mbedTLS stores a speed-up table inside those keys while it calculates. Two threads doing
       that at once broke the memory heap and froze NewTube (all threads stuck inside the certificate maths) */
    EnterCriticalSection(&g_hs_lock);
    {
        int i, slot = -1, offered = 0;
        DWORD t0 = GetTickCount();
        for (i = 0; i < N_SESS; i++)            /* this server's kept session, if any: offer it */
            if (g_sess[i].ok && !strcmp(g_sess[i].host, h->host)) {
                slot = i;
                offered = mbedtls_ssl_set_session(&h->ssl, &g_sess[i].s) == 0;
                break;
            }
        while ((r = mbedtls_ssl_handshake(&h->ssl)) != 0)
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (slot < 0)                           /* no slot yet: a free one, else the least recently used */
            for (i = 0; i < N_SESS; i++)
                if (slot < 0 || !g_sess[i].ok || g_sess[i].used < g_sess[slot].used) {
                    slot = i;
                    if (!g_sess[i].ok) break;
                }
        if (g_sess[slot].ok) { mbedtls_ssl_session_free(&g_sess[slot].s); g_sess[slot].ok = 0; }
        if (r == 0) {                           /* keep the (new or renewed) session for next time */
            mbedtls_ssl_session_init(&g_sess[slot].s);
            if (mbedtls_ssl_get_session(&h->ssl, &g_sess[slot].s) == 0) {
                strncpy(g_sess[slot].host, h->host, sizeof(g_sess[slot].host) - 1);
                g_sess[slot].host[sizeof(g_sess[slot].host) - 1] = 0;
                g_sess[slot].ok = 1;
                g_sess[slot].used = ++g_sess_clock;
            } else
                mbedtls_ssl_session_free(&g_sess[slot].s);
            if (offered) { g_resumed_hs++; g_resumed_ms += GetTickCount() - t0; }
            else { g_full_hs++; g_full_ms += GetTickCount() - t0; }
        }
    }
    LeaveCriticalSection(&g_hs_lock);
    if (r != 0) {
        {
            char e[160];
            mbedtls_strerror(r, e, sizeof(e));
            say("sbhttp: TLS to %s refused: %s", h->host, e);
            mbedtls_ssl_free(&h->ssl);
            closesocket(h->s);
            return 0;
        }
    }
    h->connected = 1;
    h->connections++;
    return 1;
}

static int tls_write_all(sbhttp *h, const char *data, int len)
{
    while (len > 0) {
        int r = mbedtls_ssl_write(&h->ssl, (const unsigned char *)data, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return 0;
        data += r;
        len -= r;
    }
    return 1;
}

/* read the response header (up to the empty line) one byte at a time: it's small, and the body that follows must
   not be swallowed */
static int read_header(sbhttp *h, char *out, int max)
{
    int n = 0;
    while (n < max - 1) {
        unsigned char c;
        int r = mbedtls_ssl_read(&h->ssl, &c, 1);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return 0;
        out[n++] = (char)c;
        if (n >= 4 && !memcmp(out + n - 4, "\r\n\r\n", 4)) { out[n] = 0; return n; }
    }
    return 0;
}

static const char *header_value(const char *hdr, const char *name)    /* case-insensitive "Name:" lookup */
{
    size_t nl = strlen(name);
    const char *p = hdr;
    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
        if (!_strnicmp(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ') p++;
            return p;
        }
    }
    return NULL;
}

/* ask for the bytes from h->pos (one CHUNK). Follows redirects. 1 = a body is open, 0 = failed, 2 = past the end */
static int request(sbhttp *h)
{
    char req[2600], hdr[8192];
    int tries, status;
    const char *v;
    long long end;

    if (h->size >= 0 && h->pos >= h->size)
        return 2;
    for (tries = 0; tries < 6; tries++) {
        if (!h->connected && !connect_host(h))
            return 0;
        end = h->pos + CHUNK - 1;
        if (h->size >= 0 && end > h->size - 1) end = h->size - 1;
        _snprintf(req, sizeof(req) - 1, "GET %s HTTP/1.1\r\nHost: %s\r\nRange: bytes=%I64d-%I64d\r\n"
                  "User-Agent: Mozilla/5.0\r\nAccept: */*\r\nConnection: keep-alive\r\n\r\n",
                  h->path, h->host, h->pos, end);
        req[sizeof(req) - 1] = 0;
        h->requests++;
        if (!tls_write_all(h, req, (int)strlen(req)) || !read_header(h, hdr, sizeof(hdr))) {
            disconnect(h);                          /* the old connection was closed by the server: try anew */
            continue;
        }
        status = atoi(hdr + 9);                     /* "HTTP/1.1 206 ..." */
        if (status >= 300 && status < 400 && (v = header_value(hdr, "Location")) != NULL) {
            char url[2600];
            size_t n = strcspn(v, "\r\n");
            if (n >= sizeof(url)) return 0;
            memcpy(url, v, n);
            url[n] = 0;
            disconnect(h);
            if (!parse_url(h, url)) { say("sbhttp: redirect to an unsupported address"); return 0; }
            continue;
        }
        if (status == 416)                          /* "range not satisfiable": we're at/after the end */
            return 2;
        if (status != 206 && status != 200) {
            char line[200], *p = hdr;
            size_t n;
            say("sbhttp: server refused (request: %d-byte path, range %I64d-%I64d). Its answer:", (int)strlen(h->path),
                h->pos, end);
            while (*p) {                            /* every header line: the reason is sometimes in there */
                n = strcspn(p, "\r\n");
                if (n == 0) break;
                if (n >= sizeof(line)) n = sizeof(line) - 1;
                memcpy(line, p, n);
                line[n] = 0;
                say("    %s", line);
                p += strcspn(p, "\r\n");
                while (*p == '\r' || *p == '\n') p++;
            }
            say("    path ends: ...%s", strlen(h->path) > 80 ? h->path + strlen(h->path) - 80 : h->path);
            disconnect(h);
            return 0;
        }
        if (header_value(hdr, "Transfer-Encoding")) { say("sbhttp: chunked replies not supported"); return 0; }
        v = header_value(hdr, "Content-Length");
        h->body_left = v ? _atoi64(v) : 0;
        if (status == 206 && (v = header_value(hdr, "Content-Range")) != NULL && (v = strchr(v, '/')) != NULL)
            h->size = _atoi64(v + 1);               /* "bytes a-b/TOTAL" */
        else if (status == 200) {
            h->size = h->body_left;                 /* whole file from the start */
            if (h->pos > 0) {                       /* server ignored our range: skip to the position */
                h->stream_pos = 0;
                return 1;
            }
        }
        h->stream_pos = h->pos;
        return 1;
    }
    return 0;
}

sbhttp *sbhttp_open(const char *url)
{
    sbhttp *h = (sbhttp *)calloc(1, sizeof(sbhttp));
    if (!h || !g_ready || !parse_url(h, url)) { free(h); return NULL; }
    h->size = -1;
    if (request(h) != 1) {                          /* first request: also tells us the size */
        disconnect(h);
        free(h);
        return NULL;
    }
    return h;
}

int sbhttp_read(sbhttp *h, unsigned char *buf, int len)
{
    int tries = 0;
    for (;;) {
        int r;
        /* the open body is somewhere else than where the program wants to read? */
        if (h->body_left > 0 && h->stream_pos != h->pos) {
            if (h->pos > h->stream_pos && h->pos - h->stream_pos <= SKIP_FORWARD
                && h->pos - h->stream_pos <= h->body_left) {
                unsigned char junk[4096];           /* small jump forward: read past it */
                while (h->stream_pos < h->pos) {
                    int want = (int)((h->pos - h->stream_pos) < (long long)sizeof(junk) ? h->pos - h->stream_pos
                                                                                           : (long long)sizeof(junk));
                    r = mbedtls_ssl_read(&h->ssl, junk, want);
                    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                    if (r <= 0) { disconnect(h); break; }
                    h->stream_pos += r;
                    h->body_left -= r;
                }
            } else
                disconnect(h);                      /* anything else: a fresh request at the new position */
        }
        if (h->body_left <= 0) {
            if (h->size >= 0 && h->pos >= h->size)
                return 0;                           /* end of file */
            r = request(h);
            if (r == 2) return 0;
            if (r == 0) { if (++tries > 3) return -1; continue; }
            continue;
        }
        if (len > h->body_left) len = (int)h->body_left;
        r = mbedtls_ssl_read(&h->ssl, buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) {                               /* connection lost mid-body: reopen at the same position */
            disconnect(h);
            if (++tries > 3) return -1;
            continue;
        }
        h->body_left -= r;
        h->stream_pos += r;
        h->pos += r;
        h->bytes += r;
        return r;
    }
}

long long sbhttp_seek(sbhttp *h, long long pos)
{
    if (pos < 0 || (h->size >= 0 && pos > h->size))
        return -1;
    h->pos = pos;
    return pos;
}

long long sbhttp_size(sbhttp *h) { return h->size; }
long long sbhttp_tell(sbhttp *h) { return h->pos; }

void sbhttp_stats(sbhttp *h, long long *bytes, int *requests, int *connections)
{
    *bytes = h->bytes;
    *requests = h->requests;
    *connections = h->connections;
}

void sbhttp_close(sbhttp *h)
{
    if (!h) return;
    disconnect(h);
    free(h);
}

/* ---- one-shot requests (API calls): the whole reply in memory ---- */

static int read_exact(sbhttp *h, unsigned char *buf, int len)  /* 1 = got all len bytes */
{
    while (len > 0) {
        int r = mbedtls_ssl_read(&h->ssl, buf, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return 0;
        buf += r;
        len -= r;
    }
    return 1;
}

static int read_line(sbhttp *h, char *out, int max)            /* one "...\r\n" line (chunk sizes) */
{
    int n = 0;
    while (n < max - 1) {
        unsigned char c;
        if (!read_exact(h, &c, 1)) return 0;
        out[n++] = (char)c;
        if (n >= 2 && out[n - 2] == '\r' && out[n - 1] == '\n') { out[n - 2] = 0; return 1; }
    }
    return 0;
}

static int grow(char **buf, int *cap, int need)
{
    char *nb;
    int nc = *cap ? *cap : 65536;
    if (need <= *cap) return 1;
    while (nc < need) nc *= 2;
    nb = (char *)realloc(*buf, nc);
    if (!nb) return 0;
    *buf = nb;
    *cap = nc;
    return 1;
}

#define FETCH_MAX (8 * 1024 * 1024)     /* refuse replies bigger than this (an API reply is ~100 KB) */

int sbhttp_fetch(const char *method, const char *url, const char *headers, const char *body, int body_len,
                 int *status_out, char **reply_out, int *reply_len_out)
{
    sbhttp h;
    char *req, hdr[16384], line[64];
    const char *v;
    char *reply = NULL;
    int tries, status = 0, len = 0, cap = 0, ok = 0, req_len, fails = 0;

    *reply_out = NULL;
    *reply_len_out = 0;
    *status_out = 0;
    if (!g_ready) return -1;
    ZeroMemory(&h, sizeof(h));
    if (!parse_url(&h, url)) { say("sbhttp_fetch: not an https:// address"); return -1; }
    req = (char *)malloc(4096 + body_len);
    if (!req) return -1;
    for (tries = 0; tries < 6 && !ok; tries++) {
        if (!connect_host(&h)) {                    /* (i.ytimg.com sometimes drops a new connection: once more) */
            if (++fails >= 2) break;
            continue;
        }
        req_len = _snprintf(req, 4096 - 1, "%s %s HTTP/1.1\r\nHost: %s\r\n%s%sContent-Length: %d\r\n"
                            "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
                            method, h.path, h.host, headers ? headers : "",
                            (headers && *headers && headers[strlen(headers) - 1] != '\n') ? "\r\n" : "", body_len);
        if (req_len < 0) { say("sbhttp_fetch: request header too long"); break; }
        if (body_len) memcpy(req + req_len, body, body_len);
        if (!tls_write_all(&h, req, req_len + body_len) || !read_header(&h, hdr, sizeof(hdr))) {
            say("sbhttp_fetch: no reply from %s", h.host);
            disconnect(&h);
            break;
        }
        status = atoi(hdr + 9);
        if (status >= 300 && status < 400 && (v = header_value(hdr, "Location")) != NULL) {
            char to[2600];
            size_t n = strcspn(v, "\r\n");
            disconnect(&h);
            if (n >= sizeof(to)) break;
            memcpy(to, v, n);
            to[n] = 0;
            if (!parse_url(&h, to)) { say("sbhttp_fetch: redirect to an unsupported address"); break; }
            continue;
        }
        v = header_value(hdr, "Transfer-Encoding");
        if (v && !_strnicmp(v, "chunked", 7)) {     /* body = <hex size>\r\n<data>\r\n ... 0\r\n\r\n */
            for (;;) {
                int n;
                if (!read_line(&h, line, sizeof(line))) break;
                n = (int)strtol(line, NULL, 16);
                if (n <= 0) { ok = 1; break; }
                if (len + n > FETCH_MAX || !grow(&reply, &cap, len + n + 1)) break;
                if (!read_exact(&h, (unsigned char *)reply + len, n)) break;
                len += n;
                if (!read_line(&h, line, sizeof(line))) break;
            }
        } else if ((v = header_value(hdr, "Content-Length")) != NULL) {
            int n = atoi(v);
            if (n >= 0 && n <= FETCH_MAX && grow(&reply, &cap, n + 1) && read_exact(&h, (unsigned char *)reply, n)) {
                len = n;
                ok = 1;
            }
        } else {                                    /* no length: read until the server closes */
            for (;;) {
                int r;
                if (len + 16384 > FETCH_MAX || !grow(&reply, &cap, len + 16384 + 1)) break;
                r = mbedtls_ssl_read(&h.ssl, (unsigned char *)reply + len, 16384);
                if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                if (r <= 0) { ok = 1; break; }
                len += r;
            }
        }
        disconnect(&h);
        if (!ok) {
            say("sbhttp_fetch: reply from %s cut short (%d bytes)", h.host, len);
            if (++fails >= 2) break;    /* one fresh try, then give up (the caller decides what's next) */
            len = 0;
        }
    }
    free(req);
    disconnect(&h);
    if (!ok) { free(reply); return -1; }
    if (!reply) { reply = (char *)malloc(1); if (!reply) return -1; }
    reply[len] = 0;                                 /* always NUL-terminated, handy for text */
    *status_out = status;
    *reply_out = reply;
    *reply_len_out = len;
    return 0;
}

void sbhttp_free(void *p) { free(p); }
