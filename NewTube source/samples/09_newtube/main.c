/*
 * 09_newtube - NewTube: a 2012-YouTube-style home screen for the 360 (owner's mockup v1, design\newtube_mockup_v1.webp).
 *
 * Laid out in the mockup's 1280 x 720 coordinates and shrunk into the TV-safe 90 % (p360_draw_safe), except the
 * dark bands, which reach the screen edges.
 *
 * Threads (the screen never waits for the network):
 *   main     draws the menu every TV refresh, reads the controller, plays videos
 *   script   runs game:\youtube.lua: page sections, searches, a video's links (and opens the player)
 *   thumbs   3 threads download + decode thumbnails (the rows near the focus first); paused during playback
 * Everything shared is guarded by g_lock.
 *
 * Controls: D-pad / left stick move, A play (hold A = info panel), B back, Y search, BACK quit.
 * In a video: see the "playing" section (seek bar with preview pictures, buttons, More Videos).
 */
#include <xtl.h>
#include <xaudio2.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "p360.h"
#include "p360_video.h"
#include "p360_draw.h"
#include "../../ffmpeg360/sbplayer.h"
#include "../../ffmpeg360/sbhttp.h"
#include "../../ffmpeg360/sbimage.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

/* ---------------------------------------------------------------- look */
#define C_TOPBAR    0xFF2B2B2B
#define C_TOPBAR2   0xFF232323
#define C_SIDEBAR   0xFF353535
#define C_SIDE_SEL  0xFF262626
#define C_SIDE_CUR  0xFF4A4A4A
#define C_LINE_DARK 0xFF2A2A2A
#define C_LINE_LITE 0xFF404040
#define C_GREEN     0xFF5DB91E
#define C_TITLE     0xFF333333
#define C_META      0xFF777777
#define C_SECTION   0xFF555555
#define C_CARD      0xFF2A2A2A

#define CONTENT_X   250.0f          /* left edge of the content (mockup px) */
#define TILE_X0     266.0f
#define TILE_STEP   249.5f
#define TILE_W      232.0f
#define TILE_H      131.0f          /* 16:9 of 232 */
#define ROW_STEP    339.0f
#define ROW_Y0      131.0f
#define SIDE_W      236.0f
#define SIDE_Y0     110.0f
#define SIDE_STEP   59.5f
/* Subscriptions page (owner's mockup 2026-11-22): a channel column between the sidebar and the rows */
#define SUBS_PAGE   5               /* SIDE[] index of "Subscriptions" */
#define SUBCOL_W    292.0f          /* column width; the rows move right by this much */
#define SUBCOL_STEP 53.5f
#define SUBCOL_ROWS 12              /* rows that fit on screen */

static p360_font *F_REG, *F_BOLD;
static p360_image *IMG_LOGO, *IMG_PAPER, *IMG_BADGE, *IMG_AVATAR, *IMG_YBTN, *IMG_GEAR, *IMG_XBTN, *IMG_PILL;
/* you, top right (in place of the old "Sign In" button: sign-in comes in a later update) */
static char g_me_name[32];          /* the signed-in profile's gamertag ("" = nobody signed in) */
static p360_image *g_me_pic;        /* its gamer picture (64 x 64) */

/* ---------------------------------------------------------------- data */
#define MAX_SECTIONS 12
#define MAX_VIDEOS 24
typedef struct {
    char id[16], title[220], channel[96], views[48], age[48], dur[16];
    char list[40];                  /* a YouTube PLAYLIST tile: its id (id = the video its picture comes from) */
    int seconds;
    int thumb;                      /* 0 none, 1 downloading, 2 pixels ready, 3 shown, 4 failed */
    int fresh;                      /* Subscriptions: uploaded since your last visit ("NEW" badge) */
    unsigned int *pix;
    int pw, ph;
    p360_image *img;
} VIDEO;
typedef struct {
    char title[80], query[120], filter[32];
    int state;                      /* 0 waiting, 1 loading, 2 ready, 3 failed */
    char err[120];
    int n;
    VIDEO v[MAX_VIDEOS];
    int col, first;                 /* focused column, first column shown */
    float sx;                       /* horizontal scroll (animated) */
} SECTION;

typedef struct { const char *label, *icon, *page; } SIDEITEM;
static const SIDEITEM SIDE[] = {
    { "Search", "icon_search", NULL }, { "Home", "icon_home", "Home" }, { "Gaming", "icon_gaming", "Gaming" },
    { "Music", "icon_music", "Music" }, { "Movies", "icon_movies", "Movies" },
    { "Subscriptions", "icon_subscriptions", "Subscriptions" }, { "Account", "icon_account", "Account" },
    { "Offline Player", "icon_offline", NULL },
};
#define N_SIDE 8
static p360_image *IMG_SIDE[N_SIDE];

static CRITICAL_SECTION g_lock;
static SECTION g_sec[MAX_SECTIONS];
static int g_nsec;
static int g_gen;                   /* bumped on every page change: late results for an old page are dropped */
static char g_message[200];         /* shown instead of sections (e.g. "sign-in comes later") */
static int g_focus_row;             /* for the thumbnail threads: which rows to fetch first */
static int g_paused_thumbs;
static LONG g_thumbs_ok, g_thumbs_bad;  /* for the log */

/* script jobs */
enum { JOB_NONE, JOB_PAGE, JOB_SEARCH, JOB_PLAY, JOB_CHANNEL, JOB_PREVIEW, JOB_PLAYLIST };
typedef struct { int type, gen; char text[220]; } JOB;
static JOB g_job;                   /* the newest page/search job (a newer one replaces a waiting one) */
static JOB g_play_job;              /* a waiting "play this video" job: its own slot, so it never erases a page
                                       job (or the other way round) and it goes first */
static HANDLE g_job_event;

/* playback hand-over between the script thread and the screen */
enum { PS_IDLE, PS_LOADING, PS_READY, PS_FAILED };
static int g_play_state, g_play_seq;   /* g_play_seq: bumped by every play / cancel; late results are dropped */
static char g_play_msg[200], g_play_title[220];
static int g_play_seconds;
static sbplayer_info g_play_info;

/* everything the player screen shows about the video playing. Filled by the script thread in two steps (yt_streams
   before playback, yt_next a moment after it started), always under g_lock and only if seq is still g_play_seq */
#define MAX_Q 6
#define MAX_REL 20
#define URL_MAX 3000
#define MAX_CAPT 32
typedef struct { char author[64], text[600], age[40], likes[16], avatar[200]; int replies; } COMMENT;

/* the hold-A panel's details (description, channel, likes / dislikes, comments) for the video it's open on */
typedef struct {
    char id[16];
    int gen, have_next, have_votes;
    double likes_n, dislikes_n;
    char desc[2000], avatar[400], subs[48], published[64], likes[32];
    char cm_token[1200], cm_count[24];
    int ncm, cm_started, cm_loading;
    COMMENT cm[60];
} MDETAILS;
static MDETAILS g_md;
static char g_md_job[16];               /* details wanted for this video ("" = none waiting) */
static float g_md_scroll, g_md_h;
static int g_cmt_target;                /* the waiting comments page is for: 0 the player, 1 the hold-A panel */
typedef struct {
    int seq;
    VIDEO v;                            /* the video (from the tile it was started from, then the player API) */
    char audio[URL_MAX];
    int nq, q;                          /* quality choices (best first) + the one playing */
    int q_height[MAX_Q];
    char q_url[MAX_Q][URL_MAX];
    int sb, sb_numbered, sb_w, sb_h, sb_count, sb_cols, sb_rows;    /* preview sheets for seeking */
    double sb_interval;
    char sb_pre[1400], sb_post[400];
    int ncap;                           /* caption tracks (downloaded only when chosen) */
    char cap_name[MAX_CAPT][64], cap_url[MAX_CAPT][640];
    int nchap;                          /* chapters (from "0:00 Intro" lines in the description) */
    float chap_t[64];
    char chap_title[64][48];
    char cm_token[1200], cm_count[24];  /* comments: the token for the next 20 ("" = no more), "2.4M" in all */
    int ncm, cm_started, cm_loading;    /* how many arrived, asked at least once, a page on the way */
    COMMENT cm[60];
    int have_votes;                     /* Return YouTube Dislike answered: */
    double likes_n, dislikes_n;
    int have_next;                      /* yt_next answered: */
    char likes[32], subs[48], published[64], avatar[400], desc[2000], channel_id[40];
    int nrel;
    VIDEO rel[MAX_REL];                 /* "More Videos" */
} PLAYER;
static PLAYER g_pl;

/* the caption track being shown: lines sorted by start time; the text of all lines in one block */
typedef struct { float start, end; int text; } CAPLINE;
static CAPLINE *g_cap;
static char *g_cap_text;
static int g_cap_n, g_cap_req;          /* g_cap_req: bumped by every choice; late downloads are dropped */
enum { CS_OFF, CS_LOADING, CS_READY, CS_FAILED };
static int g_cap_state, g_cap_track;
static char g_cap_pref[64];             /* the track name chosen last: picked again for the next videos */
static int g_cap_job;                   /* "download this caption track" waiting (request number; 0 = none) */
static char g_cap_job_url[640];         /*  ... its address (runs before page jobs) */
/* LIVE streams: YouTube's live addresses stop working ~40 s after they're handed out, so every LIVE_REFRESH s the
   player asks the script for fresh ones (job_live, on the script thread); the new address for the playing quality
   is handed to the player engine by the player loop (g_live_fresh) */
#define LIVE_REFRESH 15
static int g_live_job;                  /* play seq of a waiting refresh (0 = none) */
static char g_live_job_id[16];
static int g_live_fresh;                /* 1 = g_pl.q_url[] were just refreshed */
static float g_speed = 1.0f;            /* playback speed (kept for the next videos) */

static int g_side_page;             /* (defined with the menu state below) */
/* channel page (owner's mockup): its shelves of videos live in g_sec like any page; the rest is here.
   Written by the script thread (job_channel) under g_lock, only while gen == g_gen */
#define CHAN_PAGE 50
#define CH_SEC_Y0    549.0f         /* first shelf (mockup: star badge + "Featured >>" + Play all + 4 tiles) */
#define CH_STEP      350.0f         /* shelf to shelf */
#define CH_TILE_W    274.0f
#define CH_TILE_STEP 294.0f
#define CH_SCROLL0   150.0f         /* page scroll when the first shelf has the focus */
typedef struct {
    int gen, state;                 /* 0 -, 1 loading, 2 ready, 3 failed */
    char id[40], title[96], handle[64], subs[48], views[48], avatar[400], banner[700], err[160];
} CHANNEL;
static CHANNEL g_chan;
static int g_chan_row = -1;         /* -1 = the Subscribe / Play all buttons, else the shelf g_focus_row */
static int g_chan_btn;              /* 0 Subscribe, 1 Play all */
static int g_chan_back = 1;         /* the page B goes back to */
static char g_open_channel[40];     /* the player asks for this channel once it has closed */
static char g_mtoast[160];          /* a short message on the menu */
static double g_mtoast_until;

/* "Play all": the videos played one after another (next / up next take the next one) */
#define MAX_QUEUE 100
static VIDEO g_queue[MAX_QUEUE];
static int g_queue_n, g_queue_pos;
static char g_queue_name[120];          /* what the list is ("Mix - ...", a playlist / shelf / list name): player row */
static int g_queue_mix;                 /* the queue is a Mix: it tops itself up as it runs low (endless) */
static int g_mix_job;                   /* 1 = start a Mix around g_mix_id, 2 = top the Mix up from g_mix_id */
static char g_mix_id[16];
static int g_loop;                      /* player settings: 0 off, 1 this video, 2 the whole list */
static int g_stats;                     /* "stats for nerds" box in the player */
static int g_sfilter;                   /* search results: the filter chosen with LB / RB (index into SFILTERS) */
static __int64 g_subs_cutoff;           /* Subscriptions: videos newer than this (seconds since 1601) are "NEW" */
#define SUBS_SEEN_FILE "game:\\subs_seen.txt"   /* when the Subscriptions page was last opened */

static __int64 wall_seconds(void)       /* the console's clock: seconds since 1601 (UTC) */
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((__int64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000;
}

/* opening the Subscriptions page: "NEW" = uploaded after the previous visit; this visit is remembered */
static void subs_visit(void)
{
    FILE *f = fopen(SUBS_SEEN_FILE, "rb");
    char line[40];
    __int64 now = wall_seconds();
    g_subs_cutoff = 0;
    if (f) { if (fgets(line, sizeof(line), f)) g_subs_cutoff = _atoi64(line); fclose(f); }
    if (g_subs_cutoff > now) g_subs_cutoff = 0;  /* (clock went back: no badges rather than wrong ones) */
    if ((f = fopen(SUBS_SEEN_FILE, "wb")) != NULL) { fprintf(f, "%I64d\r\n", now); fclose(f); }
}

/* a YouTube playlist's page: its videos (up to 96, from youtube.lua yt_playlist) in rows of 24 in g_sec (filter "@pl").
   A plays from that video on through the whole playlist, X plays it all, B goes back to where it was opened */
#define PL_PAGE 52
typedef struct {
    int gen, state, autoplay;       /* state 0 -, 1 loading, 2 ready, 3 failed; autoplay = X on the tile: play at once */
    char id[40], title[220], channel[96], count[32];
    int back;                       /* the page it was opened from (+ which channel / search, to go back to) */
    char back_text[120];
} PLAYLISTPAGE;
static PLAYLISTPAGE g_plp;
static void open_playlist(const VIDEO *v, int autoplay);
static void close_playlist(void);
static void play_rows(int row, int col);

static void draw_channel(void);     /* (in the channel section, after the player's picture helpers) */
static void pick_open(const VIDEO *v);  /* the "Save to..." playlist picker (menu + player) */
static void pick_draw(void);
static int pick_input(void);
static int g_pick;                  /* picker open */
static void note(const char *text); /* a short message in the menu or the player */
static void watch_later_toggle(const VIDEO *v);
static void draw_search_page(void);
static void draw_kb_block(void);
static int pimg_want(const char *url);
static p360_image *pimg_get(int i);
static void pill(float x, float y, float w, float h, const char *label, int focused);
static void draw_md_panel(const VIDEO *v);
static void md_open(const VIDEO *v);
/* playlist page (owner's mockup 2026-11-22): the chosen video big on the left with its details, the list on the right */
static char g_desc_job[16];         /* a description wanted for this video ("" = none waiting) */
static char g_desc_asked[16], g_desc_id[16], g_desc_text[1500];  /* asked last / the last one that came back */
static double g_desc_likes = -1, g_desc_dislikes = -1;   /* its votes (-1 = unknown) */
static float g_desc_scroll, g_desc_h;   /* description scroll (right stick) and its full height */
static float g_rsy;                 /* right stick up/down, -1..1 (0 inside the dead zone) */
static double g_pl_moved;           /* when the playlist cursor last moved (details load after a short rest) */
static float g_pl_sy;               /* the list's scroll (animated) */
static int g_pl_top;                /* first list row kept in view */
static int g_sf;                    /* (search page focus; described with the search page) */
static float g_kb_t = 0;            /* search keyboard block: 1 = in view, 0 = slid up out of view (animated) */
#define KB_SLIDE 352.0f             /* how far it slides (the results move up by the same amount) */
#define ON_RESULTS (g_side_page == 0 && g_q[0])
static void search_input(void);
static void open_search_page(void);
static void open_channel(const char *id);
static void play_list(SECTION *s, int from);
static void channel_input(void);
static void mtoast(const char *text);   /* a short message on the menu (below) */

static void caption_free(void)          /* g_lock held */
{
    free(g_cap); g_cap = NULL;
    free(g_cap_text); g_cap_text = NULL;
    g_cap_n = 0;
}

static double now_s(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

static void say(const char *fmt, ...)
{
    char line[1024];
    va_list a;
    va_start(a, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, a);
    line[sizeof(line) - 1] = 0;
    va_end(a);
    p360_log(line);
}
static void lib_log(const char *line) { say("  %s", line); }

static void copy_str(char *dst, int max, const char *src);
static void mtoast(const char *text)
{
    copy_str(g_mtoast, sizeof(g_mtoast), text);
    g_mtoast_until = now_s() + 3;
}

/* LOCAL SUBSCRIPTIONS: kept by NewTube itself in game:\subscriptions.txt (one "channel id <TAB> name" per line),
   no YouTube account needed. The Subscriptions page shows their newest uploads. Guarded by g_lock. */
#define MAX_SUBS 100
#define SUBS_FILE "game:\\subscriptions.txt"
typedef struct { char id[40], title[96], avatar[300]; } SUB;   /* (avatar = the channel picture's address) */
static SUB g_subs[MAX_SUBS];
static int g_nsubs;
static char g_sub_vid[40];          /* the rows show: "" = All subscriptions, else this channel (its id) */

static void subs_load(void)
{
    FILE *f = fopen(SUBS_FILE, "rb");
    char line[520];
    g_nsubs = 0;
    if (!f) return;
    while (g_nsubs < MAX_SUBS && fgets(line, sizeof(line), f)) {   /* "id <TAB> name [<TAB> picture address]" */
        char *tab = strchr(line, '\t'), *end, *tab2;
        if (!tab) continue;
        *tab = 0;
        end = tab + 1 + strcspn(tab + 1, "\r\n");
        *end = 0;
        if (!line[0]) continue;
        tab2 = strchr(tab + 1, '\t');
        if (tab2) *tab2 = 0;
        memset(&g_subs[g_nsubs], 0, sizeof(SUB));
        copy_str(g_subs[g_nsubs].id, sizeof(g_subs[0].id), line);
        copy_str(g_subs[g_nsubs].title, sizeof(g_subs[0].title), tab + 1);
        if (tab2) copy_str(g_subs[g_nsubs].avatar, sizeof(g_subs[0].avatar), tab2 + 1);
        g_nsubs++;
    }
    fclose(f);
}

static void subs_save(void)
{
    FILE *f = fopen(SUBS_FILE, "wb");
    int i;
    if (!f) { mtoast("Couldn't save your subscriptions (the hard drive refused)."); return; }
    for (i = 0; i < g_nsubs; i++)
        if (g_subs[i].avatar[0]) fprintf(f, "%s\t%s\t%s\r\n", g_subs[i].id, g_subs[i].title, g_subs[i].avatar);
        else fprintf(f, "%s\t%s\r\n", g_subs[i].id, g_subs[i].title);
    fclose(f);
}

static int sub_find(const char *id)         /* -1 = not subscribed */
{
    int i;
    for (i = 0; i < g_nsubs; i++) if (!strcmp(g_subs[i].id, id)) return i;
    return -1;
}

/* RECENT SEARCHES (game:\searches.txt, newest first) + the SEARCH PAGE's shared state. Guarded by g_lock. */
#define MAX_RECENT 20
#define RECENT_FILE "game:\\searches.txt"
#define Q_MAX 100
static char g_recent[MAX_RECENT][Q_MAX];
static int g_nrecent;
#define SEARCH_PAGE 51                      /* g_side_page: the search page (typing); results = page 0 as before */
#define SETTINGS_PAGE 53                    /* g_side_page: the Settings page (sidebar gear = cursor N_SIDE) */
static void open_settings(void);
static void draw_settings_page(void);
static void settings_input(void);
static char g_q[Q_MAX];                     /* what's typed */
static char g_sug[8][Q_MAX], g_sug_for[Q_MAX];  /* suggestions (for the text g_sug_for) */
static int g_nsug, g_sug_req;
static int g_cmt_job;                       /* comments page waiting: the video's play seq (0 = none) */
static char g_cmt_tok[1200];
static int g_sug_job;                       /* "suggestions for g_sug_job_q" waiting (request number; 0 = none) */
static char g_sug_job_q[Q_MAX];

static void recent_load(void)
{
    FILE *f = fopen(RECENT_FILE, "rb");
    char line[Q_MAX + 4];
    g_nrecent = 0;
    if (!f) return;
    while (g_nrecent < MAX_RECENT && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) copy_str(g_recent[g_nrecent++], Q_MAX, line);
    }
    fclose(f);
}

static void recent_add(const char *q)       /* to the top (once; same words in other capitals = the same) */
{
    int i;
    FILE *f;
    if (!q || !q[0]) return;
    for (i = 0; i < g_nrecent; i++)
        if (!_stricmp(g_recent[i], q)) {
            memmove(g_recent[i], g_recent[i + 1], (g_nrecent - i - 1) * Q_MAX);
            g_nrecent--;
            break;
        }
    if (g_nrecent == MAX_RECENT) g_nrecent--;
    memmove(g_recent[1], g_recent[0], g_nrecent * Q_MAX);
    copy_str(g_recent[0], Q_MAX, q);
    for (i = 0; g_recent[0][i]; i++) if (g_recent[0][i] == '\r' || g_recent[0][i] == '\n') g_recent[0][i] = ' ';
    g_nrecent++;
    f = fopen(RECENT_FILE, "wb");
    if (!f) return;
    for (i = 0; i < g_nrecent; i++) fprintf(f, "%s\r\n", g_recent[i]);
    fclose(f);
}

/* SETTINGS (the sidebar gear): game:\settings.txt, one "name value" per line. Read at start, written on every change */
#define NEWTUBE_VERSION "1.0"
/* SELF-UPDATE: at start NewTube reads <address>/version.txt (line 1 = the newest youtube.lua's SCRIPT_VERSION, line 2
   optional = the oldest NewTube it works with) and, if that differs from the script it has, fetches
   <address>/youtube.lua, checks it compiles, runs it and keeps it (the old one = youtube.lua.bak, used if the new one
   ever fails to load). The address: game:\update_url.txt (first line) if present, else this: */
#define UPDATE_URL_DEFAULT "https://raw.githubusercontent.com/sebbz360/NewTube/main"   /* the owner's repo */
#define SETTINGS_FILE "game:\\settings.txt"
static const int QUALITY_CHOICES[4] = { 240, 360, 480, 720 };
typedef struct {
    int quality;        /* index into QUALITY_CHOICES: the height a video starts in (Lua MAX_HEIGHT) */
    int autoplay;       /* 1 = after a video, play the first of More Videos (a "Play all" list always goes on) */
    int captions;       /* 1 = turn captions on again in the language used last, 0 = always start without */
    int keep_speed;     /* 1 = new videos keep the last playback speed, 0 = always start at normal speed */
    int resume;         /* 0 = ask, 1 = always resume, 2 = always start from the beginning */
    int region;         /* -1 = automatic (the console's own country setting), else an index into REGIONS */
    int textsize;       /* 0 = automatic (larger on standard-definition TVs), 1 = normal, 2 = larger */
    int autoupdate;     /* 1 = fetch YouTube-script fixes at start (see SELF-UPDATE) */
} SETTINGS;
static SETTINGS g_set = { 2, 1, 1, 1, 0, -1, 0, 1 };
static char g_update_note[120];     /* what the last update check said (Settings' About line) */
static char g_script_ver[48];   /* youtube.lua's SCRIPT_VERSION (for About) */

/* REGION: the country YouTube picks the Home / Gaming / Music / Movies videos (and search results) for. Without one
   YouTube answers for the whole world. Automatic = the country set in the console's own system settings */
typedef struct { const char *code, *name; } REGIONDEF;
static const REGIONDEF REGIONS[] = {
    { "", "Worldwide" },
    { "DZ", "Algeria" }, { "AR", "Argentina" }, { "AU", "Australia" }, { "AT", "Austria" }, { "BD", "Bangladesh" },
    { "BE", "Belgium" }, { "BR", "Brazil" }, { "BG", "Bulgaria" }, { "CA", "Canada" }, { "CL", "Chile" },
    { "CO", "Colombia" }, { "HR", "Croatia" }, { "CZ", "Czechia" }, { "DK", "Denmark" }, { "EG", "Egypt" },
    { "FI", "Finland" }, { "FR", "France" }, { "DE", "Germany" }, { "GR", "Greece" }, { "HK", "Hong Kong" },
    { "HU", "Hungary" }, { "IN", "India" }, { "ID", "Indonesia" }, { "IE", "Ireland" }, { "IL", "Israel" },
    { "IT", "Italy" }, { "JP", "Japan" }, { "KE", "Kenya" }, { "MY", "Malaysia" }, { "MX", "Mexico" },
    { "MA", "Morocco" }, { "NL", "Netherlands" }, { "NZ", "New Zealand" }, { "NG", "Nigeria" }, { "NO", "Norway" },
    { "PK", "Pakistan" }, { "PE", "Peru" }, { "PH", "Philippines" }, { "PL", "Poland" }, { "PT", "Portugal" },
    { "PR", "Puerto Rico" }, { "RO", "Romania" }, { "RU", "Russia" }, { "SA", "Saudi Arabia" }, { "RS", "Serbia" },
    { "SG", "Singapore" }, { "SK", "Slovakia" }, { "ZA", "South Africa" }, { "KR", "South Korea" }, { "ES", "Spain" },
    { "SE", "Sweden" }, { "CH", "Switzerland" }, { "TW", "Taiwan" }, { "TH", "Thailand" }, { "TR", "Turkey" },
    { "UA", "Ukraine" }, { "AE", "United Arab Emirates" }, { "GB", "United Kingdom" }, { "US", "United States" },
    { "VE", "Venezuela" }, { "VN", "Vietnam" },
};
#define N_REGIONS ((int)(sizeof(REGIONS) / sizeof(REGIONS[0])))
/* the console's country (XGetLocale: XC_LOCALE_* in the XDK's xconfig.h, 1..43) -> its code */
static const char *LOCALE_CODE[44] = { "",
    "AU", "AT", "BE", "BR", "CA", "CL", "CN", "CO", "CZ", "DK", "FI", "FR", "DE", "GR", "HK", "HU", "IN", "IE", "IT",
    "JP", "KR", "MX", "NL", "NZ", "NO", "PL", "PT", "SG", "SK", "ZA", "ES", "SE", "CH", "TW", "GB", "US", "RU",
    "" /* 38 = worldwide */, "TR", "AR", "SA", "IL", "AE" };
static int g_region_ver;            /* bumped on every change: the script thread hands the new one to youtube.lua */

static const char *region_code(void)
{
    if (g_set.region >= 0 && g_set.region < N_REGIONS) return REGIONS[g_set.region].code;
    {
        DWORD loc = XGetLocale();
        return loc < 44 ? LOCALE_CODE[loc] : "";
    }
}

static const char *region_name(const char *code)
{
    int i;
    for (i = 0; i < N_REGIONS; i++) if (!strcmp(REGIONS[i].code, code)) return REGIONS[i].name;
    return code;                    /* (China: set on the console, not in the list: YouTube isn't offered there) */
}

static void settings_save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "wb");
    if (!f) return;
    fprintf(f, "quality %d\r\nautoplay %d\r\ncaptions %d\r\nkeep_speed %d\r\nresume %d\r\nregion %s\r\ntext %d\r\n"
            "updates %d\r\n",
            QUALITY_CHOICES[g_set.quality], g_set.autoplay, g_set.captions, g_set.keep_speed, g_set.resume,
            g_set.region < 0 ? "auto" : REGIONS[g_set.region].code[0] ? REGIONS[g_set.region].code : "world",
            g_set.textsize, g_set.autoupdate);
    fclose(f);
}

static void settings_load(void)
{
    FILE *f = fopen(SETTINGS_FILE, "rb");
    char line[80], name[40], text[40];
    int v, i;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%39s %39s", name, text) != 2) continue;
        if (!strcmp(name, "region")) {
            g_set.region = -1;
            for (i = 0; i < N_REGIONS; i++)
                if (!strcmp(text, REGIONS[i].code[0] ? REGIONS[i].code : "world")) g_set.region = i;
            continue;
        }
        v = atoi(text);
        if (!strcmp(name, "quality")) { for (i = 0; i < 4; i++) if (QUALITY_CHOICES[i] == v) g_set.quality = i; }
        else if (!strcmp(name, "autoplay")) g_set.autoplay = v != 0;
        else if (!strcmp(name, "captions")) g_set.captions = v != 0;
        else if (!strcmp(name, "keep_speed")) g_set.keep_speed = v != 0;
        else if (!strcmp(name, "resume") && v >= 0 && v <= 2) g_set.resume = v;
        else if (!strcmp(name, "text") && v >= 0 && v <= 2) g_set.textsize = v;
        else if (!strcmp(name, "updates")) g_set.autoupdate = v != 0;
    }
    fclose(f);
}

/* the text size setting -> the drawing layer (automatic = larger on a standard-definition picture) */
static int text_larger(void) { return g_set.textsize == 2 || (g_set.textsize == 0 && p360_display_sd()); }
static void apply_text_size(void) { p360_text_boost(text_larger() ? 0.5f : 0.0f); }

/* WATCH HISTORY: every video played, newest first, with where you stopped (game:\history.txt, one line each:
   id pos done seconds title channel views age duration, TAB-separated). Feeds "Continue watching", the Account
   page's history and the red progress bars under thumbnails. Guarded by g_lock. */
#define MAX_WATCH 200
#define WATCH_FILE "game:\\history.txt"
typedef struct { VIDEO v; int pos, done; } WATCH;
static WATCH g_watch[MAX_WATCH];
static int g_nwatch;

static void clean_tabs(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\r' || *s == '\n') *s = ' '; }

static void watch_load(void)
{
    FILE *f = fopen(WATCH_FILE, "rb");
    static char line[1024];
    g_nwatch = 0;
    if (!f) return;
    while (g_nwatch < MAX_WATCH && fgets(line, sizeof(line), f)) {
        char *part[9], *p = line;
        int k;
        WATCH *w = &g_watch[g_nwatch];
        line[strcspn(line, "\r\n")] = 0;
        for (k = 0; k < 9; k++) {           /* split at the TABs */
            part[k] = p;
            p = strchr(p, '\t');
            if (!p) { k++; break; }
            *p++ = 0;
        }
        if (k < 9) continue;
        memset(w, 0, sizeof(*w));
        copy_str(w->v.id, sizeof(w->v.id), part[0]);
        w->pos = atoi(part[1]);
        w->done = atoi(part[2]);
        w->v.seconds = atoi(part[3]);
        copy_str(w->v.title, sizeof(w->v.title), part[4]);
        copy_str(w->v.channel, sizeof(w->v.channel), part[5]);
        copy_str(w->v.views, sizeof(w->v.views), part[6]);
        copy_str(w->v.age, sizeof(w->v.age), part[7]);
        copy_str(w->v.dur, sizeof(w->v.dur), part[8]);
        if (w->v.id[0]) g_nwatch++;
    }
    fclose(f);
}

static void watch_save(void)
{
    FILE *f = fopen(WATCH_FILE, "wb");
    int i;
    if (!f) return;
    for (i = 0; i < g_nwatch; i++) {
        VIDEO *v = &g_watch[i].v;
        fprintf(f, "%s\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\r\n", v->id, g_watch[i].pos, g_watch[i].done, v->seconds,
                v->title, v->channel, v->views, v->age, v->dur);
    }
    fclose(f);
}

static int watch_find(const char *id)
{
    int i;
    for (i = 0; i < g_nwatch; i++) if (!strcmp(g_watch[i].v.id, id)) return i;
    return -1;
}

/* the video v was watched up to pos (of seconds): to the front of the history. Near the end = watched ("done") */
static void watch_update(const VIDEO *v, int pos, int seconds)
{
    int i = watch_find(v->id);
    WATCH w;
    if (!v->id[0]) return;
    memset(&w, 0, sizeof(w));
    w.v = *v;
    w.v.img = NULL; w.v.pix = NULL; w.v.thumb = 0; w.v.fresh = 0;
    if (seconds > 0) w.v.seconds = seconds;
    clean_tabs(w.v.title); clean_tabs(w.v.channel); clean_tabs(w.v.views); clean_tabs(w.v.age); clean_tabs(w.v.dur);
    w.done = w.v.seconds > 0 && (pos > w.v.seconds - 20 || pos > w.v.seconds * 0.95);
    w.pos = w.done ? 0 : pos;
    if (i >= 0) { memmove(&g_watch[i], &g_watch[i + 1], (g_nwatch - i - 1) * sizeof(WATCH)); g_nwatch--; }
    if (g_nwatch == MAX_WATCH) g_nwatch--;      /* full: the oldest goes */
    memmove(&g_watch[1], &g_watch[0], g_nwatch * sizeof(WATCH));
    g_watch[0] = w;
    g_nwatch++;
    watch_save();
}

static int watch_resume(const char *id)     /* where to resume (0 = from the start) */
{
    int i = watch_find(id);
    return i >= 0 && !g_watch[i].done && g_watch[i].pos >= 10 ? g_watch[i].pos : 0;
}

static float watch_progress(const char *id)  /* for the red bar under a thumbnail: -1 = never played */
{
    int i = watch_find(id);
    if (i < 0) return -1;
    if (g_watch[i].done) return 1;
    return g_watch[i].v.seconds > 0 ? (float)g_watch[i].pos / g_watch[i].v.seconds : -1;
}

/* PLAYLISTS (local): list 0 is always "Watch later"; game:\playlists.txt = "#name" lines, each followed by its
   videos ("id seconds title channel views age duration", TAB-separated). Guarded by g_lock. */
#define MAX_LISTS 16
#define MAX_LIST_VIDEOS 50
#define LISTS_FILE "game:\\playlists.txt"
typedef struct { char name[64]; int n; VIDEO v[MAX_LIST_VIDEOS]; } PLIST;
static PLIST g_lists[MAX_LISTS];
static int g_nlists;

static void lists_save(void)
{
    FILE *f = fopen(LISTS_FILE, "wb");
    int i, k;
    if (!f) { mtoast("Couldn't save your playlists (the hard drive refused)."); return; }
    for (i = 0; i < g_nlists; i++) {
        fprintf(f, "#%s\r\n", g_lists[i].name);
        for (k = 0; k < g_lists[i].n; k++) {
            VIDEO *v = &g_lists[i].v[k];
            fprintf(f, "%s\t%d\t%s\t%s\t%s\t%s\t%s\r\n", v->id, v->seconds, v->title, v->channel, v->views, v->age,
                    v->dur);
        }
    }
    fclose(f);
}

static void lists_load(void)
{
    FILE *f = fopen(LISTS_FILE, "rb");
    static char line[1024];
    g_nlists = 0;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '#') {
                if (g_nlists == MAX_LISTS) break;
                memset(&g_lists[g_nlists], 0, sizeof(PLIST));
                copy_str(g_lists[g_nlists].name, sizeof(g_lists[0].name), line + 1);
                g_nlists++;
            } else if (g_nlists && line[0]) {
                PLIST *l = &g_lists[g_nlists - 1];
                char *part[7], *p = line;
                int k;
                for (k = 0; k < 7; k++) {
                    part[k] = p;
                    p = strchr(p, '\t');
                    if (!p) { k++; break; }
                    *p++ = 0;
                }
                if (k < 7 || l->n == MAX_LIST_VIDEOS) continue;
                memset(&l->v[l->n], 0, sizeof(VIDEO));
                copy_str(l->v[l->n].id, sizeof(l->v[0].id), part[0]);
                l->v[l->n].seconds = atoi(part[1]);
                copy_str(l->v[l->n].title, sizeof(l->v[0].title), part[2]);
                copy_str(l->v[l->n].channel, sizeof(l->v[0].channel), part[3]);
                copy_str(l->v[l->n].views, sizeof(l->v[0].views), part[4]);
                copy_str(l->v[l->n].age, sizeof(l->v[0].age), part[5]);
                copy_str(l->v[l->n].dur, sizeof(l->v[0].dur), part[6]);
                l->n++;
            }
        }
        fclose(f);
    }
    if (!g_nlists || strcmp(g_lists[0].name, "Watch later")) {     /* Watch later always comes first */
        if (g_nlists == MAX_LISTS) g_nlists--;
        memmove(&g_lists[1], &g_lists[0], g_nlists * sizeof(PLIST));
        memset(&g_lists[0], 0, sizeof(PLIST));
        copy_str(g_lists[0].name, sizeof(g_lists[0].name), "Watch later");
        g_nlists++;
    }
}

static int list_has(int li, const char *id)
{
    int k;
    for (k = 0; k < g_lists[li].n; k++) if (!strcmp(g_lists[li].v[k].id, id)) return k;
    return -1;
}

/* the video in or out of list li (newest at the front). 1 = it's in now */
static int list_toggle(int li, const VIDEO *v)
{
    PLIST *l = &g_lists[li];
    int k = list_has(li, v->id);
    if (k >= 0) {
        memmove(&l->v[k], &l->v[k + 1], (l->n - k - 1) * sizeof(VIDEO));
        l->n--;
        lists_save();
        return 0;
    }
    if (l->n == MAX_LIST_VIDEOS) l->n--;        /* full: the oldest goes */
    memmove(&l->v[1], &l->v[0], l->n * sizeof(VIDEO));
    l->v[0] = *v;
    l->v[0].img = NULL; l->v[0].pix = NULL; l->v[0].thumb = 0; l->v[0].fresh = 0;
    clean_tabs(l->v[0].title); clean_tabs(l->v[0].channel); clean_tabs(l->v[0].views); clean_tabs(l->v[0].age);
    clean_tabs(l->v[0].dur);
    l->n++;
    lists_save();
    return 1;
}

static int list_new(const char *name)       /* -1 = no room */
{
    char clean[64];
    int i;
    if (g_nlists == MAX_LISTS) return -1;
    copy_str(clean, sizeof(clean), name);
    for (i = 0; clean[i]; i++) if (clean[i] == '\r' || clean[i] == '\n' || clean[i] == '\t') clean[i] = ' ';
    if (clean[0] == '#') clean[0] = ' ';
    memset(&g_lists[g_nlists], 0, sizeof(PLIST));
    copy_str(g_lists[g_nlists].name, sizeof(g_lists[0].name), clean);
    g_nlists++;
    lists_save();
    return g_nlists - 1;
}

static void list_delete(int li)             /* (never Watch later) */
{
    if (li <= 0 || li >= g_nlists) return;
    memmove(&g_lists[li], &g_lists[li + 1], (g_nlists - li - 1) * sizeof(PLIST));
    g_nlists--;
    lists_save();
}

/* OFFLINE PLAYER: videos saved on the hard drive, playable without internet.
   game:\offline\<id>\video.mp4 (the picture stream), audio.m4a (the sound), thumb.jpg; game:\offline.txt = the saved
   videos, newest first ("id seconds height KB title channel views age duration", TAB-separated). A video is only
   listed there once all its files are written, so a cut-off download never shows up as saved (its folder is cleaned
   up at the next start). Downloads: the script thread gets the links (job_dl_links), the download thread
   (dl_thread) writes the files. Guarded by g_lock. */
#define OFF_DIR "game:\\offline"
#define OFF_FILE "game:\\offline.txt"
#define MAX_OFF 72                      /* 3 rows of 24 */
#define MAX_DL 12
typedef struct { VIDEO v; int height, kb; } OFFVID;
static OFFVID g_off[MAX_OFF];
static int g_noff;
enum { DL_WAIT, DL_LINKS, DL_READY, DL_RUN, DL_FAILED };
typedef struct {
    VIDEO v;
    int state, height, cancel;
    long long got, total;
    char err[100], vurl[URL_MAX], aurl[URL_MAX];
} DLITEM;
static DLITEM g_dl[MAX_DL];
static int g_ndl;
static int g_dl_hold;                   /* 1 while a video STREAMS (downloads wait, so the stream gets the network) */
static int g_off_dirty;                 /* the Offline Player page needs rebuilding (done on the screen thread) */
static int g_pl_offline;                /* the video playing comes from the hard drive */
static HANDLE g_dl_event;
static char g_off_arm[16];              /* "press RB again to delete" for this video... */
static double g_off_arm_until;          /*  ... until then */
#define OFFLINE_PAGE 7                  /* g_side_page (the sidebar row) */
static int own_id_shape(const char *id);    /* (your own video files: below) */

static void offline_path(char *out, int n, const char *id, const char *file)
{
    _snprintf(out, n - 1, "%s\\%s%s%s", OFF_DIR, id, file ? "\\" : "", file ? file : "");
    out[n - 1] = 0;
}

static int offline_find(const char *id)
{
    int i;
    for (i = 0; i < g_noff; i++) if (!strcmp(g_off[i].v.id, id)) return i;
    return -1;
}

static int offline_find_nocase(const char *name)   /* (folder names may come back in other capitals: FATX) */
{
    int i;
    for (i = 0; i < g_noff; i++) if (!_stricmp(g_off[i].v.id, name)) return i;
    return -1;
}

static int dl_find(const char *id)
{
    int i;
    for (i = 0; i < g_ndl; i++) if (!strcmp(g_dl[i].v.id, id)) return i;
    return -1;
}

static void offline_delete_files(const char *id)    /* the video's folder and everything in it */
{
    char dir[96], pat[100], f[200];
    WIN32_FIND_DATA fd;
    HANDLE h;
    if (!id[0] || strchr(id, '\\') || strchr(id, '.')) return;     /* (never anything but a video id's folder) */
    offline_path(dir, sizeof(dir), id, NULL);
    _snprintf(pat, sizeof(pat) - 1, "%s\\*", dir);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            _snprintf(f, sizeof(f) - 1, "%s\\%s", dir, fd.cFileName);
            f[sizeof(f) - 1] = 0;
            DeleteFile(f);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    RemoveDirectory(dir);
}

/* a folder NewTube made itself: named like a YouTube video id (11 letters/digits/-/_) and holding nothing but our own
   file names. Anything else in the offline folder belongs to the user and is never deleted */
static int offline_is_ours(const char *name)
{
    char pat[100];
    WIN32_FIND_DATA fd;
    HANDLE h;
    int i, ours = 1;
    if (strlen(name) != 11) return 0;
    for (i = 0; i < 11; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    _snprintf(pat, sizeof(pat) - 1, "%s\\%s\\*", OFF_DIR, name);
    pat[sizeof(pat) - 1] = 0;
    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;        /* (empty) */
    do {
        if (fd.cFileName[0] == '.') continue;
        if (_stricmp(fd.cFileName, "video.mp4") && _stricmp(fd.cFileName, "audio.m4a") &&
            _stricmp(fd.cFileName, "thumb.jpg")) ours = 0;
    } while (ours && FindNextFile(h, &fd));
    FindClose(h);
    return ours;
}

static void offline_save(void)
{
    FILE *f = fopen(OFF_FILE, "wb");
    int i;
    if (!f) { mtoast("Couldn't save the Offline Player list (the hard drive refused)."); return; }
    for (i = 0; i < g_noff; i++) {
        VIDEO *v = &g_off[i].v;
        fprintf(f, "%s\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\r\n", v->id, v->seconds, g_off[i].height, g_off[i].kb,
                v->title, v->channel, v->views, v->age, v->dur);
    }
    fclose(f);
}

static void offline_load(void)      /* + cleans up folders of downloads that never finished */
{
    FILE *f = fopen(OFF_FILE, "rb");
    static char line[1024];
    char path[120];
    WIN32_FIND_DATA fd;
    HANDLE h;
    int dropped = 0;
    g_noff = 0;
    CreateDirectory(OFF_DIR, NULL);
    if (f) {
        while (g_noff < MAX_OFF && fgets(line, sizeof(line), f)) {
            char *part[9], *p = line;
            int k;
            OFFVID *o = &g_off[g_noff];
            line[strcspn(line, "\r\n")] = 0;
            for (k = 0; k < 9; k++) {
                part[k] = p;
                p = strchr(p, '\t');
                if (!p) { k++; break; }
                *p++ = 0;
            }
            if (k < 9 || !part[0][0]) continue;
            memset(o, 0, sizeof(*o));
            copy_str(o->v.id, sizeof(o->v.id), part[0]);
            o->v.seconds = atoi(part[1]);
            o->height = atoi(part[2]);
            o->kb = atoi(part[3]);
            copy_str(o->v.title, sizeof(o->v.title), part[4]);
            copy_str(o->v.channel, sizeof(o->v.channel), part[5]);
            copy_str(o->v.views, sizeof(o->v.views), part[6]);
            copy_str(o->v.age, sizeof(o->v.age), part[7]);
            copy_str(o->v.dur, sizeof(o->v.dur), part[8]);
            offline_path(path, sizeof(path), o->v.id, "video.mp4");
            if (GetFileAttributes(path) == (DWORD)-1) { dropped++; continue; }  /* files gone (-1 = no such file) */
            g_noff++;
        }
        fclose(f);
    }
    if (dropped) offline_save();
    h = FindFirstFile(OFF_DIR "\\*", &fd);          /* folders not in the list = unfinished downloads */
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' &&
                offline_find_nocase(fd.cFileName) < 0 && offline_is_ours(fd.cFileName)) {
                say("offline: removing the unfinished download %s", fd.cFileName);
                offline_delete_files(fd.cFileName);
            }
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
}

/* RB on a video (info panel / player description): save it, cancel its download, or (pressed twice) delete it */
static void offline_toggle(const VIDEO *v)  /* g_lock held */
{
    int i;
    char t[160];
    if (!v->id[0] || v->list[0]) { note("Playlists can't be saved as a whole (save their videos one by one)."); return; }
    if (own_id_shape(v->id)) { note("That's your own file: it stays until you remove it from the offline folder."); return; }
    if ((i = dl_find(v->id)) >= 0) {
        if (g_dl[i].state == DL_RUN) g_dl[i].cancel = 1;            /* (the download thread cleans up) */
        else { memmove(&g_dl[i], &g_dl[i + 1], (g_ndl - i - 1) * sizeof(DLITEM)); g_ndl--; }
        g_off_dirty = 1;
        note("Saving cancelled.");
        return;
    }
    if ((i = offline_find(v->id)) >= 0) {
        if (strcmp(g_off_arm, v->id) || now_s() > g_off_arm_until) {
            copy_str(g_off_arm, sizeof(g_off_arm), v->id);
            g_off_arm_until = now_s() + 4;
            note("Press RB again to delete it from your console.");
            return;
        }
        g_off_arm[0] = 0;
        offline_delete_files(v->id);
        memmove(&g_off[i], &g_off[i + 1], (g_noff - i - 1) * sizeof(OFFVID));
        g_noff--;
        offline_save();
        g_off_dirty = 1;
        note("Deleted from your console.");
        return;
    }
    if (!strcmp(v->dur, "LIVE")) { note("Live streams can't be saved."); return; }
    if (g_noff + g_ndl >= MAX_OFF) { note("The Offline Player is full (72 videos): delete some first."); return; }
    if (g_ndl >= MAX_DL) { note("12 videos are waiting to be saved already: wait for some to finish."); return; }
    memset(&g_dl[g_ndl], 0, sizeof(DLITEM));
    g_dl[g_ndl].v = *v;
    g_dl[g_ndl].v.img = NULL; g_dl[g_ndl].v.pix = NULL; g_dl[g_ndl].v.thumb = 0;
    clean_tabs(g_dl[g_ndl].v.title); clean_tabs(g_dl[g_ndl].v.channel); clean_tabs(g_dl[g_ndl].v.views);
    clean_tabs(g_dl[g_ndl].v.age); clean_tabs(g_dl[g_ndl].v.dur);
    g_ndl++;
    g_off_dirty = 1;
    if (g_job_event) SetEvent(g_job_event);         /* the script thread gets its links */
    _snprintf(t, sizeof(t) - 1, "Saving to your console: %.100s", v->title);
    t[sizeof(t) - 1] = 0;
    note(t);
}

/* YOUR OWN VIDEO FILES: any .mp4 .m4v .mov .mkv .ts put into the offline folder (or one folder down, e.g.
   offline\Movies\film.mkv) shows up on the Offline Player page. Each gets an id "f" + 8 hex digits made from its path,
   so the watch history / resume / Watch later work for it like for a YouTube video (YouTube ids are 11 long: no
   clash). Rescanned every time the page is built. g_lock held */
#define MAX_OWN 96
typedef struct { char path[260]; VIDEO v; } OWNFILE;
static OWNFILE g_own[MAX_OWN];
static int g_nown;

static int own_id_shape(const char *id)     /* "f" + 8 hex digits */
{
    int i;
    if (id[0] != 'f' || strlen(id) != 9) return 0;
    for (i = 1; i < 9; i++) if (!strchr("0123456789abcdef", id[i])) return 0;
    return 1;
}

static int own_find(const char *id)
{
    int i;
    for (i = 0; i < g_nown; i++) if (!strcmp(g_own[i].v.id, id)) return i;
    return -1;
}

static void own_add(const char *dir, const char *name, const char *folder, unsigned int size_lo, unsigned int size_hi)
{
    static const char *EXT[] = { ".mp4", ".m4v", ".mov", ".mkv", ".ts" };
    const char *dot = strrchr(name, '.');
    OWNFILE *o;
    unsigned int hash = 2166136261u;
    double mb = (size_hi * 4294967296.0 + size_lo) / (1024.0 * 1024.0);
    int i, ok = 0;
    char *p;
    if (!dot || g_nown >= MAX_OWN) return;
    for (i = 0; i < 5; i++) if (!_stricmp(dot, EXT[i])) ok = 1;
    if (!ok) return;
    o = &g_own[g_nown];
    memset(o, 0, sizeof(*o));
    _snprintf(o->path, sizeof(o->path) - 1, "%s\\%s", dir, name);
    for (p = o->path; *p; p++) hash = (hash ^ (unsigned char)tolower(*p)) * 16777619u;   /* FNV-1a of the path */
    _snprintf(o->v.id, sizeof(o->v.id) - 1, "f%08x", hash);
    copy_str(o->v.title, sizeof(o->v.title), name);
    if ((p = strrchr(o->v.title, '.')) != NULL) *p = 0;             /* no extension; dots/_ between words -> spaces */
    for (p = o->v.title; *p; p++) if (*p == '.' || *p == '_') *p = ' ';
    copy_str(o->v.channel, sizeof(o->v.channel), folder);
    copy_str(o->v.views, sizeof(o->v.views), "Your file");
    copy_str(o->v.age, sizeof(o->v.age), dot + 1);
    for (p = o->v.age; *p; p++) *p = (char)toupper(*p);
    if (mb >= 1024) _snprintf(o->v.dur, sizeof(o->v.dur) - 1, "%.1f GB", mb / 1024);
    else _snprintf(o->v.dur, sizeof(o->v.dur) - 1, "%.0f MB", mb < 1 ? 1.0 : mb);
    g_nown++;
}

static int own_cmp(const void *a, const void *b)
{
    return _stricmp(((const OWNFILE *)a)->v.title, ((const OWNFILE *)b)->v.title);
}

static void own_scan(void)
{
    WIN32_FIND_DATA fd, fd2;
    HANDLE h, h2;
    char sub[300], pat[310];
    g_nown = 0;
    h = FindFirstFile(OFF_DIR "\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            own_add(OFF_DIR, fd.cFileName, "Offline folder", fd.nFileSizeLow, fd.nFileSizeHigh);
            continue;
        }
        if (offline_find_nocase(fd.cFileName) >= 0 || offline_is_ours(fd.cFileName)) continue;   /* NewTube's own saves */
        _snprintf(sub, sizeof(sub) - 1, "%s\\%s", OFF_DIR, fd.cFileName);
        sub[sizeof(sub) - 1] = 0;
        _snprintf(pat, sizeof(pat) - 1, "%s\\*", sub);
        pat[sizeof(pat) - 1] = 0;
        h2 = FindFirstFile(pat, &fd2);
        if (h2 == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                own_add(sub, fd2.cFileName, fd.cFileName, fd2.nFileSizeLow, fd2.nFileSizeHigh);
        } while (FindNextFile(h2, &fd2));
        FindClose(h2);
    } while (FindNextFile(h, &fd));
    FindClose(h);
    qsort(g_own, g_nown, sizeof(OWNFILE), own_cmp);
}

static int sub_toggle(const char *id, const char *title)    /* 1 = now subscribed */
{
    int i = sub_find(id);
    if (i >= 0) {
        memmove(&g_subs[i], &g_subs[i + 1], (g_nsubs - i - 1) * sizeof(SUB));
        g_nsubs--;
        subs_save();
        return 0;
    }
    if (g_nsubs >= MAX_SUBS) { mtoast("That's the most subscriptions NewTube keeps (100)."); return 0; }
    memmove(&g_subs[1], &g_subs[0], g_nsubs * sizeof(SUB));     /* newest first */
    memset(&g_subs[0], 0, sizeof(SUB));
    copy_str(g_subs[0].id, sizeof(g_subs[0].id), id);
    copy_str(g_subs[0].title, sizeof(g_subs[0].title), title);
    for (i = 0; g_subs[0].title[i]; i++) if (g_subs[0].title[i] == '\t') g_subs[0].title[i] = ' ';
    g_nsubs++;
    subs_save();
    return 1;
}

static void copy_str(char *dst, int max, const char *src)
{
    if (!src) src = "";
    strncpy(dst, src, max - 1);
    dst[max - 1] = 0;
}

/* ---------------------------------------------------------------- script thread */
static lua_State *L;

static int l_http(lua_State *ls)
{
    const char *method = luaL_checkstring(ls, 1), *url = luaL_checkstring(ls, 2);
    const char *headers = luaL_optstring(ls, 3, NULL);
    size_t body_len = 0;
    const char *body = lua_isstring(ls, 4) ? lua_tolstring(ls, 4, &body_len) : NULL;
    int status, len;
    char *reply;
    double t0 = now_s();
    if (sbhttp_fetch(method, url, headers, body, (int)body_len, &status, &reply, &len)) {
        say("  http %s %.70s FAILED after %.0f ms", method, url, (now_s() - t0) * 1000);
        lua_pushnil(ls);
        lua_pushstring(ls, "network request failed");
        return 2;
    }
    say("  http %s %.70s -> %d, %d bytes, %.0f ms", method, url, status, len, (now_s() - t0) * 1000);
    lua_pushinteger(ls, status);
    lua_pushlstring(ls, reply, len);
    sbhttp_free(reply);
    return 2;
}
static int l_log(lua_State *ls) { say("  [lua] %s", luaL_checkstring(ls, 1)); return 0; }
/* sub_avatar(id, address): youtube.lua found a subscribed channel's picture (while loading its videos): kept */
static int l_sub_avatar(lua_State *ls)
{
    const char *id = luaL_checkstring(ls, 1), *url = luaL_checkstring(ls, 2);
    int i;
    EnterCriticalSection(&g_lock);
    i = sub_find(id);
    if (i >= 0 && strlen(url) < sizeof(g_subs[0].avatar) && !strchr(url, '\t') && strcmp(g_subs[i].avatar, url)) {
        copy_str(g_subs[i].avatar, sizeof(g_subs[0].avatar), url);
        subs_save();
    }
    LeaveCriticalSection(&g_lock);
    return 0;
}
static void run_waiting_play(void);
static int l_subs(lua_State *ls)        /* subs() -> { {id = , title = }, ... } (the local subscriptions) */
{
    int i;
    lua_newtable(ls);
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_nsubs; i++) {
        lua_newtable(ls);
        lua_pushstring(ls, g_subs[i].id);
        lua_setfield(ls, -2, "id");
        lua_pushstring(ls, g_subs[i].title);
        lua_setfield(ls, -2, "title");
        lua_rawseti(ls, -2, i + 1);
    }
    LeaveCriticalSection(&g_lock);
    return 1;
}

static const char *field(lua_State *ls, int idx, const char *name)    /* string field of the table at idx */
{
    const char *s;
    lua_getfield(ls, idx, name);
    s = lua_tostring(ls, -1);
    lua_pop(ls, 1);             /* the string stays valid: the table still holds it */
    return s ? s : "";
}

/* run yt_search(query, filter) and put the videos into section i (if the page hasn't changed) */
static void load_section(int i, int gen, const char *query, const char *filter)
{
    static VIDEO tmp[MAX_VIDEOS];
    int n = 0, k, ok = 0;
    char err[120] = "";
    lua_getglobal(L, "yt_search");
    lua_pushstring(L, query);
    lua_pushstring(L, filter);
    if (lua_pcall(L, 2, 2, 0)) {
        copy_str(err, sizeof(err), lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        if (lua_istable(L, -2)) {
            int total = (int)lua_objlen(L, -2);
            ok = 1;
            for (k = 1; k <= total && n < MAX_VIDEOS; k++) {
                VIDEO *v = &tmp[n];
                lua_rawgeti(L, -2, k);
                if (lua_istable(L, -1)) {
                    memset(v, 0, sizeof(*v));
                    copy_str(v->id, sizeof(v->id), field(L, -1, "id"));
                    copy_str(v->title, sizeof(v->title), field(L, -1, "title"));
                    copy_str(v->channel, sizeof(v->channel), field(L, -1, "channel"));
                    copy_str(v->views, sizeof(v->views), field(L, -1, "views"));
                    copy_str(v->age, sizeof(v->age), field(L, -1, "age"));
                    copy_str(v->dur, sizeof(v->dur), field(L, -1, "duration"));
                    copy_str(v->list, sizeof(v->list), field(L, -1, "list"));
                    lua_getfield(L, -1, "seconds");
                    v->seconds = (int)lua_tointeger(L, -1);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "age_s");           /* (Subscriptions: how old, for the NEW badge) */
                    if (lua_isnumber(L, -1) && g_subs_cutoff > 0 && lua_tonumber(L, -1) < 1e11)
                        v->fresh = wall_seconds() - (__int64)lua_tonumber(L, -1) > g_subs_cutoff;
                    lua_pop(L, 1);
                    if (v->id[0]) n++;
                }
                lua_pop(L, 1);
            }
        } else
            copy_str(err, sizeof(err), lua_tostring(L, -1));
        lua_pop(L, 2);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    EnterCriticalSection(&g_lock);
    if (gen == g_gen && i < g_nsec) {
        SECTION *s = &g_sec[i];
        memcpy(s->v, tmp, n * sizeof(VIDEO));
        s->n = n;
        s->state = ok ? 2 : 3;
        copy_str(s->err, sizeof(s->err), ok ? (n ? "" : "No videos found.") : err);
    }
    LeaveCriticalSection(&g_lock);
    if (!ok) say("section %d failed: %s", i, err);
}

/* rows made from the watch history (no network): "@continue" = half-watched videos, "@history" = everything.
   g_lock held. Returns how many videos went in (0 = the row has nothing to show) */
static int watch_row(SECTION *s, const char *filter)
{
    int i, cont = !strcmp(filter, "@continue");
    memset(s, 0, sizeof(*s));
    copy_str(s->title, sizeof(s->title), cont ? "Continue watching" : "Watch history");
    copy_str(s->filter, sizeof(s->filter), filter);
    for (i = 0; i < g_nwatch && s->n < MAX_VIDEOS; i++) {
        if (cont && (g_watch[i].done || g_watch[i].pos < 10)) continue;
        s->v[s->n] = g_watch[i].v;
        s->v[s->n].img = NULL;
        s->v[s->n].pix = NULL;
        s->v[s->n].thumb = 0;
        s->n++;
    }
    s->state = 2;
    return s->n;
}

static void row_free_images(SECTION *s)
{
    int k;
    for (k = 0; k < s->n; k++) {
        if (s->v[k].img) { p360_image_free(s->v[k].img); s->v[k].img = NULL; }
        if (s->v[k].pix) { sbimage_free(s->v[k].pix); s->v[k].pix = NULL; }
    }
}

/* the Account page = your library: Continue watching, Watch later + your playlists (those with videos), Watch
   history. All local (no network). Replaces g_sec (its pictures freed). g_lock held */
static void account_rows(void)
{
    int i, k, n = 0;
    for (i = 0; i < g_nsec; i++) row_free_images(&g_sec[i]);
    if (watch_row(&g_sec[n], "@continue")) n++;
    for (i = 0; i < g_nlists && n < MAX_SECTIONS - 1; i++) {
        SECTION *s = &g_sec[n];
        if (!g_lists[i].n) continue;
        memset(s, 0, sizeof(*s));
        copy_str(s->title, sizeof(s->title), g_lists[i].name);
        copy_str(s->filter, sizeof(s->filter), "@list");
        _snprintf(s->query, sizeof(s->query) - 1, "%d", i);
        for (k = 0; k < g_lists[i].n && k < MAX_VIDEOS; k++) {
            s->v[k] = g_lists[i].v[k];
            s->v[k].img = NULL; s->v[k].pix = NULL; s->v[k].thumb = 0;
        }
        s->n = k;
        s->state = 2;
        n++;
    }
    if (watch_row(&g_sec[n], "@history")) n++;
    g_nsec = n;
    g_message[0] = 0;
    if (!n) copy_str(g_message, sizeof(g_message), "Your library: videos you watch, Watch later and your "
                     "playlists show up here (signing in to a YouTube account comes in a later version).");
    if (g_focus_row >= g_nsec) g_focus_row = g_nsec ? g_nsec - 1 : 0;
}

/* the Offline Player page: "Saving now" (downloads, with their progress drawn live on the tiles), then the saved
   videos in rows of 24. All local. Replaces g_sec (its pictures freed): call on the screen thread, g_lock held */
static void offline_rows(void)
{
    int i, k, n = 0, kb = 0, oldn = g_nsec, oldcol[MAX_SECTIONS];
    for (i = 0; i < g_nsec; i++) { row_free_images(&g_sec[i]); oldcol[i] = g_sec[i].col; }
    if (g_ndl) {
        SECTION *s = &g_sec[n++];
        memset(s, 0, sizeof(*s));
        copy_str(s->title, sizeof(s->title), "Saving now");
        copy_str(s->filter, sizeof(s->filter), "@dl");
        for (k = 0; k < g_ndl && k < MAX_VIDEOS; k++) { s->v[k] = g_dl[k].v; s->v[k].img = NULL; s->v[k].pix = NULL; s->v[k].thumb = 0; }
        s->n = k;
        s->state = 2;
    }
    for (i = 0; i < g_noff; i++) kb += g_off[i].kb;
    for (i = 0; i < g_noff && n < MAX_SECTIONS; i += MAX_VIDEOS) {
        SECTION *s = &g_sec[n++];
        memset(s, 0, sizeof(*s));
        if (i == 0) _snprintf(s->title, sizeof(s->title) - 1, "Saved on this console (%d video%s, %d MB)", g_noff,
                              g_noff == 1 ? "" : "s", (kb + 512) / 1024);
        else _snprintf(s->title, sizeof(s->title) - 1, "Saved on this console (%d - %d)", i + 1,
                       i + MAX_VIDEOS < g_noff ? i + MAX_VIDEOS : g_noff);
        copy_str(s->filter, sizeof(s->filter), "@off");
        for (k = 0; k < MAX_VIDEOS && i + k < g_noff; k++) { s->v[k] = g_off[i + k].v; s->v[k].img = NULL; s->v[k].pix = NULL; s->v[k].thumb = 0; }
        s->n = k;
        s->state = 2;
    }
    own_scan();                                     /* your own files in the offline folder */
    for (i = 0; i < g_nown && n < MAX_SECTIONS; i += MAX_VIDEOS) {
        SECTION *s = &g_sec[n++];
        memset(s, 0, sizeof(*s));
        if (i == 0) _snprintf(s->title, sizeof(s->title) - 1, "Your own videos (%d)", g_nown);
        else _snprintf(s->title, sizeof(s->title) - 1, "Your own videos (%d - %d)", i + 1,
                       i + MAX_VIDEOS < g_nown ? i + MAX_VIDEOS : g_nown);
        copy_str(s->filter, sizeof(s->filter), "@own");
        for (k = 0; k < MAX_VIDEOS && i + k < g_nown; k++) { s->v[k] = g_own[i + k].v; s->v[k].thumb = 4; }   /* (no picture) */
        s->n = k;
        s->state = 2;
    }
    g_nsec = n;
    g_message[0] = 0;
    if (!n) copy_str(g_message, sizeof(g_message), "Nothing here yet. Hold A on a video and press RB to save it, or "
                     "copy your own .mp4 / .mkv files into the offline folder (game:\\offline) with FTP.");
    if (g_focus_row >= g_nsec) g_focus_row = g_nsec ? g_nsec - 1 : 0;
    for (i = 0; i < g_nsec && i < oldn; i++)        /* (rebuilt while you browse it: stay where you were) */
        g_sec[i].col = oldcol[i] < g_sec[i].n ? oldcol[i] : (g_sec[i].n ? g_sec[i].n - 1 : 0);
}

/* after a video: Home's "Continue watching" row (live page or the kept copy of Home) gets the new state: rebuilt,
   added at the top, or removed when nothing is half-watched any more. g_lock held. Returns -1/0/+1 = rows removed /
   same / added (the live page moves its focus along) */
static int refresh_continue(SECTION *secs, int *n)
{
    static SECTION fresh;
    int i, has = *n > 0 && !strcmp(secs[0].filter, "@continue"), count = watch_row(&fresh, "@continue");
    for (i = 0; i < *n; i++) {              /* downloads in flight may land on the wrong row after a move: redo */
        int k;
        for (k = 0; k < secs[i].n; k++) if (secs[i].v[k].thumb == 1) secs[i].v[k].thumb = 0;
    }
    if (has) {
        row_free_images(&secs[0]);
        if (count) { secs[0] = fresh; return 0; }
        memmove(&secs[0], &secs[1], (*n - 1) * sizeof(SECTION));
        (*n)--;
        return -1;
    }
    if (!count) return 0;
    if (*n == MAX_SECTIONS) { row_free_images(&secs[*n - 1]); (*n)--; }
    memmove(&secs[1], &secs[0], *n * sizeof(SECTION));
    secs[0] = fresh;
    (*n)++;
    return 1;
}

/* the Subscriptions page's rows: "Latest from your subscriptions" (all channels mixed, newest first), then one
   row per channel (query = its id). g_lock held. 0 = no subscriptions */
static int subscription_rows(void)
{
    int i, n = 0;
    if (!g_nsubs) return 0;
    memset(&g_sec[0], 0, sizeof(SECTION));
    copy_str(g_sec[0].title, sizeof(g_sec[0].title), "Latest from your subscriptions");
    copy_str(g_sec[0].filter, sizeof(g_sec[0].filter), "@latest");
    n = 1;
    for (i = 0; i < g_nsubs && n < MAX_SECTIONS; i++, n++) {
        memset(&g_sec[n], 0, sizeof(SECTION));
        copy_str(g_sec[n].title, sizeof(g_sec[n].title), g_subs[i].title);
        copy_str(g_sec[n].query, sizeof(g_sec[n].query), g_subs[i].id);
        copy_str(g_sec[n].filter, sizeof(g_sec[n].filter), "@channel");
    }
    return n;
}

/* the Subscriptions page with ONE channel picked in the column: its newest, most viewed, live streams, playlists
   (youtube.lua "@ch_*" rows, query = its id). g_lock held */
static int channel_rows(const char *id)
{
    static const char *TITLE[4] = { "Latest videos", "Popular videos", "Live streams", "Playlists" };
    static const char *FILTER[4] = { "@ch_latest", "@ch_popular", "@ch_live", "@ch_lists" };
    int i;
    for (i = 0; i < 4; i++) {
        memset(&g_sec[i], 0, sizeof(SECTION));
        copy_str(g_sec[i].title, sizeof(g_sec[i].title), TITLE[i]);
        copy_str(g_sec[i].query, sizeof(g_sec[i].query), id);
        copy_str(g_sec[i].filter, sizeof(g_sec[i].filter), FILTER[i]);
    }
    return 4;
}

/* HOME: "Because you watched ..." rows = the videos YouTube lists next to the last videos you played (filter
   "@related", query = that video's id), placed after the first home-feed row. Personal recommendations from the
   watch history on the console: no account. g_lock held */
#define BECAUSE_ROWS 2
static void insert_because_rows(void)
{
    int i, k, n = 0, at = 0;
    const char *picked[BECAUSE_ROWS];
    for (i = 0; i < g_nsec; i++) if (!strcmp(g_sec[i].filter, "@home")) { at = i + 1; break; }   /* after Recommended */
    if (!at) at = g_nsec && !strcmp(g_sec[0].filter, "@continue") ? 1 : 0;
    for (i = 0; i < g_nwatch && n < BECAUSE_ROWS && g_nsec < MAX_SECTIONS; i++) {
        VIDEO *v = &g_watch[i].v;
        int dup = 0;
        if (strlen(v->id) != 11 || own_id_shape(v->id) || !strcmp(v->dur, "LIVE")) continue;
        for (k = 0; k < n; k++) if (!strcmp(picked[k], v->id)) dup = 1;
        if (dup) continue;
        picked[n] = v->id;
        memmove(&g_sec[at + 1], &g_sec[at], (g_nsec - at) * sizeof(SECTION));
        memset(&g_sec[at], 0, sizeof(SECTION));
        _snprintf(g_sec[at].title, sizeof(g_sec[at].title) - 1, "Because you watched %.56s%s", v->title,
                  strlen(v->title) > 56 ? "..." : "");
        copy_str(g_sec[at].query, sizeof(g_sec[at].query), v->id);
        copy_str(g_sec[at].filter, sizeof(g_sec[at].filter), "@related");
        g_nsec++;
        at++;
        n++;
    }
}

static void job_page(JOB *j)
{
    int i, n = 0;
    if (!strcmp(j->text, "Subscriptions")) {
        char vid[40];
        EnterCriticalSection(&g_lock);
        copy_str(vid, sizeof(vid), sub_find(g_sub_vid) >= 0 ? g_sub_vid : "");
        LeaveCriticalSection(&g_lock);
        if (!vid[0]) subs_visit();          /* (what counts as NEW: since the previous visit) */
        EnterCriticalSection(&g_lock);
        if (j->gen == g_gen) {
            g_nsec = n = vid[0] ? channel_rows(vid) : subscription_rows();
            if (!n) copy_str(g_message, sizeof(g_message), "You haven't subscribed to any channels yet. Open a "
                             "channel (the channel picture in the video player) and press Subscribe: no YouTube "
                             "account needed, NewTube keeps the list on your console.");
        }
        LeaveCriticalSection(&g_lock);
        goto load_rows;
    }
    if (!strcmp(j->text, "Account")) {     /* (no sign-in yet) your history: continue watching + everything */
        EnterCriticalSection(&g_lock);
        if (j->gen == g_gen) { g_nsec = 0; account_rows(); }
        LeaveCriticalSection(&g_lock);
        return;
    }
    lua_getglobal(L, "yt_page");
    lua_pushstring(L, j->text);
    if (lua_pcall(L, 1, 1, 0) || !lua_istable(L, -1)) {
        say("yt_page failed: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    EnterCriticalSection(&g_lock);
    if (j->gen == g_gen) {
        n = (int)lua_objlen(L, -1);
        if (n > MAX_SECTIONS) n = MAX_SECTIONS;
        for (i = 0; i < n; i++) {
            SECTION *s = &g_sec[i];
            lua_rawgeti(L, -1, i + 1);
            memset(s, 0, sizeof(*s));
            copy_str(s->title, sizeof(s->title), field(L, -1, "title"));
            copy_str(s->query, sizeof(s->query), field(L, -1, "query"));
            copy_str(s->filter, sizeof(s->filter), field(L, -1, "filter"));
            lua_pop(L, 1);
        }
        g_nsec = n;
        if (!strcmp(j->text, "Home")) {     /* "Continue watching" on top (from the history: already filled) */
            refresh_continue(g_sec, &g_nsec);
            insert_because_rows();          /* + "Because you watched ..." */
            n = g_nsec;
        }
    }
    LeaveCriticalSection(&g_lock);
    lua_pop(L, 1);
load_rows:
    for (i = 0; i < n; i++) {       /* one search per section, top to bottom; stop if the page changed */
        char q[120], f[32];
        run_waiting_play();         /* a video asked for meanwhile goes first */
        EnterCriticalSection(&g_lock);
        if (j->gen != g_gen) { LeaveCriticalSection(&g_lock); return; }
        if (g_sec[i].state == 2) { LeaveCriticalSection(&g_lock); continue; }   /* (filled already: history) */
        g_sec[i].state = 1;
        copy_str(q, sizeof(q), g_sec[i].query);
        copy_str(f, sizeof(f), g_sec[i].filter);
        LeaveCriticalSection(&g_lock);
        load_section(i, j->gen, q, f);
    }
}

/* SEARCH FILTERS (LB / RB on the results page): YouTube's search "params" = a tiny protobuf message in base64:
   field 1 = sort (2 upload date, 3 view count), field 2 = filters { 1 upload date: 2 today 3 week 4 month 5 year;
   2 type: 1 video; 3 length: 1 under 4 minutes, 2 over 20 minutes } */
typedef struct { const char *name, *param; } SFILTER;
static const SFILTER SFILTERS[] = {
    { "Everything", "" }, { "Uploaded today", "EgQIAhAB" }, { "This week", "EgQIAxAB" }, { "This month", "EgQIBBAB" },
    { "This year", "EgQIBRAB" }, { "Under 4 minutes", "EgQQARgB" }, { "Over 20 minutes", "EgQQARgC" },
    { "Newest first", "CAISAhAB" }, { "Most viewed", "CAMSAhAB" },
};
#define N_SFILTERS ((int)(sizeof(SFILTERS) / sizeof(SFILTERS[0])))

static void job_search(JOB *j)
{
    int gen = j->gen;
    char p0[16];
    EnterCriticalSection(&g_lock);
    copy_str(p0, sizeof(p0), SFILTERS[g_sfilter].param);
    if (gen == g_gen) {
        memset(g_sec, 0, sizeof(SECTION) * 2);
        if (g_sfilter) _snprintf(g_sec[0].title, sizeof(g_sec[0].title) - 1, "Results for \"%.40s\" (%s)", j->text,
                                 SFILTERS[g_sfilter].name);
        else _snprintf(g_sec[0].title, sizeof(g_sec[0].title) - 1, "Results for \"%s\"", j->text);
        copy_str(g_sec[0].query, sizeof(g_sec[0].query), j->text);
        copy_str(g_sec[0].filter, sizeof(g_sec[0].filter), p0);
        _snprintf(g_sec[1].title, sizeof(g_sec[1].title) - 1, "Most viewed: \"%s\"", j->text);
        copy_str(g_sec[1].query, sizeof(g_sec[1].query), j->text);
        copy_str(g_sec[1].filter, sizeof(g_sec[1].filter), "CAM=");      /* sort by view count */
        memset(&g_sec[2], 0, sizeof(SECTION));
        _snprintf(g_sec[2].title, sizeof(g_sec[2].title) - 1, "Playlists: \"%s\"", j->text);
        copy_str(g_sec[2].query, sizeof(g_sec[2].query), j->text);
        copy_str(g_sec[2].filter, sizeof(g_sec[2].filter), "EgIQAw==");  /* playlists only */
        g_sec[0].state = 1;
        g_nsec = 3;
    }
    LeaveCriticalSection(&g_lock);
    load_section(0, gen, j->text, p0);
    run_waiting_play();
    EnterCriticalSection(&g_lock);
    if (gen == g_gen) g_sec[1].state = 1;
    LeaveCriticalSection(&g_lock);
    load_section(1, gen, j->text, "CAM=");
    run_waiting_play();
    EnterCriticalSection(&g_lock);
    if (gen == g_gen) g_sec[2].state = 1;
    LeaveCriticalSection(&g_lock);
    load_section(2, gen, j->text, "EgIQAw==");
}

static double fnum(lua_State *ls, int idx, const char *name)        /* number field of the table at idx (0 = none) */
{
    double d;
    lua_getfield(ls, idx, name);
    d = lua_tonumber(ls, -1);
    lua_pop(ls, 1);
    return d;
}

static int copy_url(char *dst, int max, const char *src)    /* a cut-off link is useless: 1 = copied whole */
{
    if (!src || !src[0] || (int)strlen(src) >= max) { dst[0] = 0; return 0; }
    strcpy(dst, src);
    return 1;
}

/* the "More Videos" list + details of video id -> into g_pl (if seq is still the video playing) */
/* CHAPTERS: YouTube makes a video's chapters from timestamp lines in its description ("0:00 Intro", "12:34 - Part
   two", "Intro (0:00)"). Same rule here: the first at 0:00, at least 3, in order. -> g_pl.nchap / chap_t / chap_title.
   g_lock held */
static int parse_stamp(const char *s, int *len)    /* "1:02:15" / "12:34" at s -> seconds (-1 = none) */
{
    int parts[3], np = 0, i = 0;
    while (np < 3) {
        int v = 0, d = 0;
        while (s[i] >= '0' && s[i] <= '9' && d < 3) { v = v * 10 + (s[i] - '0'); i++; d++; }
        if (!d || (np && d != 2)) break;
        parts[np++] = v;
        if (s[i] != ':' || !(s[i + 1] >= '0' && s[i + 1] <= '9')) break;
        i++;
    }
    if (np < 2) return -1;
    *len = i;
    return np == 2 ? parts[0] * 60 + parts[1] : parts[0] * 3600 + parts[1] * 60 + parts[2];
}

static void chapters_parse(void)
{
    const char *p = g_pl.desc;
    int n = 0;
    g_pl.nchap = 0;
    while (*p && n < 64) {
        const char *end = p + strcspn(p, "\n"), *q = p, *title = NULL;
        int t = -1, len = 0, tl = 0;
        while (q < end && (*q == ' ' || *q == '-' || *q == '(' || *q == '[' || (unsigned char)*q >= 0x80)) q++;
        if (q < end && *q >= '0' && *q <= '9' && (t = parse_stamp(q, &len)) >= 0) {        /* "0:00 Intro" */
            title = q + len;
            while (title < end && (strchr(" )]-:|.", *title) || (unsigned char)*title >= 0x80)) title++;
            tl = (int)(end - title);
        } else {                                                                            /* "Intro 0:00" */
            const char *r = end;
            while (r > p && (r[-1] == ' ' || r[-1] == '\r' || r[-1] == ')' || r[-1] == ']')) r--;
            while (r > p && ((r[-1] >= '0' && r[-1] <= '9') || r[-1] == ':')) r--;
            if (r < end && *r >= '0' && *r <= '9' && (t = parse_stamp(r, &len)) >= 0) {
                title = p;
                while (title < r && (strchr(" -*>", *title) || (unsigned char)*title >= 0x80)) title++;    /* bullets */
                tl = (int)(r - title);
                while (tl > 0 && strchr(" -:|([", title[tl - 1])) tl--;
            }
        }
        while (tl > 0 && (title[tl - 1] == '\r' || title[tl - 1] == ' ')) tl--;
        if (t >= 0 && tl > 0 && (n == 0 ? t == 0 : t > g_pl.chap_t[n - 1])) {
            g_pl.chap_t[n] = (float)t;
            if (tl >= (int)sizeof(g_pl.chap_title[0])) tl = sizeof(g_pl.chap_title[0]) - 1;
            memcpy(g_pl.chap_title[n], title, tl);
            g_pl.chap_title[n][tl] = 0;
            n++;
        } else if (t >= 0 && n == 0) { /* (a stamp before the 0:00 one: not a chapter list yet) */ }
        p = *end ? end + 1 : end;
    }
    g_pl.nchap = n >= 3 ? n : 0;
}

static int chapter_at(double t)                 /* the chapter playing at t (-1 = no chapters) */
{
    int k = g_pl.nchap - 1;
    while (k > 0 && g_pl.chap_t[k] > t) k--;
    return g_pl.nchap ? k : -1;
}

static void load_next(const char *id, int seq)
{
    static VIDEO rel[MAX_REL];
    static char likes[32], subs[48], published[64], avatar[400], desc[2000], age[48], views[48], chid[40];
    static char cmtok[1200], cmcount[24];
    int n = 0, k, total, t, ok = 0;
    lua_getglobal(L, "yt_next");
    lua_pushstring(L, id);
    if (lua_pcall(L, 1, 2, 0)) {
        say("yt_next failed: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    t = lua_gettop(L) - 1;
    if (lua_istable(L, t)) {
        ok = 1;
        copy_str(likes, sizeof(likes), field(L, t, "likes"));
        copy_str(subs, sizeof(subs), field(L, t, "subs"));
        copy_str(published, sizeof(published), field(L, t, "published"));
        copy_url(avatar, sizeof(avatar), field(L, t, "avatar"));
        copy_str(desc, sizeof(desc), field(L, t, "desc"));
        copy_str(age, sizeof(age), field(L, t, "age"));
        copy_str(views, sizeof(views), field(L, t, "views"));
        copy_str(chid, sizeof(chid), field(L, t, "channel_id"));
        copy_str(cmtok, sizeof(cmtok), field(L, t, "comments_token"));
        copy_str(cmcount, sizeof(cmcount), field(L, t, "comment_count"));
        lua_getfield(L, t, "related");
        if (lua_istable(L, -1)) {
            total = (int)lua_objlen(L, -1);
            for (k = 1; k <= total && n < MAX_REL; k++) {
                VIDEO *v = &rel[n];
                lua_rawgeti(L, -1, k);
                memset(v, 0, sizeof(*v));
                copy_str(v->id, sizeof(v->id), field(L, -1, "id"));
                copy_str(v->title, sizeof(v->title), field(L, -1, "title"));
                copy_str(v->channel, sizeof(v->channel), field(L, -1, "channel"));
                copy_str(v->views, sizeof(v->views), field(L, -1, "views"));
                copy_str(v->dur, sizeof(v->dur), field(L, -1, "duration"));
                v->seconds = (int)fnum(L, -1, "seconds");
                if (v->id[0]) n++;
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    } else
        say("yt_next failed: %s", lua_tostring(L, -1));
    lua_pop(L, 2);
    lua_gc(L, LUA_GCCOLLECT, 0);
    if (!ok) return;
    EnterCriticalSection(&g_lock);
    if (seq == g_pl.seq) {
        copy_str(g_pl.likes, sizeof(g_pl.likes), likes);
        copy_str(g_pl.subs, sizeof(g_pl.subs), subs);
        copy_str(g_pl.published, sizeof(g_pl.published), published);
        strcpy(g_pl.avatar, avatar);
        strcpy(g_pl.channel_id, chid);
        copy_str(g_pl.desc, sizeof(g_pl.desc), desc);
        copy_str(g_pl.cm_token, sizeof(g_pl.cm_token), cmtok);
        copy_str(g_pl.cm_count, sizeof(g_pl.cm_count), cmcount);
        chapters_parse();
        if (age[0]) copy_str(g_pl.v.age, sizeof(g_pl.v.age), age);
        if (views[0]) copy_str(g_pl.v.views, sizeof(g_pl.v.views), views);
        memcpy(g_pl.rel, rel, n * sizeof(VIDEO));
        g_pl.nrel = n;
        g_pl.have_next = 1;
    }
    LeaveCriticalSection(&g_lock);
    {                                       /* + likes / dislikes for the description panel's ratio bar */
        double lk = -1, dk = -1;
        lua_getglobal(L, "yt_votes");
        lua_pushstring(L, id);
        if (lua_pcall(L, 1, 2, 0) == 0) {
            if (lua_isnumber(L, -2)) { lk = lua_tonumber(L, -2); dk = lua_tonumber(L, -1); }
            lua_pop(L, 2);
        } else lua_pop(L, 1);
        EnterCriticalSection(&g_lock);
        if (seq == g_pl.seq && lk >= 0) { g_pl.likes_n = lk; g_pl.dislikes_n = dk; g_pl.have_votes = 1; }
        LeaveCriticalSection(&g_lock);
    }
}

static void job_play(JOB *j)
{
    static char aurl[URL_MAX], qurl[MAX_Q][URL_MAX], sbpre[1400], sbpost[400], capn[MAX_CAPT][64], capu[MAX_CAPT][640];
    char title[220] = "", views[48] = "";
    int qh[MAX_Q], nq = 0, pick = 0, ok = 0, r, k, t, ncap = 0;
    int sbc = 0, sbn = 0, sbw = 0, sbh = 0, sbcols = 0, sbrows = 0;
    double sbi = 0;
    aurl[0] = sbpre[0] = sbpost[0] = 0;
    EnterCriticalSection(&g_lock);              /* SAVED on the console: play the files (no network needed) */
    k = offline_find(j->text);
    if (k >= 0) {
        offline_path(qurl[0], URL_MAX, j->text, "video.mp4");
        offline_path(aurl, URL_MAX, j->text, "audio.m4a");
        qh[0] = g_off[k].height;
        copy_str(title, sizeof(title), g_off[k].v.title);
        copy_str(views, sizeof(views), g_off[k].v.views);
        nq = 1;
        ok = 1;
    }
    g_pl_offline = k >= 0;
    if (own_id_shape(j->text)) {                /* your own file in the offline folder (one file: picture + sound) */
        if (g_nown == 0) own_scan();            /* (started from the history before the page was ever opened) */
        k = own_find(j->text);
        if (k >= 0) {
            copy_str(qurl[0], URL_MAX, g_own[k].path);
            qh[0] = 0;
            copy_str(title, sizeof(title), g_own[k].v.title);
            nq = 1;
            ok = 1;
        } else
            copy_str(g_play_msg, sizeof(g_play_msg), "That file isn't in the offline folder any more.");
        g_pl_offline = 1;
        LeaveCriticalSection(&g_lock);
        if (!ok) goto have_links;
        say("play: own file %s", qurl[0]);
        goto have_links;
    }
    LeaveCriticalSection(&g_lock);
    if (ok) {
        say("play: from the hard drive (%dp)", qh[0]);
        goto have_links;
    }
    lua_pushinteger(L, QUALITY_CHOICES[g_set.quality]);     /* Settings: the quality a video starts in */
    lua_setglobal(L, "MAX_HEIGHT");
    lua_getglobal(L, "yt_streams");
    lua_pushstring(L, j->text);
    if (lua_pcall(L, 1, 2, 0)) {
        copy_str(g_play_msg, sizeof(g_play_msg), lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        t = lua_gettop(L) - 1;
        if (lua_istable(L, t)) {
            copy_url(aurl, sizeof(aurl), field(L, t, "audio"));
            copy_str(title, sizeof(title), field(L, t, "title"));
            copy_str(views, sizeof(views), field(L, t, "views"));
            lua_getfield(L, t, "quality");
            if (lua_istable(L, -1)) {
                int total = (int)lua_objlen(L, -1);
                for (k = 1; k <= total && nq < MAX_Q; k++) {
                    lua_rawgeti(L, -1, k);
                    qh[nq] = (int)fnum(L, -1, "height");
                    if (copy_url(qurl[nq], URL_MAX, field(L, -1, "url"))) nq++;
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            ncap = 0;
            lua_getfield(L, t, "captions");
            if (lua_istable(L, -1)) {
                int total = (int)lua_objlen(L, -1);
                for (k = 1; k <= total && ncap < MAX_CAPT; k++) {
                    lua_rawgeti(L, -1, k);
                    copy_str(capn[ncap], sizeof(capn[0]), field(L, -1, "name"));
                    if (copy_url(capu[ncap], sizeof(capu[0]), field(L, -1, "url"))) ncap++;
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            pick = (int)fnum(L, t, "pick") - 1;
            if (pick < 0 || pick >= nq) pick = nq - 1;
            ok = nq > 0;
            sbc = (int)fnum(L, t, "sb_count");
            if (sbc > 0 && copy_url(sbpre, sizeof(sbpre), field(L, t, "sb_pre"))) {
                copy_url(sbpost, sizeof(sbpost), field(L, t, "sb_post"));
                sbn = (int)fnum(L, t, "sb_numbered");
                sbw = (int)fnum(L, t, "sb_w"); sbh = (int)fnum(L, t, "sb_h");
                sbcols = (int)fnum(L, t, "sb_cols"); sbrows = (int)fnum(L, t, "sb_rows");
                sbi = fnum(L, t, "sb_interval");
            } else
                sbc = 0;
            if (!ok) copy_str(g_play_msg, sizeof(g_play_msg), "No video stream this player can use.");
        } else
            copy_str(g_play_msg, sizeof(g_play_msg), lua_tostring(L, -1) ? lua_tostring(L, -1) : "no links");
        lua_pop(L, 2);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
have_links:
    EnterCriticalSection(&g_lock);
    if (j->gen != g_play_seq) ok = 0;   /* cancelled already: don't even open the streams */
    if (ok && j->gen == g_pl.seq) {     /* hand the details to the player screen */
        if (title[0]) copy_str(g_pl.v.title, sizeof(g_pl.v.title), title);
        if (views[0]) copy_str(g_pl.v.views, sizeof(g_pl.v.views), views);
        strcpy(g_pl.audio, aurl);
        g_pl.nq = nq;
        g_pl.q = pick;
        for (k = 0; k < nq; k++) { g_pl.q_height[k] = qh[k]; strcpy(g_pl.q_url[k], qurl[k]); }
        g_pl.ncap = ncap;
        for (k = 0; k < ncap; k++) { strcpy(g_pl.cap_name[k], capn[k]); strcpy(g_pl.cap_url[k], capu[k]); }
        g_pl.sb = sbc > 0 && sbw > 0 && sbh > 0 && sbcols > 0 && sbrows > 0 && sbi > 0;
        if (g_pl.sb) {
            strcpy(g_pl.sb_pre, sbpre); strcpy(g_pl.sb_post, sbpost);
            g_pl.sb_numbered = sbn; g_pl.sb_w = sbw; g_pl.sb_h = sbh; g_pl.sb_count = sbc;
            g_pl.sb_cols = sbcols; g_pl.sb_rows = sbrows; g_pl.sb_interval = sbi;
        }
    }
    LeaveCriticalSection(&g_lock);
    if (ok) {
        r = sbplayer_open(qurl[pick], aurl[0] ? aurl : NULL, lib_log, 4, &g_play_info);
        if (r < 0) {
            sbplayer_close();
            if (r == -10) copy_str(g_play_msg, sizeof(g_play_msg), "Its picture isn't H.264 (for example H.265 / "
                                   "HEVC): this player only decodes H.264. Re-encode it on the PC (e.g. HandBrake, H.264).");
            else if (r == -12) copy_str(g_play_msg, sizeof(g_play_msg), "It's a 10-bit video: this player only shows "
                                        "normal 8-bit H.264. Re-encode it on the PC (HandBrake, H.264, 8-bit).");
            else _snprintf(g_play_msg, sizeof(g_play_msg) - 1, "The video couldn't be opened (%d).", r);
            ok = 0;
        }
    }
    EnterCriticalSection(&g_lock);
    if (j->gen != g_play_seq || g_play_state != PS_LOADING) {   /* B pressed while loading: never mind */
        if (ok) sbplayer_close();
        ok = 0;
        say("play: finished after it was cancelled (dropped)");
    } else
        g_play_state = ok ? PS_READY : PS_FAILED;
    LeaveCriticalSection(&g_lock);
    if (ok && !own_id_shape(j->text)) load_next(j->text, j->gen);   /* while it plays: details + "More Videos" */
}

/* a channel page: header -> g_chan, its shelves -> g_sec (if the page wasn't left meanwhile) */
static void job_channel(JOB *j)
{
    static SECTION secs[MAX_SECTIONS];
    static CHANNEL c;
    int ns = 0, ok = 0, t, k, i;
    char err[160] = "";
    memset(&c, 0, sizeof(c));
    lua_getglobal(L, "yt_channel");
    lua_pushstring(L, j->text);
    if (lua_pcall(L, 1, 2, 0)) {
        copy_str(err, sizeof(err), lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        t = lua_gettop(L) - 1;
        if (lua_istable(L, t)) {
            ok = 1;
            copy_str(c.title, sizeof(c.title), field(L, t, "title"));
            copy_str(c.handle, sizeof(c.handle), field(L, t, "handle"));
            copy_str(c.subs, sizeof(c.subs), field(L, t, "subs"));
            copy_str(c.views, sizeof(c.views), field(L, t, "views"));
            copy_url(c.avatar, sizeof(c.avatar), field(L, t, "avatar"));
            copy_url(c.banner, sizeof(c.banner), field(L, t, "banner"));
            lua_getfield(L, t, "sections");
            if (lua_istable(L, -1)) {
                int total = (int)lua_objlen(L, -1);
                for (i = 1; i <= total && ns < MAX_SECTIONS; i++) {
                    SECTION *s = &secs[ns];
                    memset(s, 0, sizeof(*s));
                    lua_rawgeti(L, -1, i);
                    copy_str(s->title, sizeof(s->title), field(L, -1, "title"));
                    lua_getfield(L, -1, "videos");
                    if (lua_istable(L, -1)) {
                        int nv = (int)lua_objlen(L, -1);
                        for (k = 1; k <= nv && s->n < MAX_VIDEOS; k++) {
                            VIDEO *v = &s->v[s->n];
                            lua_rawgeti(L, -1, k);
                            copy_str(v->id, sizeof(v->id), field(L, -1, "id"));
                            copy_str(v->title, sizeof(v->title), field(L, -1, "title"));
                            copy_str(v->channel, sizeof(v->channel), field(L, -1, "channel"));
                            copy_str(v->views, sizeof(v->views), field(L, -1, "views"));
                            copy_str(v->age, sizeof(v->age), field(L, -1, "age"));
                            copy_str(v->dur, sizeof(v->dur), field(L, -1, "duration"));
                            copy_str(v->list, sizeof(v->list), field(L, -1, "list"));
                            v->seconds = (int)fnum(L, -1, "seconds");
                            if (v->id[0]) s->n++;
                            lua_pop(L, 1);
                        }
                    }
                    lua_pop(L, 1);
                    s->state = 2;
                    if (s->n) ns++;
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        } else
            copy_str(err, sizeof(err), lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
        lua_pop(L, 2);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    EnterCriticalSection(&g_lock);
    if (j->gen == g_gen && g_side_page == CHAN_PAGE) {
        c.gen = g_chan.gen;
        copy_str(c.id, sizeof(c.id), g_chan.id);
        if (!c.title[0]) copy_str(c.title, sizeof(c.title), g_chan.title);
        c.state = ok ? 2 : 3;
        copy_str(c.err, sizeof(c.err), err);
        g_chan = c;
        memcpy(g_sec, secs, ns * sizeof(SECTION));
        g_nsec = ns;
    }
    LeaveCriticalSection(&g_lock);
    if (!ok) say("channel failed: %s", err);
}

/* a YouTube playlist's page: yt_playlist(id) -> rows of 24 videos in g_sec (if that page is still the one shown) */
static void job_playlist(JOB *j)
{
    static SECTION secs[MAX_SECTIONS];
    static PLAYLISTPAGE p;
    int ns = 0, ok = 0, t, k, total = 0;
    char err[160] = "";
    memset(&p, 0, sizeof(p));
    memset(secs, 0, sizeof(secs));
    lua_getglobal(L, "yt_playlist");
    lua_pushstring(L, j->text);
    if (lua_pcall(L, 1, 2, 0)) {
        copy_str(err, sizeof(err), lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        t = lua_gettop(L) - 1;
        if (lua_istable(L, t)) {
            ok = 1;
            copy_str(p.title, sizeof(p.title), field(L, t, "title"));
            copy_str(p.channel, sizeof(p.channel), field(L, t, "channel"));
            copy_str(p.count, sizeof(p.count), field(L, t, "count"));
            lua_getfield(L, t, "videos");
            if (lua_istable(L, -1)) {
                int nv = (int)lua_objlen(L, -1);
                for (k = 1; k <= nv && ns < MAX_SECTIONS; k++) {
                    SECTION *s = &secs[ns];
                    VIDEO *v = &s->v[s->n];
                    lua_rawgeti(L, -1, k);
                    copy_str(v->id, sizeof(v->id), field(L, -1, "id"));
                    copy_str(v->title, sizeof(v->title), field(L, -1, "title"));
                    copy_str(v->channel, sizeof(v->channel), field(L, -1, "channel"));
                    copy_str(v->views, sizeof(v->views), field(L, -1, "views"));
                    copy_str(v->age, sizeof(v->age), field(L, -1, "age"));
                    copy_str(v->dur, sizeof(v->dur), field(L, -1, "duration"));
                    v->seconds = (int)fnum(L, -1, "seconds");
                    lua_pop(L, 1);
                    if (!v->id[0]) continue;
                    s->n++;
                    total++;
                    if (s->n == MAX_VIDEOS) ns++;
                }
                if (ns < MAX_SECTIONS && secs[ns].n) ns++;
            }
            lua_pop(L, 1);
        } else
            copy_str(err, sizeof(err), lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
        lua_pop(L, 2);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    for (k = 0; k < ns; k++) {          /* row titles: the playlist's name (+ which videos when it needs more rows) */
        SECTION *s = &secs[k];
        int first = k * MAX_VIDEOS + 1;
        if (ns == 1) copy_str(s->title, sizeof(s->title), p.title);
        else _snprintf(s->title, sizeof(s->title) - 1, "%.50s (%d-%d)", p.title, first, first + s->n - 1);
        copy_str(s->filter, sizeof(s->filter), "@pl");
        s->state = 2;
    }
    EnterCriticalSection(&g_lock);
    if (j->gen == g_gen && g_side_page == PL_PAGE) {
        copy_str(g_plp.title, sizeof(g_plp.title), p.title);
        copy_str(g_plp.channel, sizeof(g_plp.channel), p.channel);
        copy_str(g_plp.count, sizeof(g_plp.count), p.count);
        g_plp.state = ok && ns ? 2 : 3;
        memcpy(g_sec, secs, ns * sizeof(SECTION));
        g_nsec = ns;
        if (!ok) _snprintf(g_message, sizeof(g_message) - 1, "This playlist couldn't be loaded: %.140s", err);
        else if (!ns) copy_str(g_message, sizeof(g_message), "This playlist has no videos NewTube can play.");
        if (g_plp.state == 2 && g_plp.autoplay) play_rows(0, 0);
        say("playlist %s: %d videos in %d rows", j->text, total, ns);
    }
    LeaveCriticalSection(&g_lock);
    if (!ok) say("playlist failed: %s", err);
}

/* download + read one caption track (request req) -> g_cap (if it's still the one wanted) */
static void job_captions(int req, const char *url)
{
    CAPLINE *lines = NULL;
    char *text = NULL;
    int n = 0, total, k, size = 0, used = 0, t;
    lua_getglobal(L, "yt_captions");
    lua_pushstring(L, url);
    if (lua_pcall(L, 1, 2, 0)) {
        say("captions failed: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        t = lua_gettop(L) - 1;
        if (lua_istable(L, t)) {
            total = (int)lua_objlen(L, t);
            for (k = 1; k <= total; k++) {          /* first: how much text in all */
                lua_rawgeti(L, t, k);
                size += (int)strlen(field(L, -1, "text")) + 1;
                lua_pop(L, 1);
            }
            lines = (CAPLINE *)malloc((total + 1) * sizeof(CAPLINE));
            text = (char *)malloc(size + 1);
            if (lines && text)
                for (k = 1; k <= total; k++) {
                    const char *s;
                    lua_rawgeti(L, t, k);
                    s = field(L, -1, "text");
                    lines[n].start = (float)fnum(L, -1, "start");
                    lines[n].end = lines[n].start + (float)fnum(L, -1, "dur");
                    lines[n].text = used;
                    strcpy(text + used, s);
                    used += (int)strlen(s) + 1;
                    n++;
                    lua_pop(L, 1);
                }
        } else
            say("captions failed: %s", lua_tostring(L, -1));
        lua_pop(L, 2);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    EnterCriticalSection(&g_lock);
    if (req == g_cap_req && g_cap_state == CS_LOADING) {
        caption_free();
        if (lines && text && n) { g_cap = lines; g_cap_text = text; g_cap_n = n; g_cap_state = CS_READY; lines = NULL; text = NULL; }
        else g_cap_state = CS_FAILED;
    }
    LeaveCriticalSection(&g_lock);
    free(lines);
    free(text);
}

/* suggestions for what's typed on the search page -> g_sug (if it's still the newest request) */
static void job_suggest(int req, const char *q)
{
    char sug[8][Q_MAX];
    int n = 0, k, t;
    lua_getglobal(L, "yt_suggest");
    lua_pushstring(L, q);
    if (lua_pcall(L, 1, 2, 0)) { say("suggest failed: %s", lua_tostring(L, -1)); lua_pop(L, 1); return; }
    t = lua_gettop(L) - 1;
    if (lua_istable(L, t)) {
        int total = (int)lua_objlen(L, t);
        for (k = 1; k <= total && n < 8; k++) {
            lua_rawgeti(L, t, k);
            if (lua_isstring(L, -1)) copy_str(sug[n++], Q_MAX, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 2);
    EnterCriticalSection(&g_lock);
    if (req == g_sug_req) {
        memcpy(g_sug, sug, sizeof(sug));
        g_nsug = n;
        copy_str(g_sug_for, Q_MAX, q);
    }
    LeaveCriticalSection(&g_lock);
}

/* a live stream's fresh addresses (yt_streams again) -> g_pl.q_url[] for the same heights */
static void job_live(int seq, const char *id)
{
    int t, k, i, n = 0;
    DWORD t0 = GetTickCount();
    lua_getglobal(L, "yt_live_refresh");        /* (quick: only looks for the live address, see youtube.lua) */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); lua_getglobal(L, "yt_streams"); }   /* (an older script) */
    lua_pushstring(L, id);
    if (lua_pcall(L, 1, 2, 0)) { say("live refresh failed: %s", lua_tostring(L, -1)); lua_pop(L, 1); return; }
    t = lua_gettop(L) - 1;
    if (lua_istable(L, t)) {
        lua_getfield(L, t, "quality");
        if (lua_istable(L, -1)) {
            int total = (int)lua_objlen(L, -1);
            for (k = 1; k <= total; k++) {
                int h;
                const char *u;
                lua_rawgeti(L, -1, k);
                h = (int)fnum(L, -1, "height");
                u = field(L, -1, "url");
                EnterCriticalSection(&g_lock);
                if (seq == g_play_seq && u && !strncmp(u, "hls:", 4) && (int)strlen(u) < URL_MAX)
                    for (i = 0; i < g_pl.nq; i++)
                        if (g_pl.q_height[i] == h) { strcpy(g_pl.q_url[i], u); n++; }
                LeaveCriticalSection(&g_lock);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    } else
        say("live refresh: %s", lua_tostring(L, t + 1) ? lua_tostring(L, t + 1) : "no answer");
    lua_pop(L, 2);
    /* (no full memory clean-up here: Lua's normal step-by-step one is enough, and a full one every 20 s took
       processor time from the video decoder) */
    say("live refresh: %d address(es) in %u ms", n, (unsigned)(GetTickCount() - t0));
    EnterCriticalSection(&g_lock);
    if (n && seq == g_play_seq) g_live_fresh = 1;
    LeaveCriticalSection(&g_lock);
}

/* a video to save: its links (the quality chosen in Settings + the sound), handed to the download thread */
static void job_dl_links(const char *id)
{
    static char vurl[URL_MAX], aurl[URL_MAX];
    char err[100] = "";
    int ok = 0, k, t, height = 0;
    vurl[0] = aurl[0] = 0;
    lua_pushinteger(L, QUALITY_CHOICES[g_set.quality]);
    lua_setglobal(L, "MAX_HEIGHT");
    lua_getglobal(L, "yt_streams");
    lua_pushstring(L, id);
    if (lua_pcall(L, 1, 2, 0)) {
        copy_str(err, sizeof(err), lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        t = lua_gettop(L) - 1;
        if (lua_istable(L, t)) {
            int pick = (int)fnum(L, t, "pick");
            lua_getfield(L, t, "live");
            if (lua_toboolean(L, -1)) copy_str(err, sizeof(err), "Live streams can't be saved.");
            lua_pop(L, 1);
            lua_getfield(L, t, "quality");
            if (!err[0] && lua_istable(L, -1)) {
                if (pick < 1 || pick > (int)lua_objlen(L, -1)) pick = (int)lua_objlen(L, -1);
                lua_rawgeti(L, -1, pick);
                if (lua_istable(L, -1)) {
                    height = (int)fnum(L, -1, "height");
                    ok = copy_url(vurl, URL_MAX, field(L, -1, "url")) && strncmp(vurl, "hls:", 4);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            if (ok) ok = copy_url(aurl, URL_MAX, field(L, t, "audio")) && aurl[0];
            if (!ok && !err[0]) copy_str(err, sizeof(err), "No streams this player can save.");
        } else
            copy_str(err, sizeof(err), lua_tostring(L, t + 1) ? lua_tostring(L, t + 1) : "no links");
        lua_pop(L, 2);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    EnterCriticalSection(&g_lock);
    k = dl_find(id);
    if (k >= 0 && g_dl[k].state == DL_LINKS) {      /* (not cancelled meanwhile) */
        if (ok) {
            strcpy(g_dl[k].vurl, vurl);
            strcpy(g_dl[k].aurl, aurl);
            g_dl[k].height = height;
            g_dl[k].state = DL_READY;
            SetEvent(g_dl_event);
        } else {
            g_dl[k].state = DL_FAILED;
            copy_str(g_dl[k].err, sizeof(g_dl[k].err), err);
        }
        g_off_dirty = 1;
    }
    LeaveCriticalSection(&g_lock);
    say("save %s: %s", id, ok ? "links ready" : err);
}

/* MIX: youtube.lua yt_mix(id) -> the "Play all" queue. mode 1 = start it (play its first video), 2 = top up a
   running Mix (videos not in it yet go on the end; played ones make room), 3 = started from the player: the video
   playing is the Mix's first, so it just carries on into the Mix */
static void start_play(VIDEO *v);
static void job_mix(int mode, const char *id)
{
    static VIDEO got[MAX_QUEUE];
    int n = 0, k, i, t;
    char err[120] = "";
    lua_getglobal(L, "yt_mix");
    lua_pushstring(L, id);
    if (lua_pcall(L, 1, 2, 0)) { copy_str(err, sizeof(err), lua_tostring(L, -1)); lua_pop(L, 1); }
    else {
        t = lua_gettop(L) - 1;
        if (lua_istable(L, t)) {
            int total = (int)lua_objlen(L, t);
            for (k = 1; k <= total && n < MAX_QUEUE; k++) {
                VIDEO *v = &got[n];
                lua_rawgeti(L, t, k);
                memset(v, 0, sizeof(*v));
                copy_str(v->id, sizeof(v->id), field(L, -1, "id"));
                copy_str(v->title, sizeof(v->title), field(L, -1, "title"));
                copy_str(v->channel, sizeof(v->channel), field(L, -1, "channel"));
                copy_str(v->views, sizeof(v->views), field(L, -1, "views"));
                copy_str(v->age, sizeof(v->age), field(L, -1, "age"));
                copy_str(v->dur, sizeof(v->dur), field(L, -1, "duration"));
                v->seconds = (int)fnum(L, -1, "seconds");
                if (v->id[0]) n++;
                lua_pop(L, 1);
            }
        } else copy_str(err, sizeof(err), lua_tostring(L, t + 1) ? lua_tostring(L, t + 1) : "no Mix");
        lua_pop(L, 2);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    EnterCriticalSection(&g_lock);
    if (!n) {
        if (mode != 2) note("YouTube didn't make a Mix for this video.");
        say("mix failed: %s", err);
    } else if (mode == 2) {
        if (g_queue_mix) {
            int added = 0;
            for (i = 0; i < n; i++) {
                int dup = 0;
                for (k = 0; k < g_queue_n; k++) if (!strcmp(g_queue[k].id, got[i].id)) dup = 1;
                if (dup) continue;
                if (g_queue_n == MAX_QUEUE) {           /* full: the videos already played make room */
                    int drop = g_queue_pos;
                    if (drop <= 0) break;
                    memmove(&g_queue[0], &g_queue[drop], (g_queue_n - drop) * sizeof(VIDEO));
                    g_queue_n -= drop;
                    g_queue_pos = 0;
                }
                g_queue[g_queue_n++] = got[i];
                added++;
            }
            say("mix topped up: +%d videos (%d in the list)", added, g_queue_n);
        }
    } else {
        memcpy(g_queue, got, n * sizeof(VIDEO));
        g_queue_n = n;
        g_queue_pos = 0;
        g_queue_mix = 1;
        _snprintf(g_queue_name, sizeof(g_queue_name) - 1, "Mix - %s", got[0].title);
        g_queue_name[sizeof(g_queue_name) - 1] = 0;
        if (mode == 3) {
            char t2[80];
            _snprintf(t2, sizeof(t2) - 1, "Mix started: %d videos up next (it keeps going).", n - 1);
            t2[sizeof(t2) - 1] = 0;
            note(t2);
        } else start_play(&g_queue[0]);
        say("mix: %d videos", n);
    }
    g_mix_job = 0;
    LeaveCriticalSection(&g_lock);
}

/* the region chosen in Settings -> youtube.lua's set_region(code) (only Lua's own thread may touch Lua) */
static void apply_region(void)
{
    static int applied = -1;
    char code[8];
    int ver;
    EnterCriticalSection(&g_lock);
    ver = g_region_ver;
    copy_str(code, sizeof(code), region_code());
    LeaveCriticalSection(&g_lock);
    if (ver == applied) return;
    applied = ver;
    lua_getglobal(L, "set_region");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return; }     /* (an older script without regions) */
    lua_pushstring(L, code);
    if (lua_pcall(L, 1, 0, 0)) { say("set_region failed: %s", lua_tostring(L, -1)); lua_pop(L, 1); }
    else say("region: %s", code[0] ? code : "worldwide");
}

/* the description panel's comments: 20 more (youtube.lua yt_comments) appended to g_pl.cm */
static void job_comments(int seq, const char *token, int target)   /* target 0 = the player, 1 = the hold-A panel */
{
    static char next[1200];
    int t, k, total;
    COMMENT *arr = target ? g_md.cm : g_pl.cm;
    int *n = target ? &g_md.ncm : &g_pl.ncm, *loading = target ? &g_md.cm_loading : &g_pl.cm_loading;
    char *tok = target ? g_md.cm_token : g_pl.cm_token;
#define CM_STILL_WANTED (target ? seq == g_md.gen : seq == g_pl.seq)
    lua_getglobal(L, "yt_comments");
    lua_pushstring(L, token);
    next[0] = 0;
    if (lua_pcall(L, 1, 2, 0)) {
        say("comments failed: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        EnterCriticalSection(&g_lock);
        if (CM_STILL_WANTED) { *loading = 0; tok[0] = 0; }
        LeaveCriticalSection(&g_lock);
        return;
    }
    t = lua_gettop(L) - 1;
    EnterCriticalSection(&g_lock);
    if (CM_STILL_WANTED) {
        if (lua_istable(L, t)) {
            copy_str(next, sizeof(next), field(L, t, "next"));
            lua_getfield(L, t, "comments");
            total = lua_istable(L, -1) ? (int)lua_objlen(L, -1) : 0;
            for (k = 1; k <= total && *n < 60; k++) {
                COMMENT *c = &arr[*n];
                lua_rawgeti(L, -1, k);
                copy_str(c->author, sizeof(c->author), field(L, -1, "author"));
                copy_str(c->text, sizeof(c->text), field(L, -1, "text"));
                copy_str(c->age, sizeof(c->age), field(L, -1, "age"));
                copy_str(c->likes, sizeof(c->likes), field(L, -1, "likes"));
                copy_str(c->avatar, sizeof(c->avatar), field(L, -1, "avatar"));
                c->replies = (int)fnum(L, -1, "replies");
                (*n)++;
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        } else say("comments: %s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        copy_str(tok, 1200, next);
        *loading = 0;
    }
#undef CM_STILL_WANTED
    LeaveCriticalSection(&g_lock);
    lua_pop(L, 2);
    lua_gc(L, LUA_GCCOLLECT, 0);
}

/* the hold-A panel's video: description, channel, likes (yt_next) + dislikes (yt_votes); comments come on request */
static void job_mdetails(const char *id)
{
    static char desc[2000], avatar[400], subs[48], published[64], likes[32], tok[1200], cnt[24];
    double lk = -1, dk = -1;
    int t, ok = 0;
    lua_getglobal(L, "yt_next");
    lua_pushstring(L, id);
    if (lua_pcall(L, 1, 2, 0)) {
        say("details for %s failed: %s", id, lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        t = lua_gettop(L) - 1;
        if (lua_istable(L, t)) {
            ok = 1;
            copy_str(desc, sizeof(desc), field(L, t, "desc"));
            copy_url(avatar, sizeof(avatar), field(L, t, "avatar"));
            copy_str(subs, sizeof(subs), field(L, t, "subs"));
            copy_str(published, sizeof(published), field(L, t, "published"));
            copy_str(likes, sizeof(likes), field(L, t, "likes"));
            copy_str(tok, sizeof(tok), field(L, t, "comments_token"));
            copy_str(cnt, sizeof(cnt), field(L, t, "comment_count"));
        }
        lua_pop(L, 2);
    }
    if (ok) {
        lua_getglobal(L, "yt_votes");
        lua_pushstring(L, id);
        if (lua_pcall(L, 1, 2, 0) == 0) {
            if (lua_isnumber(L, -2)) { lk = lua_tonumber(L, -2); dk = lua_tonumber(L, -1); }
            lua_pop(L, 2);
        } else lua_pop(L, 1);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    EnterCriticalSection(&g_lock);
    if (!strcmp(g_md.id, id)) {
        copy_str(g_md.desc, sizeof(g_md.desc), ok ? desc : "The details couldn't be loaded.");
        copy_str(g_md.avatar, sizeof(g_md.avatar), ok ? avatar : "");
        copy_str(g_md.subs, sizeof(g_md.subs), ok ? subs : "");
        copy_str(g_md.published, sizeof(g_md.published), ok ? published : "");
        copy_str(g_md.likes, sizeof(g_md.likes), ok ? likes : "");
        copy_str(g_md.cm_token, sizeof(g_md.cm_token), ok ? tok : "");
        copy_str(g_md.cm_count, sizeof(g_md.cm_count), ok ? cnt : "");
        if (lk >= 0) { g_md.likes_n = lk; g_md.dislikes_n = dk; g_md.have_votes = 1; }
        g_md.have_next = 1;
    }
    LeaveCriticalSection(&g_lock);
}

/* the playlist page's chosen video: its description (yt_next: the same reply the player's details come from) */
static void job_desc(const char *id)
{
    static char d[1500];
    int t;
    d[0] = 0;
    lua_getglobal(L, "yt_next");
    lua_pushstring(L, id);
    if (lua_pcall(L, 1, 2, 0)) {
        say("description for %s failed: %s", id, lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        t = lua_gettop(L) - 1;
        if (lua_istable(L, t)) copy_str(d, sizeof(d), field(L, t, "desc"));
        lua_pop(L, 2);
    }
    {                                                   /* + likes / dislikes (Return YouTube Dislike) */
        double lk = -1, dk = -1;
        lua_getglobal(L, "yt_votes");
        lua_pushstring(L, id);
        if (lua_pcall(L, 1, 2, 0) == 0) {
            if (lua_isnumber(L, -2)) { lk = lua_tonumber(L, -2); dk = lua_tonumber(L, -1); }
            else say("votes for %s: %s", id, lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
            lua_pop(L, 2);
        } else lua_pop(L, 1);
        EnterCriticalSection(&g_lock);
        g_desc_likes = lk;
        g_desc_dislikes = dk;
        LeaveCriticalSection(&g_lock);
    }
    lua_gc(L, LUA_GCCOLLECT, 0);
    EnterCriticalSection(&g_lock);
    copy_str(g_desc_id, sizeof(g_desc_id), id);
    copy_str(g_desc_text, sizeof(g_desc_text), d[0] ? d : "No description.");
    LeaveCriticalSection(&g_lock);
}

static void run_waiting_play(void)      /* jobs that go before page loading: play a video, captions, suggestions */
{
    char desc_id[16];
    static char cmt_tok[1200];
    char md_id[16];
    int cmt, cmt_target;
    char dl_id[16] = "";
    int i;
    JOB p;
    int cap, sug, live;
    char url[640], q[Q_MAX], live_id[16], mix_id[16];
    int mix;
    apply_region();                             /* (a region changed in Settings goes to the script first) */
    EnterCriticalSection(&g_lock);
    mix = g_mix_job > 0 && g_mix_job < 10 ? g_mix_job : 0;     /* (10 + mode = being made right now) */
    if (mix) { g_mix_job = 10 + mix; copy_str(mix_id, sizeof(mix_id), g_mix_id); }
    p = g_play_job;
    g_play_job.type = JOB_NONE;
    cap = g_cap_job;
    g_cap_job = 0;
    strcpy(url, g_cap_job_url);
    sug = g_sug_job;
    g_sug_job = 0;
    strcpy(q, g_sug_job_q);
    live = g_live_job;
    g_live_job = 0;
    strcpy(live_id, g_live_job_id);
    copy_str(desc_id, sizeof(desc_id), g_desc_job);
    g_desc_job[0] = 0;
    cmt = g_cmt_job;
    g_cmt_job = 0;
    cmt_target = g_cmt_target;
    copy_str(cmt_tok, sizeof(cmt_tok), g_cmt_tok);
    copy_str(md_id, sizeof(md_id), g_md_job);
    g_md_job[0] = 0;
    for (i = 0; i < g_ndl; i++)                     /* a video to save waiting for its links (one per round) */
        if (g_dl[i].state == DL_WAIT) { g_dl[i].state = DL_LINKS; copy_str(dl_id, sizeof(dl_id), g_dl[i].v.id); break; }
    LeaveCriticalSection(&g_lock);
    if (p.type == JOB_PLAY) job_play(&p);
    if (live) job_live(live, live_id);
    if (cap) job_captions(cap, url);
    if (sug) job_suggest(sug, q);
    if (mix) job_mix(mix, mix_id);
    if (desc_id[0]) job_desc(desc_id);
    if (md_id[0]) job_mdetails(md_id);
    if (cmt) job_comments(cmt, cmt_tok, cmt_target);
    if (dl_id[0]) {
        job_dl_links(dl_id);
        SetEvent(g_job_event);                      /* (more may be waiting: another round) */
    }
}

/* SELF-UPDATE (see NEWTUBE_VERSION): script thread, right after youtube.lua loaded */
static void update_note(const char *s)
{
    EnterCriticalSection(&g_lock);
    copy_str(g_update_note, sizeof(g_update_note), s);
    LeaveCriticalSection(&g_lock);
    say("update: %s", s);
}

static void script_update(void)
{
    char base[300] = UPDATE_URL_DEFAULT, url[420], ver[64] = "", minapp[16] = "", have[48];
    char *reply = NULL, *nl;
    int status = 0, len = 0;
    FILE *f;
    if (!g_set.autoupdate) { update_note("automatic fixes are off"); return; }
    f = fopen("game:\\update_url.txt", "rb");
    if (f) {
        if (fgets(base, sizeof(base), f)) base[strcspn(base, "\r\n \t")] = 0;
        fclose(f);
    }
    if (!base[0]) { update_note("no update address set"); return; }
    if (base[strlen(base) - 1] == '/') base[strlen(base) - 1] = 0;
    _snprintf(url, sizeof(url) - 1, "%s/version.txt", base);
    url[sizeof(url) - 1] = 0;
    if (sbhttp_fetch("GET", url, "User-Agent: NewTube\r\n", NULL, 0, &status, &reply, &len) || status != 200) {
        if (reply) sbhttp_free(reply);
        update_note("couldn't check for fixes");
        return;
    }
    nl = reply;                                         /* line 1: the version, line 2 (optional): oldest NewTube */
    { int k = 0; while (*nl && *nl != '\r' && *nl != '\n' && k < (int)sizeof(ver) - 1) ver[k++] = *nl++; ver[k] = 0; }
    while (*nl == '\r' || *nl == '\n') nl++;
    { int k = 0; while (*nl && *nl != '\r' && *nl != '\n' && k < (int)sizeof(minapp) - 1) minapp[k++] = *nl++; minapp[k] = 0; }
    sbhttp_free(reply);
    reply = NULL;
    EnterCriticalSection(&g_lock);
    copy_str(have, sizeof(have), g_script_ver);
    LeaveCriticalSection(&g_lock);
    if (!ver[0] || !strcmp(ver, have)) { update_note("up to date"); return; }
    if (minapp[0] && atof(minapp) > atof(NEWTUBE_VERSION)) { update_note("a fix is out for a newer NewTube"); return; }
    _snprintf(url, sizeof(url) - 1, "%s/youtube.lua", base);
    url[sizeof(url) - 1] = 0;
    if (sbhttp_fetch("GET", url, "User-Agent: NewTube\r\n", NULL, 0, &status, &reply, &len) || status != 200 || len < 1000) {
        if (reply) sbhttp_free(reply);
        update_note("the fix couldn't be downloaded");
        return;
    }
    /* only a script that compiles, says the version it should, and runs without an error is kept */
    if (!strstr(reply, ver) || luaL_loadbuffer(L, reply, len, "youtube.lua") || lua_pcall(L, 0, 0, 0)) {
        say("update: the new script was refused: %s", lua_isstring(L, -1) ? lua_tostring(L, -1) : "(version mismatch)");
        if (lua_gettop(L) > 0) lua_settop(L, 0);
        luaL_dofile(L, "game:\\youtube.lua");           /* (the one we had, in full again) */
        lua_settop(L, 0);
        sbhttp_free(reply);
        update_note("a fix was refused (kept the old script)");
        return;
    }
    f = fopen("game:\\youtube.lua.new", "wb");
    if (f) {
        int ok = fwrite(reply, 1, len, f) == (size_t)len;
        fclose(f);
        if (ok) {
            DeleteFileA("game:\\youtube.lua.bak");
            MoveFileA("game:\\youtube.lua", "game:\\youtube.lua.bak");
            if (!MoveFileA("game:\\youtube.lua.new", "game:\\youtube.lua"))
                MoveFileA("game:\\youtube.lua.bak", "game:\\youtube.lua");     /* (put the old one back) */
        }
    }
    sbhttp_free(reply);
    lua_getglobal(L, "SCRIPT_VERSION");
    EnterCriticalSection(&g_lock);
    copy_str(g_script_ver, sizeof(g_script_ver), lua_tostring(L, -1) ? lua_tostring(L, -1) : ver);
    g_region_ver++;                                     /* (the new script hasn't heard the region yet) */
    LeaveCriticalSection(&g_lock);
    lua_pop(L, 1);
    apply_region();
    {
        char msg[120];
        _snprintf(msg, sizeof(msg) - 1, "updated to %s", ver);
        msg[sizeof(msg) - 1] = 0;
        update_note(msg);
    }
    mtoast("NewTube got the latest YouTube fixes.");
}

static DWORD WINAPI script_thread(void *unused)
{
    (void)unused;
    L = luaL_newstate();
    luaL_openlibs(L);
    lua_register(L, "http", l_http);
    lua_register(L, "log", l_log);
    lua_register(L, "subs", l_subs);
    lua_register(L, "sub_avatar", l_sub_avatar);
    if (luaL_dofile(L, "game:\\youtube.lua")) {             /* (a broken update: the copy before it) */
        say("youtube.lua failed to load: %s", lua_tostring(L, -1));
        lua_settop(L, 0);
        if (!luaL_dofile(L, "game:\\youtube.lua.bak")) say("loaded youtube.lua.bak instead");
    }
    if (!lua_isnil(L, -1) && lua_gettop(L) > 0 && lua_isstring(L, -1)) {
        say("youtube.lua failed to load: %s", lua_tostring(L, -1));
        EnterCriticalSection(&g_lock);
        copy_str(g_message, sizeof(g_message), "youtube.lua couldn't be loaded (see log.txt).");
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    lua_getglobal(L, "SCRIPT_VERSION");
    EnterCriticalSection(&g_lock);
    copy_str(g_script_ver, sizeof(g_script_ver), lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    LeaveCriticalSection(&g_lock);
    lua_pop(L, 1);
    apply_region();
    script_update();                                    /* fixes from the internet (if switched on + an address set) */
    for (;;) {
        JOB j;
        WaitForSingleObject(g_job_event, INFINITE);
        for (;;) {                  /* the event only says "something's waiting": empty both slots */
            run_waiting_play();
            EnterCriticalSection(&g_lock);
            j = g_job;
            g_job.type = JOB_NONE;
            LeaveCriticalSection(&g_lock);
            if (j.type == JOB_NONE) break;
            say("job: %s \"%s\"", j.type == JOB_PAGE ? "page" : j.type == JOB_CHANNEL ? "channel" :
                j.type == JOB_PREVIEW ? "preview" : j.type == JOB_PLAYLIST ? "playlist" : "search", j.text);
            if (j.type == JOB_PAGE) job_page(&j);
            else if (j.type == JOB_SEARCH) job_search(&j);
            else if (j.type == JOB_CHANNEL) job_channel(&j);
            else if (j.type == JOB_PLAYLIST) job_playlist(&j);
            else if (j.type == JOB_PREVIEW) {                                   /* search page: one row (+ filter) */
                char fp[16];
                EnterCriticalSection(&g_lock);
                copy_str(fp, sizeof(fp), SFILTERS[g_sfilter].param);
                LeaveCriticalSection(&g_lock);
                load_section(0, j.gen, j.text, fp);
            }
        }
    }
}

static void post_job(int type, const char *text)    /* call with g_lock held */
{
    JOB *slot = type == JOB_PLAY ? &g_play_job : &g_job;
    slot->type = type;
    slot->gen = type == JOB_PLAY ? g_play_seq : g_gen;
    copy_str(slot->text, sizeof(slot->text), text);
    SetEvent(g_job_event);
}

/* ---------------------------------------------------------------- thumbnail threads */
static DWORD WINAPI thumb_thread(void *unused)
{
    char url[128], id[16];
    (void)unused;
    for (;;) {
        int found = 0, gen = 0, si = 0, vi = 0, d, r, status, len, w, h, local = 0;
        char *reply;
        unsigned int *px = NULL;
        EnterCriticalSection(&g_lock);
        if (!g_paused_thumbs) {
            /* the focused row first, then outward */
            for (d = 0; d < 2 * MAX_SECTIONS && !found; d++) {
                int row = g_focus_row + ((d & 1) ? (d + 1) / 2 : -(d / 2));
                if (row < 0 || row >= g_nsec || g_sec[row].state != 2) continue;
                for (r = 0; r < g_sec[row].n; r++)
                    if (g_sec[row].v[r].thumb == 0 && own_id_shape(g_sec[row].v[r].id)) g_sec[row].v[r].thumb = 4;
                    else if (g_sec[row].v[r].thumb == 0) {     /* (your own files have no picture to fetch) */
                        found = 1; si = row; vi = r;
                        g_sec[row].v[r].thumb = 1;
                        copy_str(id, sizeof(id), g_sec[row].v[r].id);
                        gen = g_gen;
                        break;
                    }
            }
        }
        if (found) local = offline_find(id) >= 0;
        LeaveCriticalSection(&g_lock);
        if (!found) { Sleep(40); continue; }
        if (local) {                                /* saved on the console: its picture is on the hard drive */
            FILE *f;
            char path[120];
            offline_path(path, sizeof(path), id, "thumb.jpg");
            f = fopen(path, "rb");
            if (f) {
                unsigned char *buf;
                fseek(f, 0, SEEK_END);
                len = (int)ftell(f);
                fseek(f, 0, SEEK_SET);
                buf = len > 0 && len < 1024 * 1024 ? (unsigned char *)malloc(len) : NULL;
                if (buf && (int)fread(buf, 1, len, f) == len) px = sbimage_decode_jpeg(buf, len, &w, &h);
                free(buf);
                fclose(f);
            }
        }
        _snprintf(url, sizeof(url) - 1, "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
        url[sizeof(url) - 1] = 0;
        if (!px && !sbhttp_fetch("GET", url, NULL, NULL, 0, &status, &reply, &len)) {
            if (status == 200) px = sbimage_decode_jpeg((unsigned char *)reply, len, &w, &h);
            sbhttp_free(reply);
        }
        InterlockedIncrement(px ? &g_thumbs_ok : &g_thumbs_bad);
        EnterCriticalSection(&g_lock);
        if (gen == g_gen && si < g_nsec && !strcmp(g_sec[si].v[vi].id, id)) {
            VIDEO *v = &g_sec[si].v[vi];
            if (px) { v->pix = px; v->pw = w; v->ph = h; v->thumb = 2; px = NULL; }
            else v->thumb = 4;
        }
        LeaveCriticalSection(&g_lock);
        if (px) sbimage_free(px);
    }
}

/* ---------------------------------------------------------------- download thread (saving videos) */
#define DL_BUF (256 * 1024)
#define AUDIO_BYTES_PER_S 16500         /* guess for the progress bar until the sound's real size is known (128 kbit) */

/* one stream -> one file. Progress goes into the item (found again by id: the list can move). 1 = OK,
   0 = failed (*why), -1 = cancelled */
static int dl_file(const char *id, const char *url, const char *path, unsigned char *buf, long long *got,
                   long long guess_rest, char *why, int whyn)
{
    sbhttp *h = sbhttp_open(url);
    FILE *f;
    long long size, done = 0;
    int r = 1, k;
    if (!h) { copy_str(why, whyn, "The download couldn't be started."); return 0; }
    size = sbhttp_size(h);
    f = fopen(path, "wb");
    if (!f) { sbhttp_close(h); copy_str(why, whyn, "The hard drive refused the file."); return 0; }
    EnterCriticalSection(&g_lock);
    if ((k = dl_find(id)) >= 0 && size > 0) g_dl[k].total = *got + size + guess_rest;
    LeaveCriticalSection(&g_lock);
    for (;;) {
        int n, cancel, hold;
        EnterCriticalSection(&g_lock);
        k = dl_find(id);
        cancel = k < 0 || g_dl[k].cancel;
        hold = g_dl_hold;
        if (k >= 0) g_dl[k].got = *got + done;
        LeaveCriticalSection(&g_lock);
        if (cancel) { r = -1; break; }
        if (hold) { Sleep(250); continue; }         /* a video is streaming: let it have the network */
        if (size > 0 && done >= size) break;
        n = sbhttp_read(h, buf, DL_BUF);
        if (n == 0) break;                          /* the end */
        if (n < 0) { copy_str(why, whyn, "The download broke off (network?)."); r = 0; break; }
        if ((int)fwrite(buf, 1, n, f) != n) { copy_str(why, whyn, "The hard drive is full or refused to write."); r = 0; break; }
        done += n;
    }
    fclose(f);
    sbhttp_close(h);
    if (r == 1 && (done == 0 || (size > 0 && done != size))) { copy_str(why, whyn, "The download came out incomplete."); r = 0; }
    *got += done;
    return r;
}

static DWORD WINAPI dl_thread(void *unused)
{
    unsigned char *buf = (unsigned char *)malloc(DL_BUF);
    (void)unused;
    if (!buf) return 0;
    for (;;) {
        static char vurl[URL_MAX], aurl[URL_MAX];
        char id[16] = "", dir[96], path[120], why[100] = "";
        VIDEO v;
        int i, k, r, height = 0, status, len;
        long long got = 0;
        char *reply;
        WaitForSingleObject(g_dl_event, 1000);
        EnterCriticalSection(&g_lock);
        for (i = 0; i < g_ndl && !g_dl_hold; i++)
            if (g_dl[i].state == DL_READY) {
                g_dl[i].state = DL_RUN;
                copy_str(id, sizeof(id), g_dl[i].v.id);
                strcpy(vurl, g_dl[i].vurl);
                strcpy(aurl, g_dl[i].aurl);
                height = g_dl[i].height;
                v = g_dl[i].v;
                g_off_dirty = 1;
                break;
            }
        LeaveCriticalSection(&g_lock);
        if (!id[0]) continue;
        say("save %s: downloading (%dp)", id, height);
        offline_path(dir, sizeof(dir), id, NULL);
        CreateDirectory(OFF_DIR, NULL);
        CreateDirectory(dir, NULL);
        offline_path(path, sizeof(path), id, "video.mp4");
        r = dl_file(id, vurl, path, buf, &got, (long long)v.seconds * AUDIO_BYTES_PER_S, why, sizeof(why));
        if (r == 1) {
            offline_path(path, sizeof(path), id, "audio.m4a");
            r = dl_file(id, aurl, path, buf, &got, 0, why, sizeof(why));
        }
        if (r == 1) {                               /* the thumbnail (a missing one is no reason to fail) */
            char url[128];
            _snprintf(url, sizeof(url) - 1, "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
            url[sizeof(url) - 1] = 0;
            if (!sbhttp_fetch("GET", url, NULL, NULL, 0, &status, &reply, &len)) {
                if (status == 200) {
                    FILE *f;
                    offline_path(path, sizeof(path), id, "thumb.jpg");
                    if ((f = fopen(path, "wb")) != NULL) { fwrite(reply, 1, len, f); fclose(f); }
                }
                sbhttp_free(reply);
            }
        }
        EnterCriticalSection(&g_lock);
        k = dl_find(id);
        if (r == 1 && k >= 0 && g_noff < MAX_OFF) {     /* saved: to the front of the Offline Player list */
            char t[160];
            memmove(&g_off[1], &g_off[0], g_noff * sizeof(OFFVID));
            memset(&g_off[0], 0, sizeof(OFFVID));
            g_off[0].v = g_dl[k].v;
            g_off[0].height = height;
            g_off[0].kb = (int)(got / 1024);
            g_noff++;
            offline_save();
            memmove(&g_dl[k], &g_dl[k + 1], (g_ndl - k - 1) * sizeof(DLITEM));
            g_ndl--;
            _snprintf(t, sizeof(t) - 1, "Saved: %.120s", v.title);
            t[sizeof(t) - 1] = 0;
            note(t);
            say("save %s: done, %d KB", id, (int)(got / 1024));
        } else {
            offline_delete_files(id);
            if (k >= 0 && (r < 0 || g_dl[k].cancel)) {  /* cancelled: gone from the list */
                memmove(&g_dl[k], &g_dl[k + 1], (g_ndl - k - 1) * sizeof(DLITEM));
                g_ndl--;
                say("save %s: cancelled", id);
            } else if (k >= 0) {
                g_dl[k].state = DL_FAILED;
                copy_str(g_dl[k].err, sizeof(g_dl[k].err), why[0] ? why : "Saving failed.");
                say("save %s: FAILED %s", id, g_dl[k].err);
            }
        }
        g_off_dirty = 1;
        LeaveCriticalSection(&g_lock);
    }
}

/* ---------------------------------------------------------------- controller */
static WORD g_held, g_pressed, g_repeat;   /* g_repeat = presses + auto-repeat while a direction is held */
static double g_dir_since, g_dir_next;

static void read_pad(void)
{
    XINPUT_STATE st;
    WORD b = 0, dirs;
    double t = now_s();
    if (XInputGetState(0, &st) == ERROR_SUCCESS) {
        b = st.Gamepad.wButtons;
        if (st.Gamepad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (st.Gamepad.sThumbLX > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (st.Gamepad.sThumbLY > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        {
            int ry = st.Gamepad.sThumbRY;               /* (dead zone 8000 of 32767) */
            g_rsy = ry > 8000 ? (ry - 8000) / 24767.0f : ry < -8000 ? (ry + 8000) / 24767.0f : 0;
        }
    } else g_rsy = 0;
    g_pressed = b & ~g_held;
    g_repeat = g_pressed;
    dirs = b & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
    if (g_pressed & dirs) { g_dir_since = t; g_dir_next = t + 0.40; }
    else if (dirs && t >= g_dir_next) { g_repeat |= dirs; g_dir_next = t + 0.11; }
    g_held = b;
}

/* ---------------------------------------------------------------- menu state */
enum { M_MENU, M_PLAY };
static int g_mode = M_MENU;
static int g_side_focus = 0;        /* 1 = the sidebar has the focus */
static int g_sub_focus = 0;         /* Subscriptions page: 1 = the channel column has the focus */
static int g_sub_cur = 0, g_sub_top = 0;   /* the column's chosen channel (g_subs index) and first row shown */
static double g_sub_moved = -1;     /* when the column cursor last moved (its channel loads after a short pause) */
static float g_subcol_t = 1;        /* how far the column is out: 1 = shown, 0 = slid away under the sidebar (animated) */
#define ON_SUBS (g_side_page == SUBS_PAGE)
static int g_side_cur = 1;          /* sidebar cursor */
static int g_side_page = 1;         /* page shown (CHAN_PAGE = a channel page) */

static float g_sy;                  /* vertical scroll (animated) */
static int g_info;                  /* hold-A info panel open */
static double g_a_down = -1;
static int g_a_used;

/* keyboard */
static XOVERLAPPED g_kb_ov;
static WCHAR g_kb_buf[256];
static int g_kb_open;

static void free_page_images(void)  /* g_lock held */
{
    int i, k;
    for (i = 0; i < g_nsec; i++)
        for (k = 0; k < g_sec[i].n; k++) {
            VIDEO *v = &g_sec[i].v[k];
            if (v->img) { p360_image_free(v->img); v->img = NULL; }
            if (v->pix) { sbimage_free(v->pix); v->pix = NULL; }
        }
}

/* page cache: a page you leave after it finished loading is kept (rows, thumbnails, where you were), so going back
   to it is instant. Kept CACHE_SECONDS; at most CACHE_PAGES other pages (thumbnails use ~20 MB per page) */
#define CACHE_SECONDS 600
#define CACHE_PAGES 2
typedef struct {
    int used;
    double fetched, last_used;
    int nsec, focus_row;
    float sy;
    SECTION sec[MAX_SECTIONS];
} PAGECACHE;
static PAGECACHE g_cache[N_SIDE];
static double g_page_fetched;       /* when the page on screen was downloaded */

static void cache_free(PAGECACHE *c)
{
    int i, k;
    for (i = 0; i < c->nsec; i++)
        for (k = 0; k < c->sec[i].n; k++) {
            VIDEO *v = &c->sec[i].v[k];
            if (v->img) { p360_image_free(v->img); v->img = NULL; }
            if (v->pix) { sbimage_free(v->pix); v->pix = NULL; }
        }
    c->used = 0;
}

static void leave_page(void)        /* g_lock held: keep the page on screen in the cache, or free it */
{
    int i, k, done = g_nsec > 0, count = 0, oldest = -1;
    for (i = 0; i < g_nsec; i++)
        if (g_sec[i].state != 2) done = 0;
    if (g_side_page < 1 || g_side_page >= N_SIDE || !SIDE[g_side_page].page || !done) {
        free_page_images();
        return;
    }
    {
        PAGECACHE *c = &g_cache[g_side_page];
        if (c->used) cache_free(c);
        c->used = 1;
        c->fetched = g_page_fetched;
        c->last_used = now_s();
        c->nsec = g_nsec;
        c->focus_row = g_focus_row;
        c->sy = g_sy;
        memcpy(c->sec, g_sec, sizeof(SECTION) * g_nsec);    /* the pictures now belong to the cache */
        for (i = 0; i < g_nsec; i++)
            for (k = 0; k < c->sec[i].n; k++)
                if (c->sec[i].v[k].thumb == 1) c->sec[i].v[k].thumb = 0;   /* a half-done download starts again */
    }
    for (i = 0; i < N_SIDE; i++)    /* too many kept: drop the one unused longest */
        if (g_cache[i].used) {
            count++;
            if (oldest < 0 || g_cache[i].last_used < g_cache[oldest].last_used) oldest = i;
        }
    if (count > CACHE_PAGES) cache_free(&g_cache[oldest]);
}

static void open_page(int side)
{
    PAGECACHE *c = &g_cache[side];
    EnterCriticalSection(&g_lock);
    leave_page();
    g_gen++;
    g_nsec = 0;
    g_message[0] = 0;
    g_focus_row = 0;
    g_sy = 0;
    g_side_page = side;
    if (c->used && now_s() - c->fetched > CACHE_SECONDS) cache_free(c);     /* too old: fresh videos */
    if (c->used && side == 6) cache_free(c);    /* Account = the history: always rebuilt (instant, no network) */
    if (c->used) {
        memcpy(g_sec, c->sec, sizeof(SECTION) * c->nsec);
        g_nsec = c->nsec;
        g_focus_row = c->focus_row;
        g_sy = c->sy;
        g_page_fetched = c->fetched;
        c->used = 0;                /* it's the live page again (no double ownership of the pictures) */
        if (side == 1) {            /* Home: "Continue watching" as it is now */
            g_focus_row += refresh_continue(g_sec, &g_nsec);
            if (g_focus_row < 0) g_focus_row = 0;
            if (g_focus_row >= g_nsec) g_focus_row = g_nsec ? g_nsec - 1 : 0;
        }
        say("page %s from the cache (%.0f s old)", SIDE[side].page, now_s() - c->fetched);
    } else if (SIDE[side].page) {
        g_page_fetched = now_s();
        post_job(JOB_PAGE, SIDE[side].page);
    }
    else if (side == OFFLINE_PAGE) {
        offline_rows();
        g_off_dirty = 0;
    }
    LeaveCriticalSection(&g_lock);
}

static void open_search(const char *text)
{
    EnterCriticalSection(&g_lock);
    {
        static char last[Q_MAX];
        /* (the filter stays as chosen: it is picked on the search page's Filter key before searching) */
        copy_str(last, sizeof(last), text);
    }
    leave_page();
    g_gen++;
    g_nsec = 0;
    g_message[0] = 0;
    g_focus_row = 0;
    g_sy = 0;
    g_side_page = 0;
    post_job(JOB_SEARCH, text);
    recent_add(text);                       /* remembered for the search page */
    copy_str(g_q, Q_MAX, text);
    LeaveCriticalSection(&g_lock);
}

static void start_keyboard(void)            /* the Xbox's own keyboard (other languages), starting with g_q */
{
    static WCHAR start[Q_MAX];
    DWORD r;
    int i;
    if (g_kb_open) return;
    for (i = 0; g_q[i] && i < Q_MAX - 1; i++) start[i] = (unsigned char)g_q[i];   /* (typed text is plain ASCII) */
    start[i] = 0;
    ZeroMemory(&g_kb_ov, sizeof(g_kb_ov));
    g_kb_buf[0] = 0;
    r = XShowKeyboardUI(0, VKBD_DEFAULT, start, L"Search NewTube", L"What would you like to watch?", g_kb_buf, 255,
                        &g_kb_ov);
    if (r == ERROR_IO_PENDING) g_kb_open = 1;
    else say("keyboard couldn't open (%u)", r);
}

static void utf16_to_utf8(const WCHAR *w, char *out, int max)
{
    int n = 0;
    for (; *w && n < max - 4; w++) {
        unsigned int c = *w;
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 63)); }
        else { out[n++] = (char)(0xE0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 63)); out[n++] = (char)(0x80 | (c & 63)); }
    }
    out[n] = 0;
}

static int g_kb_mode;                       /* what the Xbox keyboard is for: 0 a search, 1 a new playlist's name */
static void pick_new_list(const char *name);

static void check_keyboard(void)            /* (called every frame, in the menu and in the player) */
{
    char text[400];
    if (!g_kb_open || !XHasOverlappedIoCompleted(&g_kb_ov)) return;
    g_kb_open = 0;
    if (XGetOverlappedResult(&g_kb_ov, NULL, FALSE) != ERROR_SUCCESS) return;   /* cancelled */
    utf16_to_utf8(g_kb_buf, text, sizeof(text));
    if (g_kb_mode == 1) { g_kb_mode = 0; if (text[0]) pick_new_list(text); return; }
    if (text[0]) {
        say("search: %s", text);
        open_search(text);
        g_side_focus = 0;
    }
}

/* ---------------------------------------------------------------- drawing pieces */
static void spinner(float cx, float cy, float r, unsigned int rgb)
{
    int i;
    double t = now_s();
    for (i = 0; i < 10; i++) {
        float a = (float)(i * 6.2831853 / 10);
        float k = (float)fmod(t * 1.4 - i / 10.0, 1.0);
        unsigned int alpha = (unsigned int)(40 + 215 * (1.0 - (k < 0 ? k + 1 : k)));
        p360_rect(cx + r * (float)sin(a) - 3, cy - r * (float)cos(a) - 3, 6, 6, (alpha << 24) | (rgb & 0xFFFFFF));
    }
}

/* a video tile w wide (picture 16:9 + 2 title lines + "views | age" + channel); focused = the dark card */
static void draw_tile_w(VIDEO *v, float x, float y, float w, int focused)
{
    float dw, ty, h = w * 9 / 16;
    char meta[120];
    if (focused) p360_rect(x - 8, y - 9, w + 17, h + 121, C_CARD);
    if (v->img) p360_image_draw(v->img, x, y, w, h, 0xFFFFFFFF);
    else if (own_id_shape(v->id)) {                 /* your own file: a dark "film" card with its type */
        int i;
        p360_rect_v(x, y, w, h, 0xFF3A3F45, 0xFF1E2226);
        for (i = 0; i < 9; i++) {                   /* film-strip holes along the top and bottom */
            p360_rect(x + 8 + i * (w - 16) / 9, y + 6, w / 24, h / 14, 0xFF15181B);
            p360_rect(x + 8 + i * (w - 16) / 9, y + h - 6 - h / 14, w / 24, h / 14, 0xFF15181B);
        }
        p360_text(F_BOLD, x + w / 2 - p360_text_width(F_BOLD, w * 0.14f, v->age) / 2, y + h / 2 - w * 0.09f,
                  w * 0.14f, 0xFFBBC2C8, v->age);
    } else {
        p360_rect(x, y, w, h, 0xFFCFCFCF);
        p360_frame_rect(x, y, w, h, 1, 0xFFBDBDBD);
    }
    if (v->list[0]) {                               /* a playlist: the 2012 dark band with the video count */
        char num[24];
        const char *sp = strchr(v->dur, ' ');
        int n = sp ? (int)(sp - v->dur) : 0, i;
        float bw = w * 0.38f, bx = x + w - bw, cx = bx + bw / 2, cy = y + h / 2;
        if (n >= (int)sizeof(num)) n = sizeof(num) - 1;
        memcpy(num, v->dur, n);
        num[n] = 0;
        p360_rect(bx, y, bw, h, 0xD0101010);
        p360_text(F_BOLD, cx - p360_text_width(F_BOLD, w * 0.11f, num) / 2, cy - h * 0.30f, w * 0.11f, 0xFFFFFFFF, num);
        p360_text(F_REG, cx - p360_text_width(F_REG, w * 0.06f, "VIDEOS") / 2, cy - h * 0.06f, w * 0.06f, 0xFFDDDDDD, "VIDEOS");
        for (i = 0; i < 3; i++)                     /* the "list" sign: three lines */
            p360_rect(cx - bw * 0.18f, cy + h * 0.16f + i * h * 0.07f, bw * 0.36f, h * 0.035f, 0xFFDDDDDD);
    } else {
        float seen = watch_progress(v->id);         /* watched before: how far, as a red bar along the bottom */
        if (seen >= 0) {
            if (seen < 0.03f) seen = 0.03f;
            p360_rect(x, y + h - 5, w, 5, 0xB0404040);
            p360_rect(x, y + h - 5, w * seen, 5, 0xFFE52D27);
        }
    }
    if (!v->list[0]) {                              /* saved on the console / being saved */
        int d = dl_find(v->id);
        char tag[48];
        if (d >= 0) {
            DLITEM *it = &g_dl[d];
            float p = it->total > 0 ? (float)((double)it->got / (double)it->total) : 0;
            if (p > 0.99f) p = 0.99f;
            if (it->state == DL_FAILED) copy_str(tag, sizeof(tag), "SAVING FAILED");
            else if (it->state == DL_RUN && g_dl_hold) copy_str(tag, sizeof(tag), "SAVING: PAUSED");
            else if (it->state == DL_RUN) { _snprintf(tag, sizeof(tag) - 1, "SAVING %d%%", (int)(p * 100)); tag[sizeof(tag) - 1] = 0; }
            else copy_str(tag, sizeof(tag), "WAITING TO SAVE");
            p360_rect(x, y + h - 8, w, 8, 0xC0202020);
            if (it->state == DL_RUN) p360_rect(x, y + h - 8, w * p, 8, C_GREEN);
            dw = p360_text_width(F_BOLD, 14, tag) + 10;
            p360_rect(x + 4, y + 4, dw, 20, it->state == DL_FAILED ? 0xF0AA2222 : 0xE0000000);
            p360_text(F_BOLD, x + 9, y + 6, 14, 0xFFFFFFFF, tag);
        } else if (offline_find(v->id) >= 0) {
            dw = p360_text_width(F_BOLD, 14, "SAVED") + 10;
            p360_rect(x + 4, y + 4, dw, 20, 0xF0408A12);
            p360_text(F_BOLD, x + 9, y + 6, 14, 0xFFFFFFFF, "SAVED");
        } else if (v->fresh) {                      /* Subscriptions: new since your last visit */
            dw = p360_text_width(F_BOLD, 14, "NEW") + 10;
            p360_rect(x + 4, y + 4, dw, 20, 0xF01E73BE);
            p360_text(F_BOLD, x + 9, y + 6, 14, 0xFFFFFFFF, "NEW");
        }
    }
    if (v->dur[0] && !v->list[0]) {
        dw = p360_text_width(F_BOLD, 16, v->dur) + 10;
        /* (a live stream's tag says LIVE, on red like YouTube's) */
        p360_rect(x + w - dw - 4, y + h - 28, dw, 20, strcmp(v->dur, "LIVE") ? 0xE0000000 : 0xF0CC1A1A);
        p360_text(F_BOLD, x + w - dw + 1, y + h - 27, 16, 0xFFFFFFFF, v->dur);
    }
    if (v->age[0]) _snprintf(meta, sizeof(meta) - 1, "%s | %s", v->views, v->age);
    else _snprintf(meta, sizeof(meta) - 1, "%s", v->views);
    meta[sizeof(meta) - 1] = 0;
    ty = y + h + 9;
    if (focused) {
        int lines = p360_text_wrap(F_REG, x + 2, ty, w - 4, 20, 2, 0xFFFFFFFF, v->title);
        ty += lines * p360_text_line(F_REG, 20) + 4;
        p360_text_fit(F_REG, x + 2, ty, w - 4, 15, 0xFFBBBBBB, meta);
        p360_text_fit(F_REG, x + 2, ty + 20, w - 4, 15, 0xFFBBBBBB, v->channel);
    } else {
        int lines = p360_text_wrap(F_BOLD, x, ty, w, 19, 2, C_TITLE, v->title);
        ty += lines * p360_text_line(F_BOLD, 19) + 3;
        p360_text_fit(F_REG, x, ty, w, 15, C_META, meta);
        p360_text_fit(F_REG, x, ty + 19, w, 15, C_META, v->channel);
    }
}

static void draw_tile(VIDEO *v, float x, float y, int focused) { draw_tile_w(v, x, y, TILE_W, focused); }

static void draw_sections(void)
{
    int i, k;
    /* Subscriptions: everything moves right past the channel column, and the rows run to the screen edge */
    float ox = ON_SUBS ? (SUBCOL_W - 1) * g_subcol_t : 0, X0 = TILE_X0 + ox, CX = CONTENT_X + ox, TX = 337 + ox;
    float right = ON_SUBS ? 1360.0f : 1270.0f, ch = ON_SUBS ? 650.0f : 610.0f;
    p360_clip(CX, 110, right - CX, ch);
    if (g_message[0]) {
        float mw = ON_SUBS ? 700.0f : 980.0f;
        p360_rect(X0, 150, mw, 120, 0xFFF7F7F7);
        p360_frame_rect(X0, 150, mw, 120, 1, 0xFFD0D0D0);
        p360_text_wrap(F_REG, X0 + 24, 186, mw - 50, 24, 2, C_SECTION, g_message);
    } else if (g_nsec == 0) {
        char t[260];
        if (g_side_page == PL_PAGE) _snprintf(t, sizeof(t) - 1, "Loading the playlist \"%.200s\"...", g_plp.title);
        else copy_str(t, sizeof(t), "Loading...");
        t[sizeof(t) - 1] = 0;
        spinner(X0 + 40, 180, 16, 0xFF555555);
        p360_text_fit(F_REG, X0 + 72, 168, 900 - ox, 24, C_SECTION, t);
    }
    for (i = 0; i < g_nsec; i++) {
        SECTION *s = &g_sec[i];
        float y = ROW_Y0 + i * ROW_STEP - g_sy + (ON_RESULTS ? g_kb_t * KB_SLIDE : 0);
        char t[100];
        if (y > 720 || y + ROW_STEP < 110) continue;
        p360_image_draw(IMG_BADGE, X0, y, 60, 64, 0xFFFFFFFF);
        _snprintf(t, sizeof(t) - 1, "%s \xC2\xBB", s->title);
        t[sizeof(t) - 1] = 0;
        p360_text(F_REG, TX, y + 17, 26, C_SECTION, t);
        if (!strcmp(s->filter, "@channel") && !ON_SUBS)             /* a channel's row (the column says X itself) */
            p360_text(F_REG, 1240 - p360_text_width(F_REG, 18, "X  Channel page"), y + 24, 18, C_META, "X  Channel page");
        if (!strcmp(s->filter, "@list") || !strcmp(s->filter, "@pl"))   /* Account / a YouTube playlist */
            pill(1240 - 140, y + 14, 140, 40, "X  Play all", 0);
        if (i == 0 && g_side_page == 0 && g_q[0]) {                 /* search results: the filter */
            char f[80];
            _snprintf(f, sizeof(f) - 1, "LB / RB  Filter: %s", SFILTERS[g_sfilter].name);
            f[sizeof(f) - 1] = 0;
            p360_text(F_REG, 1240 - p360_text_width(F_REG, 18, f), y + 24, 18, g_sfilter ? 0xFF4A8A1E : C_META, f);
        }
        if (s->n && (!strcmp(s->filter, "@latest") || !strcmp(s->filter, "@channel"))) {   /* Subscriptions: NEW count */
            int k2, nn = 0;
            for (k2 = 0; k2 < s->n; k2++) nn += s->v[k2].fresh;
            if (nn) {
                char f[32];
                float tw = p360_text_width(F_REG, 26, t);
                _snprintf(f, sizeof(f) - 1, "%d new", nn);
                f[sizeof(f) - 1] = 0;
                p360_rect(TX + tw + 14, y + 18, p360_text_width(F_BOLD, 16, f) + 14, 24, 0xFF1E73BE);
                p360_text(F_BOLD, TX + tw + 21, y + 21, 16, 0xFFFFFFFF, f);
            }
        }
        if (i == 0 && g_side_page == PL_PAGE) {                     /* who made it, how many videos */
            char by[160];
            _snprintf(by, sizeof(by) - 1, "by %s  \xC2\xB7  %s  \xC2\xB7  B  Back", g_plp.channel, g_plp.count);
            by[sizeof(by) - 1] = 0;
            p360_text_fit(F_REG, TX, y + 50, 880, 17, C_META, by);
        }
        if (s->state < 2) {
            for (k = 0; k < 4; k++) p360_rect(X0 + k * TILE_STEP, y + 80, TILE_W, TILE_H, 0xFFDADADA);
            spinner(X0 + 2 * TILE_STEP - 9, y + 80 + TILE_H / 2, 14, 0xFF666666);
            continue;
        }
        if (s->state == 3 || s->n == 0) {
            p360_text(F_REG, X0, y + 100, 22, 0xFF995555, s->err[0] ? s->err : "Nothing here.");
            continue;
        }
        p360_clip(X0 - 10, 110, right - 10 - X0, ch);
        for (k = 0; k < s->n; k++) {
            float x = X0 + k * TILE_STEP - s->sx;
            if (x < X0 - TILE_STEP || x > right) continue;
            draw_tile(&s->v[k], x, y + 80, !g_side_focus && !(ON_SUBS && g_sub_focus) && i == g_focus_row && k == s->col);
        }
        p360_clip(CX, 110, right - CX, ch);
        if (!ON_SUBS && s->first + 4 < s->n) {                                  /* more to the right */
            p360_rect(1252, y + 80 + TILE_H / 2 - 20, 14, 40, 0x80000000);
            p360_text(F_BOLD, 1253, y + 80 + TILE_H / 2 - 13, 22, 0xFFFFFFFF, "\xC2\xBB");
        }
    }
    p360_clip_off();
}

/* Subscriptions: the channel column (owner's mockup): one row per subscribed channel, a picture + its name; the chosen
   one dark with the green bar on its right edge (like the sidebar). Row lines run on down to the bottom. g_lock held */
/* the rows for "All subscriptions" (id "") or one channel; reloads only when that changes. g_lock held */
static void sub_show(const char *id)
{
    if (!strcmp(id, g_sub_vid) && (g_nsec || g_message[0])) return;
    copy_str(g_sub_vid, sizeof(g_sub_vid), id);
    free_page_images();
    g_gen++;                                /* (rows / thumbnails still on the way for the old choice are dropped) */
    g_nsec = 0;
    g_message[0] = 0;
    g_focus_row = 0;
    g_sy = 0;
    g_page_fetched = now_s();
    post_job(JOB_PAGE, "Subscriptions");
}

static int sub_view_row(void) { return sub_find(g_sub_vid) + 1; }   /* the column row on show (0 = All) */

static void draw_sub_column(void)
{
    int i, cols = g_nsubs + 1, shown = sub_view_row();
    float x0 = SIDE_W - (1 - g_subcol_t) * SUBCOL_W, w = SUBCOL_W, bar_h = 36 + 110 * 0.9f;
    if (g_subcol_t < 0.01f) return;                                 /* (hidden: the sidebar covers the rest) */
    p360_draw_safe(0);                                              /* the band, from the top bar to the bottom edge */
    p360_rect(64 + x0 * 0.9f, bar_h, w * 0.9f, 720 - bar_h, C_SIDEBAR);
    p360_rect(64 + (x0 + w) * 0.9f - 2, bar_h, 2, 720 - bar_h, 0xFF2E2E2E);
    p360_draw_safe(1);
    if (g_sub_cur >= cols) g_sub_cur = cols - 1;
    if (g_sub_cur < 0) g_sub_cur = 0;
    if (g_sub_cur < g_sub_top) g_sub_top = g_sub_cur;
    if (g_sub_cur >= g_sub_top + SUBCOL_ROWS) g_sub_top = g_sub_cur - SUBCOL_ROWS + 1;
    for (i = 0; i < SUBCOL_ROWS + 1; i++) {
        int k = g_sub_top + i;                                      /* row 0 = All subscriptions, k = g_subs[k - 1] */
        float y = SIDE_Y0 + i * SUBCOL_STEP;
        if (k < cols) {
            int sel = k == shown, cur = k == g_sub_cur && g_sub_focus && !g_side_focus;
            if (sel || cur) p360_rect(x0, y, w, SUBCOL_STEP - 1, cur ? C_SIDE_CUR : C_SIDE_SEL);
            if (sel) p360_rect(x0 + w - 6, y, 6, SUBCOL_STEP - 1, C_GREEN);   /* (over the edge line) */
            if (k == 0) p360_image_draw(IMG_SIDE[SUBS_PAGE], x0 + 12, y + 12, 28, 28, sel || cur ? 0xFFFFFFFF : 0xFFBBBBBB);
            else {                                                  /* the channel's picture (grey person until then) */
                const char *av = g_subs[k - 1].avatar;
                p360_image *pic = av[0] ? pimg_get(pimg_want(av)) : NULL;
                p360_image_draw(pic ? pic : IMG_AVATAR, x0 + 6, y + 6, 40, 40, 0xFFFFFFFF);
            }
            p360_text_fit(sel ? F_BOLD : F_REG, x0 + 60, y + 15, w - 80, 20, sel || cur ? 0xFFFFFFFF : 0xFFE4E4E4,
                          k == 0 ? "All subscriptions" : g_subs[k - 1].title);
        }
        p360_rect(x0, y + SUBCOL_STEP - 1, w - 2, 1, C_LINE_DARK);
        p360_rect(x0, y + SUBCOL_STEP, w - 2, 1, C_LINE_LITE);
    }
    if (!g_nsubs)
        p360_text_wrap(F_REG, x0 + 16, SIDE_Y0 + SUBCOL_STEP + 16, w - 32, 18, 4, 0xFFBBBBBB,
                       "No subscriptions yet. Open a channel and press Subscribe.");
    if (g_sub_focus && !g_side_focus && g_nsubs)                    /* what the buttons do here */
        p360_text_fit(F_REG, x0 + 16, 728, w - 32, 15, 0xFF999999,
                      g_sub_cur ? "A  Videos    X  Channel page    B  Back" : "A  Videos    B  Back");
}

/* ---- the playlist page: one list over all its rows (g_sec, 24 videos each) */
#define PL_LIST_X  821.0f
#define PL_LIST_Y  209.0f
#define PL_STEP    93.0f
#define PL_THUMB_W 136.0f          /* (16:9 = 76.5 tall: inside the 91-tall dark card with room around it) */
static int pl_total(void) { int r, n = 0; for (r = 0; r < g_nsec; r++) n += g_sec[r].n; return n; }
static VIDEO *pl_at(int i, int *row, int *col)
{
    int r;
    for (r = 0; r < g_nsec; r++) {
        if (i < g_sec[r].n) { if (row) *row = r; if (col) *col = i; return &g_sec[r].v[i]; }
        i -= g_sec[r].n;
    }
    return NULL;
}
static int pl_cur(void)
{
    int r, n = 0;
    if (g_focus_row >= g_nsec) return 0;
    for (r = 0; r < g_focus_row; r++) n += g_sec[r].n;
    return n + g_sec[g_focus_row].col;
}
static void pl_set(int i) { int r, c; if (pl_at(i, &r, &c)) { g_focus_row = r; g_sec[r].col = c; } }

static void fmt_count(char *out, double n)  /* 19514087 -> "19.5M", 6732 -> "6.7K", 950 -> "950" */
{
    if (n >= 1e9) sprintf(out, "%.1fB", n / 1e9);
    else if (n >= 1e6) sprintf(out, "%.1fM", n / 1e6);
    else if (n >= 1e4) sprintf(out, "%.0fK", n / 1e3);
    else if (n >= 1e3) sprintf(out, "%.1fK", n / 1e3);
    else sprintf(out, "%.0f", n);
}

static void draw_playlist_page(void)        /* g_lock held */
{
    int n = pl_total(), cur = pl_cur(), i;
    VIDEO *v = pl_at(cur, NULL, NULL);
    char line[200];
    float y, ty;
    if (g_message[0] || !n) { draw_sections(); return; }        /* (loading / failed: as before) */
    /* header: badge, title, "by ... . N videos . B Back", X Play all */
    p360_image_draw(IMG_BADGE, TILE_X0, ROW_Y0, 60, 64, 0xFFFFFFFF);
    p360_text_fit(F_REG, 337, ROW_Y0 + 17, 760, 26, C_SECTION, g_plp.title);
    _snprintf(line, sizeof(line) - 1, "by %s  \xC2\xB7  %s  \xC2\xB7  B  Back", g_plp.channel, g_plp.count);
    line[sizeof(line) - 1] = 0;
    p360_text_fit(F_REG, 337, ROW_Y0 + 50, 640, 17, C_META, line);
    pill(1240 - 140, ROW_Y0 + 14, 140, 40, "X  Play all", 0);

    /* left: the chosen video big (a sharper picture once it has rested on it), then its details */
    if (v) {
        p360_image *big = NULL;
        float bx = TILE_X0, by = 202, bw = 533, bh = 300;
        if (!strcmp(g_desc_id, v->id) || !strcmp(g_desc_asked, v->id)) {
            char url[96];
            _snprintf(url, sizeof(url) - 1, "https://i.ytimg.com/vi/%s/hqdefault.jpg", v->id);
            url[sizeof(url) - 1] = 0;
            big = pimg_get(pimg_want(url));
        }
        if (big) {                                  /* hqdefault is 4:3 with bars: its middle 16:9 band */
            int iw, ih;
            p360_image_size(big, &iw, &ih);
            p360_image_draw_part(big, 0, ih * 0.125f, (float)iw, ih * 0.75f, bx, by, bw, bh, 0xFFFFFFFF);
        } else if (v->img) p360_image_draw(v->img, bx, by, bw, bh, 0xFFFFFFFF);
        else p360_rect(bx, by, bw, bh, 0xFFCFCFCF);
        {   /* everything from the title down scrolls (right stick), inside the space under the picture */
            float top = by + bh + 16, vis = 752 - top, start;
            float maxs = g_desc_h - vis > 0 ? g_desc_h - vis : 0;
            if (g_desc_scroll > maxs) g_desc_scroll = maxs;
            if (g_desc_scroll < 0) g_desc_scroll = 0;
            p360_clip(bx, top, bw + 4, vis);
            ty = start = top - g_desc_scroll;
            ty += p360_text_wrap(F_BOLD, bx, ty, bw, 24, 0, C_TITLE, v->title) * p360_text_line(F_BOLD, 24) + 6;
            if (v->age[0]) _snprintf(line, sizeof(line) - 1, "%s  |  %s  |  %s", v->channel, v->views, v->age);
            else _snprintf(line, sizeof(line) - 1, "%s  |  %s", v->channel, v->views);
            line[sizeof(line) - 1] = 0;
            p360_text_fit(F_REG, bx, ty, bw, 19, C_TITLE, line);
            ty += 30;
            if (!strcmp(g_desc_id, v->id) && g_desc_likes >= 0) {  /* the like / dislike ratio line */
                double all = g_desc_likes + g_desc_dislikes;
                float bar = 240, part = all > 0 ? (float)(bar * g_desc_likes / all) : bar;
                char lk[24], dk[24];
                fmt_count(lk, g_desc_likes);
                fmt_count(dk, g_desc_dislikes);
                p360_rect(bx, ty + 7, bar, 5, 0xFFCC2A2A);
                p360_rect(bx, ty + 7, part, 5, 0xFF4A9A1E);
                _snprintf(line, sizeof(line) - 1, "%s likes   %s dislikes", lk, dk);
                line[sizeof(line) - 1] = 0;
                p360_text_fit(F_REG, bx + bar + 12, ty, bw - bar - 12, 16, C_META, line);
                ty += 28;
            }
            if (!strcmp(g_desc_id, v->id))
                ty += p360_text_wrap(F_REG, bx, ty, bw, 16, 0, C_META, g_desc_text) * p360_text_line(F_REG, 16);
            else if (!strcmp(g_desc_asked, v->id)) { spinner(bx + 14, ty + 12, 9, 0xFF888888); ty += 30; }
            p360_clip_off();
            g_desc_h = ty - start;                                  /* (the whole block's height, for the next frame) */
            if (maxs > 0) {                                         /* where you are in it */
                float sh = vis * vis / g_desc_h;
                p360_rect(bx + bw + 8, top, 3, vis, 0x30000000);
                p360_rect(bx + bw + 8, top + (vis - sh) * g_desc_scroll / maxs, 3, sh, 0x90000000);
            }
        }
    }

    /* right: the list; the chosen one on a dark card */
    p360_clip(PL_LIST_X - 12, 200, 1360 - PL_LIST_X, 560);
    for (i = 0; i < n; i++) {
        VIDEO *w = pl_at(i, NULL, NULL);
        int f = i == cur && !g_side_focus;
        float th = PL_THUMB_W * 9 / 16, tx = PL_LIST_X + PL_THUMB_W + 8;
        y = PL_LIST_Y + i * PL_STEP - g_pl_sy;
        if (y < 110 || y > 760) continue;
        if (f) p360_rect(PL_LIST_X - 7, y - 7, 445, PL_STEP - 2, C_CARD);
        if (w->img) p360_image_draw(w->img, PL_LIST_X, y, PL_THUMB_W, th, 0xFFFFFFFF);
        else p360_rect(PL_LIST_X, y, PL_THUMB_W, th, 0xFFCFCFCF);
        if (w->dur[0]) {
            float dw = p360_text_width(F_BOLD, 13, w->dur) + 8;
            p360_rect(PL_LIST_X + PL_THUMB_W - dw - 3, y + th - 19, dw, 16, 0xE0000000);
            p360_text(F_BOLD, PL_LIST_X + PL_THUMB_W - dw + 1, y + th - 18, 13, 0xFFFFFFFF, w->dur);
        }
        ty = y + p360_text_wrap(F_BOLD, tx, y - 2, 270, 19, 2, f ? 0xFFFFFFFF : C_TITLE, w->title) *
                 p360_text_line(F_BOLD, 19) + 1;
        if (w->age[0]) _snprintf(line, sizeof(line) - 1, "%s | %s", w->views, w->age);
        else _snprintf(line, sizeof(line) - 1, "%s", w->views);
        line[sizeof(line) - 1] = 0;
        p360_text_fit(F_REG, tx, ty, 270, 15, f ? 0xFFBBBBBB : C_META, line);
        p360_text_fit(F_REG, tx, ty + 19, 270, 15, f ? 0xFFBBBBBB : C_META, w->channel);
    }
    p360_clip_off();
}

static void pl_input(double t)              /* g_lock held: Up / Down through the list, right stick = description */
{
    int n = pl_total(), cur = pl_cur();
    VIDEO *v;
    if ((g_repeat & XINPUT_GAMEPAD_DPAD_UP) && cur > 0) { pl_set(--cur); g_pl_moved = t; g_desc_scroll = 0; }
    if ((g_repeat & XINPUT_GAMEPAD_DPAD_DOWN) && cur + 1 < n) { pl_set(++cur); g_pl_moved = t; g_desc_scroll = 0; }
    g_desc_scroll -= g_rsy * 14;            /* right stick: the description (pushing up = back towards its top) */
    /* (no Left to the sidebar here: B goes back) */
    if (cur < g_pl_top) g_pl_top = cur;
    if (cur > g_pl_top + 4) g_pl_top = cur - 4;
    v = pl_at(cur, NULL, NULL);
    if (v && t - g_pl_moved > 0.45 && strcmp(v->id, g_desc_id) && strcmp(v->id, g_desc_asked)) {
        copy_str(g_desc_asked, sizeof(g_desc_asked), v->id);   /* rested on it: its description + big picture */
        copy_str(g_desc_job, sizeof(g_desc_job), v->id);
        SetEvent(g_job_event);
    }
}

/* the signed-in profile (the first one signed in, controller 1-4): its gamertag + gamer picture. Main thread. */
static void me_refresh(void)
{
    char name[32] = "";
    DWORD i, user = 0xFFFFFFFF;
    for (i = 0; i < 4 && user == 0xFFFFFFFF; i++)
        if (XUserGetSigninState(i) != eXUserSigninState_NotSignedIn) user = i;
    if (user != 0xFFFFFFFF && XUserGetName(user, name, sizeof(name)) != ERROR_SUCCESS) name[0] = 0;
    if (!strcmp(name, g_me_name) && (g_me_pic || !name[0])) return;       /* (nothing changed) */
    copy_str(g_me_name, sizeof(g_me_name), name);
    if (g_me_pic) { p360_image_free(g_me_pic); g_me_pic = NULL; }
    if (name[0]) {
        static unsigned int px[64 * 64];                            /* 64 x 64, 32-bit colour */
        if (XUserReadGamerPicture(user, FALSE, (PBYTE)px, 64 * 4, 64, NULL) == ERROR_SUCCESS) {
            for (i = 0; i < 64 * 64; i++) px[i] |= 0xFF000000;      /* (fully opaque) */
            g_me_pic = p360_image_create(64, 64, px);
        } else say("gamer picture couldn't be read");
        say("signed in: %s", name);
    }
}

static void draw_chrome(int sidebar)     /* the top bar (+ the sidebar: not on the channel page) */
{
    int i;
    float bar_h = 36 + 110 * 0.9f, side_w = 64 + SIDE_W * 0.9f;
    /* bands to the screen edges */
    p360_draw_safe(0);
    p360_rect_v(0, 0, 1280, bar_h, C_TOPBAR, C_TOPBAR2);
    if (sidebar) {
        p360_rect(0, bar_h, side_w, 720 - bar_h, C_SIDEBAR);
        p360_rect(side_w - 3, bar_h, 3, 720 - bar_h, 0xFF2E2E2E);
    }
    p360_draw_safe(1);

    /* top bar */
    p360_image_draw(IMG_LOGO, 32, 27, 183, 60, 0xFFFFFFFF);
    p360_rect(265, 30, 507, 43, 0xFFFFFFFF);
    p360_rect(729, 30, 43, 43, 0xFFE8E8E8);
    p360_rect(728, 30, 1, 43, 0xFFCCCCCC);
    p360_image_draw(IMG_YBTN, 731, 32, 39, 39, 0xFFFFFFFF);
    if (g_side_page == SEARCH_PAGE || (g_side_page == 0 && g_q[0])) {    /* what's typed (+ a blinking caret) */
        float tw = g_q[0] ? p360_text_width(F_REG, 22, g_q) : 0;
        if (tw > 440) tw = 440;
        if (g_q[0]) p360_text_fit(F_REG, 278, 39, 440, 22, 0xFF222222, g_q);
        else p360_text(F_REG, g_side_page == SEARCH_PAGE ? 288.0f : 278.0f, 40, 20, 0xFFA0A0A0, "Search NewTube");
        if (g_side_page == SEARCH_PAGE && fmod(now_s(), 1.0) < 0.55)     /* (empty: just before the grey hint) */
            p360_rect(g_q[0] ? 280 + tw : 280, 37, 2, 29, 0xFF333333);
    } else
        p360_text(F_REG, 278, 40, 20, 0xFFA0A0A0, "Search NewTube");
    {   /* you: gamertag + gamer picture (read again every few seconds: someone may sign in / switch) */
        static double next_check;
        float tw;
        if (now_s() >= next_check) { me_refresh(); next_check = now_s() + 5; }
        p360_rect(1199, 25, 58, 58, 0xFFFFFFFF);                    /* white frame */
        p360_image_draw(g_me_pic ? g_me_pic : IMG_AVATAR, 1201, 27, 54, 54, 0xFFFFFFFF);
        if (g_me_name[0]) {
            tw = p360_text_width(F_BOLD, 22, g_me_name);
            p360_text(F_BOLD, 1186 - tw, 42, 22, 0xFFFFFFFF, g_me_name);
        }
    }
    if (!sidebar) return;

    /* sidebar */
    for (i = 0; i < N_SIDE; i++) {
        float y = SIDE_Y0 + i * SIDE_STEP;
        int cur = g_side_focus && i == g_side_cur;
        int here = i == g_side_page || (i == 0 && g_side_page == SEARCH_PAGE);
        if (here) p360_rect(0, y, SIDE_W, SIDE_STEP - 1, C_SIDE_SEL);
        if (cur) p360_rect(0, y, SIDE_W, SIDE_STEP - 1, C_SIDE_CUR);
        if (here) p360_rect(SIDE_W - 6, y, 6, SIDE_STEP - 1, C_GREEN);
        p360_rect(0, y + SIDE_STEP - 1, SIDE_W, 1, C_LINE_DARK);
        p360_rect(0, y + SIDE_STEP, SIDE_W, 1, C_LINE_LITE);
        p360_image_draw(IMG_SIDE[i], 12, y + 16, 28, 28, cur ? 0xFFFFFFFF : 0xFFBBBBBB);
        p360_text(here ? F_BOLD : F_REG, 50, y + 16, 22, cur ? 0xFFFFFFFF : 0xFFE4E4E4, SIDE[i].label);
    }
    {                                                               /* the gear = Settings (cursor N_SIDE) */
        /* (owner's mockup 2026-11-22: a normal row right under Offline Player, label always shown) */
        int cur = g_side_focus && g_side_cur == N_SIDE, here = g_side_page == SETTINGS_PAGE;
        float y = SIDE_Y0 + N_SIDE * SIDE_STEP;
        if (here) p360_rect(0, y, SIDE_W, SIDE_STEP - 1, C_SIDE_SEL);
        if (cur) p360_rect(0, y, SIDE_W, SIDE_STEP - 1, C_SIDE_CUR);
        if (here) p360_rect(SIDE_W - 6, y, 6, SIDE_STEP - 1, C_GREEN);
        p360_rect(0, y + SIDE_STEP - 1, SIDE_W, 1, C_LINE_DARK);
        p360_rect(0, y + SIDE_STEP, SIDE_W, 1, C_LINE_LITE);
        p360_image_draw(IMG_GEAR, 12, y + 16, 28, 28, cur ? 0xFFFFFFFF : 0xFFBBBBBB);
        p360_text(here ? F_BOLD : F_REG, 50, y + 18, 20, cur ? 0xFFFFFFFF : 0xFFE4E4E4, "Settings");
    }
}

static void draw_info_panel(VIDEO *v)
{
    float x = 760, y = 130, ty;
    char line[160];
    p360_rect(x, y, 490, 560, 0xF0222222);
    p360_rect(x, y, 490, 4, C_GREEN);
    if (v->img) p360_image_draw(v->img, x + 24, y + 24, 442, 249, 0xFFFFFFFF);
    ty = y + 290;
    ty += p360_text_wrap(F_BOLD, x + 24, ty, 442, 24, 2, 0xFFFFFFFF, v->title) * p360_text_line(F_BOLD, 24) + 8;
    _snprintf(line, sizeof(line) - 1, "by %s", v->channel); line[sizeof(line) - 1] = 0;
    p360_text_fit(F_REG, x + 24, ty, 442, 20, 0xFF8FD460, line);
    _snprintf(line, sizeof(line) - 1, "%s  \xC2\xB7  %s  \xC2\xB7  %s", v->views, v->age, v->dur);
    line[sizeof(line) - 1] = 0;
    p360_text_fit(F_REG, x + 24, ty + 28, 442, 18, 0xFFBBBBBB, line);
    if (v->list[0]) {                               /* a YouTube playlist */
        p360_text(F_REG, x + 24, y + 486, 18, 0xFFCCCCCC, "X  Play all");
        p360_text(F_REG, x + 24, y + 520, 18, 0xFFCCCCCC, "A  Open the playlist        B  Close");
        return;
    }
    {
        int d = dl_find(v->id), o = offline_find(v->id);
        const char *t = own_id_shape(v->id) ? "Your own file (from the offline folder)"
                      : d >= 0 ? (g_dl[d].state == DL_FAILED ? "RB  Remove (saving failed)" : "RB  Cancel saving")
                      : o >= 0 ? "RB  Delete from this console" : "RB  Save to this console (watch offline)";
        if (d >= 0 && g_dl[d].state == DL_FAILED)
            p360_text_wrap(F_REG, x + 24, y + 384, 442, 16, 2, 0xFFFF8888, g_dl[d].err);
        if (!own_id_shape(v->id)) p360_text(F_REG, x + 24, y + 418, 18, 0xFFCCCCCC, "LB  Start a Mix (endless similar videos)");
        p360_text(F_REG, x + 24, y + 452, 18, 0xFFCCCCCC, t);
    }
    p360_text(F_REG, x + 24, y + 486, 18, 0xFFCCCCCC,
              list_has(0, v->id) >= 0 ? "X  Remove from Watch later" : "X  Watch later");
    p360_text(F_REG, x + 270, y + 486, 18, 0xFFCCCCCC, "Y  Playlists...");
    p360_text(F_REG, x + 24, y + 520, 18, 0xFFCCCCCC, "A  Play        B  Close");
}

static void draw_menu(void)
{
    int tx, ty;
    VIDEO *focused = NULL;
    p360_frame_begin(0xFFECECEC);
    p360_draw_safe(0);
    for (ty = 0; ty < 720; ty += 256)                               /* paper background */
        for (tx = 0; tx < 1280; tx += 256)
            p360_image_draw(IMG_PAPER, (float)tx, (float)ty, 256, 256, 0xFFFFFFFF);
    p360_draw_safe(1);
    EnterCriticalSection(&g_lock);
    if (g_side_page == CHAN_PAGE) {                                 /* a channel page: full width, no sidebar */
        draw_channel();
        draw_chrome(0);
    } else if (g_side_page == SEARCH_PAGE) {
        draw_search_page();
        draw_chrome(1);
    } else if (g_side_page == SETTINGS_PAGE) {
        draw_settings_page();
        draw_chrome(1);
    } else {
        if (g_side_page == PL_PAGE) draw_playlist_page();
        else draw_sections();
        if (ON_SUBS) draw_sub_column();
        if (ON_RESULTS) draw_kb_block();                            /* (sliding away / back) */
        draw_chrome(1);
        if (!g_side_focus && !(ON_SUBS && g_sub_focus) && g_focus_row < g_nsec && g_sec[g_focus_row].state == 2 && g_sec[g_focus_row].n)
            focused = &g_sec[g_focus_row].v[g_sec[g_focus_row].col];
        if (g_info && focused) {                /* hold A: the full details panel (playlists / own files: the old one) */
            if (focused->list[0] || own_id_shape(focused->id)) draw_info_panel(focused);
            else draw_md_panel(focused);
        }
    }
    pick_draw();                                                    /* "Save to..." (when open) */
    if (now_s() < g_mtoast_until) {                                 /* a short message (e.g. "sign-in later") */
        float w = p360_text_width(F_REG, 22, g_mtoast) + 48;
        p360_rect(640 - w / 2, 300, w, 54, 0xF0222222);
        p360_text(F_REG, 640 - w / 2 + 24, 314, 22, 0xFFFFFFFF, g_mtoast);
    }
    if (g_play_state == PS_LOADING || g_play_state == PS_FAILED) {
        p360_draw_safe(0);
        p360_rect(0, 0, 1280, 720, 0xB0000000);
        p360_draw_safe(1);
        p360_rect(340, 290, 600, 140, 0xF0222222);
        if (g_play_state == PS_LOADING) {
            spinner(390, 360, 18, 0xFFFFFFFF);
            p360_text(F_REG, 430, 318, 20, 0xFFAAAAAA, "Loading...");
            p360_text_wrap(F_BOLD, 430, 346, 490, 22, 2, 0xFFFFFFFF, g_play_title);
        } else {
            p360_text(F_BOLD, 370, 312, 22, 0xFFFF7777, "This video can't be played.");
            p360_text_wrap(F_REG, 370, 344, 550, 18, 2, 0xFFCCCCCC, g_play_msg);
            p360_text(F_REG, 370, 396, 18, 0xFF999999, "B  Back");
        }
    }
    LeaveCriticalSection(&g_lock);
    p360_frame_end();
}

/* ---------------------------------------------------------------- menu logic */
static void upload_thumbs(void)     /* textures can only be made on this thread: up to 4 per frame */
{
    int i, k, made = 0;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_nsec && made < 4; i++)
        for (k = 0; k < g_sec[i].n && made < 4; k++) {
            VIDEO *v = &g_sec[i].v[k];
            if (v->thumb == 2 && v->pix) {
                v->img = p360_image_create(v->pw, v->ph, v->pix);
                sbimage_free(v->pix);
                v->pix = NULL;
                v->thumb = 3;
                made++;
            }
        }
    LeaveCriticalSection(&g_lock);
}

static void start_play(VIDEO *v)    /* g_lock held */
{
    VIDEO copy = *v;                /* (v may point into g_pl.rel, which is about to be cleared) */
    if (v->list[0]) { open_playlist(&copy, 0); return; }   /* a playlist tile: its page (A on a video there plays) */
    g_play_seq++;
    memset(&g_pl, 0, sizeof(g_pl));
    g_pl.seq = g_play_seq;
    g_pl.v = copy;
    g_pl.v.img = NULL;              /* (the tile's picture belongs to the menu) */
    g_pl.v.pix = NULL;
    g_pl.v.thumb = 0;
    v = &g_pl.v;
    g_play_state = PS_LOADING;
    g_play_msg[0] = 0;
    g_play_seconds = v->seconds;
    copy_str(g_play_title, sizeof(g_play_title), v->title);
    g_paused_thumbs = 1;
    say("play %s \"%s\"", v->id, v->title);
    post_job(JOB_PLAY, v->id);
}

static void menu_input(void)
{
    int sel_play = 0;
    double t = now_s();
    check_keyboard();
    if (g_kb_open) return;
    EnterCriticalSection(&g_lock);
    if (g_play_state == PS_LOADING) {
        if (g_pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) {     /* back to the menu at once */
            g_play_seq++;
            g_play_job.type = JOB_NONE;
            g_play_state = PS_IDLE;
            g_paused_thumbs = 0;
            say("play: cancelled");
        }
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (g_play_state == PS_FAILED) {
        if (g_pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A)) { g_play_state = PS_IDLE; g_paused_thumbs = 0; }
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (pick_input()) { LeaveCriticalSection(&g_lock); return; }   /* the playlist picker is open: it has the pad */
    if (g_side_page == CHAN_PAGE) { channel_input(); LeaveCriticalSection(&g_lock); return; }
    if (g_side_page == SEARCH_PAGE && !g_side_focus) { search_input(); LeaveCriticalSection(&g_lock); return; }
    if (g_side_page == SETTINGS_PAGE && !g_side_focus) { settings_input(); LeaveCriticalSection(&g_lock); return; }
    if ((g_pressed & XINPUT_GAMEPAD_Y) && !g_info) { LeaveCriticalSection(&g_lock); open_search_page(); return; }  /* Y = search */
    if (g_side_page == 0 && !g_side_focus && !g_info && (g_pressed & XINPUT_GAMEPAD_B)) {   /* results: B = back to typing */
        LeaveCriticalSection(&g_lock);
        open_search_page();
        return;
    }
    if (g_side_page == 0 && !g_side_focus && !g_info && g_q[0] &&
        (g_pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))) {     /* results: LB / RB = filter */
        char q[Q_MAX];
        g_sfilter = (g_sfilter + N_SFILTERS + ((g_pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) ? -1 : 1)) % N_SFILTERS;
        copy_str(q, sizeof(q), g_q);
        LeaveCriticalSection(&g_lock);
        open_search(q);
        return;
    }

    if (g_info) {
        VIDEO *iv = g_focus_row < g_nsec && g_sec[g_focus_row].n ? &g_sec[g_focus_row].v[g_sec[g_focus_row].col] : NULL;
        g_md_scroll -= g_rsy * 14;              /* right stick: everything from the title down */
        if (iv && !strcmp(g_md.id, iv->id) && g_md.have_next && g_md.cm_token[0] && !g_md.cm_loading && g_md.ncm < 60 &&
            !g_cmt_job && (!g_md.cm_started || g_md_scroll >= g_md_h - (632 - 30) - 200)) {    /* comments: as the player */
            g_md.cm_started = 1;
            g_md.cm_loading = 1;
            g_cmt_job = g_md.gen;
            g_cmt_target = 1;
            copy_str(g_cmt_tok, sizeof(g_cmt_tok), g_md.cm_token);
            SetEvent(g_job_event);
        }
        if (g_pressed & XINPUT_GAMEPAD_B) g_info = 0;
        else if (g_pressed & XINPUT_GAMEPAD_A) { g_info = 0; sel_play = 1; g_a_used = 1; }
        else if ((g_pressed & XINPUT_GAMEPAD_X) && iv && iv->list[0]) {   /* a playlist: play it all */
            g_info = 0;
            open_playlist(iv, 1);           /* (g_lock is recursive) */
            LeaveCriticalSection(&g_lock);
            return;
        }
        else if ((g_pressed & XINPUT_GAMEPAD_X) && iv) watch_later_toggle(iv);
        else if ((g_pressed & XINPUT_GAMEPAD_Y) && iv && !iv->list[0]) pick_open(iv);
        else if ((g_pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) && iv) offline_toggle(iv);
        else if ((g_pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) && iv) {                     /* LB = start a Mix */
            if (iv->list[0] || own_id_shape(iv->id) || !strcmp(iv->dur, "LIVE")) note("Mixes are made from YouTube videos.");
            else if (g_mix_job) note("A Mix is being made already...");
            else {
                g_mix_job = 1;
                copy_str(g_mix_id, sizeof(g_mix_id), iv->id);
                SetEvent(g_job_event);
                g_info = 0;
                note("Making a Mix...");
            }
        }
    } else if (g_side_focus) {
        if (g_repeat & XINPUT_GAMEPAD_DPAD_UP) g_side_cur = (g_side_cur + N_SIDE) % (N_SIDE + 1);
        if (g_repeat & XINPUT_GAMEPAD_DPAD_DOWN) g_side_cur = (g_side_cur + 1) % (N_SIDE + 1);
        if (g_pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_RIGHT)) {
            if (g_side_cur == N_SIDE) {     /* the gear: Settings */
                g_side_focus = 0;
                if (g_side_page != SETTINGS_PAGE) { LeaveCriticalSection(&g_lock); open_settings(); return; }
                LeaveCriticalSection(&g_lock);
                return;
            }
            if (g_side_cur == 0) {          /* Search: the search page (or back into it) */
                g_side_focus = 0;
                if (g_side_page != SEARCH_PAGE) { LeaveCriticalSection(&g_lock); open_search_page(); return; }
                LeaveCriticalSection(&g_lock);
                return;
            }
            if (g_side_cur != g_side_page) { LeaveCriticalSection(&g_lock); open_page(g_side_cur); EnterCriticalSection(&g_lock); }
            g_side_focus = 0;
            if (g_side_cur == SUBS_PAGE) {                          /* Subscriptions: into the channel column first */
                g_sub_focus = 1;
                g_sub_cur = sub_view_row();
                g_sub_moved = -1;
            }
        }
    } else if (ON_SUBS && g_sub_focus) {    /* Subscriptions: the channel column */
        /* row 0 = All subscriptions, row k = g_subs[k - 1]. Resting on a row for a moment loads its videos; A / Right
           = load it now (if needed) and go into them; X = the channel page */
        int cols = g_nsubs + 1;
        const char *cid = g_sub_cur > 0 && g_sub_cur <= g_nsubs ? g_subs[g_sub_cur - 1].id : "";
        if (g_repeat & XINPUT_GAMEPAD_DPAD_UP) { g_sub_cur = (g_sub_cur + cols - 1) % cols; g_sub_moved = t; }
        if (g_repeat & XINPUT_GAMEPAD_DPAD_DOWN) { g_sub_cur = (g_sub_cur + 1) % cols; g_sub_moved = t; }
        cid = g_sub_cur > 0 && g_sub_cur <= g_nsubs ? g_subs[g_sub_cur - 1].id : "";
        if (g_pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_LEFT)) {
            g_sub_focus = 0;
            g_side_focus = 1;
            g_side_cur = SUBS_PAGE;
        } else if ((g_pressed & XINPUT_GAMEPAD_X) && cid[0]) {     /* its channel page */
            char id[40];
            copy_str(id, sizeof(id), cid);
            g_sub_focus = 0;
            LeaveCriticalSection(&g_lock);
            open_channel(id);
            return;
        } else if (g_pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_RIGHT)) {
            sub_show(cid);                                          /* (nothing reloads if it is on show already) */
            g_sub_moved = -1;
            if (g_nsec || !g_message[0]) g_sub_focus = 0;           /* into its videos (they may still be loading) */
        } else if (g_sub_moved >= 0 && t - g_sub_moved > 0.45) {
            sub_show(cid);                                          /* rested on it: its videos */
            g_sub_moved = -1;
        }
        g_a_down = -1;
    } else {
        SECTION *s = g_focus_row < g_nsec ? &g_sec[g_focus_row] : NULL;
        if ((g_pressed & XINPUT_GAMEPAD_X) && s && (!strcmp(s->filter, "@channel") ||          /* Subscriptions: X = */
            (!strncmp(s->filter, "@ch_", 4) && strcmp(s->filter, "@ch_lists")))) {            /*   the channel page */
            char id[40];
            copy_str(id, sizeof(id), s->query);
            LeaveCriticalSection(&g_lock);
            open_channel(id);
            return;
        }
        if ((g_pressed & XINPUT_GAMEPAD_X) && s && !strcmp(s->filter, "@list") && s->n) {   /* a playlist: play all */
            play_list(s, 0);
            LeaveCriticalSection(&g_lock);
            return;
        }
        if ((g_pressed & XINPUT_GAMEPAD_X) && g_side_page == PL_PAGE && g_nsec) {   /* playlist page: play it all */
            play_rows(0, 0);
            LeaveCriticalSection(&g_lock);
            return;
        }
        if ((g_pressed & XINPUT_GAMEPAD_X) && s && s->state == 2 && s->n && s->v[s->col].list[0]) {  /* a playlist tile */
            open_playlist(&s->v[s->col], 1);
            LeaveCriticalSection(&g_lock);
            return;
        }
        if ((g_pressed & XINPUT_GAMEPAD_B) && g_side_page == PL_PAGE) {    /* back to where the playlist was opened */
            LeaveCriticalSection(&g_lock);
            close_playlist();
            return;
        }
        if ((g_pressed & XINPUT_GAMEPAD_B) && ON_SUBS) {                         /* Subscriptions: back to the channels */
            g_sub_focus = 1; g_sub_cur = sub_view_row(); g_sub_moved = -1;
        }
        else if (g_pressed & XINPUT_GAMEPAD_B) { g_side_focus = 1; g_side_cur = g_side_page < N_SIDE ? g_side_page : 1; }
        if ((g_repeat & XINPUT_GAMEPAD_DPAD_UP) && g_focus_row == 0 && ON_RESULTS && !g_info) {  /* back to typing */
            LeaveCriticalSection(&g_lock);
            open_search_page();
            return;
        }
        if (g_side_page == PL_PAGE) pl_input(t);                 /* (one list, not rows) */
        else {
        if (g_repeat & XINPUT_GAMEPAD_DPAD_UP && g_focus_row > 0) g_focus_row--;
        if (g_repeat & XINPUT_GAMEPAD_DPAD_DOWN && g_focus_row + 1 < g_nsec) g_focus_row++;
        if (s && (g_repeat & XINPUT_GAMEPAD_DPAD_LEFT)) {
            if (s->col > 0) s->col--;
            else if ((g_pressed & XINPUT_GAMEPAD_DPAD_LEFT) && ON_SUBS) {             /* the channel column */
                g_sub_focus = 1; g_sub_cur = sub_view_row(); g_sub_moved = -1;
            }
            else if (g_pressed & XINPUT_GAMEPAD_DPAD_LEFT) {
                g_side_focus = 1;
                g_side_cur = g_side_page < N_SIDE ? g_side_page : 1;
            }
        }
        if (s && (g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) && s->col + 1 < s->n) s->col++;
        }
        /* A: short press plays, holding it opens the info panel */
        if (g_pressed & XINPUT_GAMEPAD_A) { g_a_down = t; g_a_used = 0; }
        if (g_a_down >= 0 && (g_held & XINPUT_GAMEPAD_A) && !g_a_used && t - g_a_down > 0.5) {
            g_info = 1;
            g_a_used = 1;
            if (s && s->state == 2 && s->n) md_open(&s->v[s->col]);
        }
        if (g_a_down >= 0 && !(g_held & XINPUT_GAMEPAD_A)) {
            if (!g_a_used) sel_play = 1;
            g_a_down = -1;
        }
    }
    if (!(g_held & XINPUT_GAMEPAD_A)) g_a_down = -1;
    if (sel_play && g_side_page == PL_PAGE && g_focus_row < g_nsec && g_sec[g_focus_row].n)
        play_rows(g_focus_row, g_sec[g_focus_row].col);     /* a playlist's video: on through the playlist from it */
    else if (sel_play && g_focus_row < g_nsec && g_sec[g_focus_row].state == 2 && g_sec[g_focus_row].n) {
        g_queue_n = 0; g_queue_mix = 0;                  /* a single video: no "Play all" list */
        start_play(&g_sec[g_focus_row].v[g_sec[g_focus_row].col]);
    }
    LeaveCriticalSection(&g_lock);
}

static void menu_animate(void)
{
    int i;
    float target;
    EnterCriticalSection(&g_lock);
    if (g_side_page == CHAN_PAGE) {     /* the whole channel page scrolls; the buttons row shows the banner */
        target = g_chan_row < 0 ? 0 : CH_SCROLL0 + g_focus_row * CH_STEP;
        g_sy += (target - g_sy) * 0.25f;
        for (i = 0; i < g_nsec; i++) {
            SECTION *s = &g_sec[i];
            if (s->col < s->first) s->first = s->col;
            if (s->col > s->first + 3) s->first = s->col - 3;
            s->sx += (s->first * CH_TILE_STEP - s->sx) * 0.25f;
        }
        LeaveCriticalSection(&g_lock);
        return;
    }
    {   /* Subscriptions: the channel column slides out while it (or the sidebar) has the focus, away for the videos */
        float want = ON_SUBS && (g_sub_focus || g_side_focus) ? 1.0f : 0.0f;
        g_subcol_t += (want - g_subcol_t) * 0.25f;
        if (fabs(want - g_subcol_t) < 0.005f) g_subcol_t = want;
        want = g_side_page == SEARCH_PAGE && g_sf != 2 ? 1.0f : 0.0f;   /* search keyboard: away for the results */
        g_kb_t += (want - g_kb_t) * 0.22f;
        if (fabs(want - g_kb_t) < 0.003f) g_kb_t = want;
    }
    g_pl_sy += (g_pl_top * PL_STEP - g_pl_sy) * 0.25f;           /* (playlist page list) */
    target = g_focus_row * ROW_STEP;
    if (target > 0 && g_focus_row == g_nsec - 1 && g_nsec > 1) target = (g_nsec - 1) * ROW_STEP;
    g_sy += (target - g_sy) * 0.25f;
    for (i = 0; i < g_nsec; i++) {
        SECTION *s = &g_sec[i];
        int shown = ON_SUBS && g_subcol_t > 0.5f ? 3 : 4;   /* (Subscriptions: the column leaves room for 3 tiles) */
        if (s->col < s->first) s->first = s->col;
        if (s->col > s->first + shown - 1) s->first = s->col - shown + 1;
        s->sx += (s->first * TILE_STEP - s->sx) * 0.25f;
    }
    LeaveCriticalSection(&g_lock);
}

/* ---------------------------------------------------------------- playing */
/* The player screen follows the owner's mockup (2012 YouTube player, design in the conversation 2026-11-14):
     top     title (bold) + "channel | views | age"
     bottom  time box, the red progress bar with its knob, a dark button strip (channel picture | previous, play,
             next | quality gear with an HD tag, comments), and a light "More Videos" strip under it.
   Controls (shown for 5 s after any button, always while paused or while something is chosen):
     on the bar      Left/Right = seek (a preview picture shows where; it jumps when you stop pressing, or on A),
                     A = pause/play, Down = buttons, Up = hide, B = cancel seeking / back to the menu
     on the buttons  Left/Right = choose, A = use it, Down = More Videos, Up / B = back to the bar
     More Videos     Left/Right = choose, A = play it, Up / B = back to the buttons
     BACK            back to the menu at any time */
#define BUFS 4
#define BUF_FRAMES 4096
static IXAudio2 *g_xa;
static short g_abuf[BUFS][BUF_FRAMES * 2];
static p360_image *IMG_KNOB, *IMG_PREV, *IMG_NEXT, *IMG_PLAY, *IMG_PAUSE, *IMG_COMMENT, *IMG_HD;

#define C_RED       0xFFC4161C      /* the 2012 player's red */

static void fmt_time(char *out, double s)
{
    int t = (int)(s < 0 ? 0 : s);
    if (t >= 3600) sprintf(out, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else sprintf(out, "%d:%02d", t / 60, t % 60);
}

/* ---- pictures for the player screen: preview sheets, the channel picture, "More Videos" thumbnails ----
   The screen asks with pimg_want(address) (-> slot number) and draws pimg_get(slot) once it has arrived. Two helper
   threads download + decode them (newest request first). pimg_reset() forgets everything when the player closes. */
#define N_PIMG 64
enum { PI_FREE, PI_WANTED, PI_LOADING, PI_PIXELS, PI_READY, PI_FAILED };
typedef struct { char url[1900]; int state, gen, w, h; unsigned int *pix; p360_image *img; double asked; } PIMG;
static PIMG g_pimg[N_PIMG];

static int pimg_want(const char *url)       /* main thread; -1 = no room / no address */
{
    int i, free_i = -1;
    if (!url || !url[0] || strlen(url) >= sizeof(g_pimg[0].url)) return -1;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < N_PIMG; i++) {
        if (g_pimg[i].state != PI_FREE && !strcmp(g_pimg[i].url, url)) {
            if (g_pimg[i].state == PI_WANTED) g_pimg[i].asked = now_s();    /* still wanted: keep it at the front */
            LeaveCriticalSection(&g_lock);
            return i;
        }
        if (g_pimg[i].state == PI_FREE && free_i < 0) free_i = i;
    }
    if (free_i >= 0) {
        PIMG *p = &g_pimg[free_i];
        strcpy(p->url, url);
        p->state = PI_WANTED;
        p->gen++;
        p->asked = now_s();
    }
    LeaveCriticalSection(&g_lock);
    return free_i;
}

static p360_image *pimg_get(int i)          /* main thread (textures can only be made here) */
{
    p360_image *img = NULL;
    if (i < 0) return NULL;
    EnterCriticalSection(&g_lock);
    if (g_pimg[i].state == PI_PIXELS) {
        g_pimg[i].img = p360_image_create(g_pimg[i].w, g_pimg[i].h, g_pimg[i].pix);
        sbimage_free(g_pimg[i].pix);
        g_pimg[i].pix = NULL;
        g_pimg[i].state = g_pimg[i].img ? PI_READY : PI_FAILED;
    }
    if (g_pimg[i].state == PI_READY) img = g_pimg[i].img;
    LeaveCriticalSection(&g_lock);
    return img;
}

static void pimg_reset(void)                /* main thread */
{
    int i;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < N_PIMG; i++) {
        PIMG *p = &g_pimg[i];
        if (p->img) { p360_image_free(p->img); p->img = NULL; }
        if (p->pix) { sbimage_free(p->pix); p->pix = NULL; }
        p->state = PI_FREE;
        p->gen++;                           /* a download still running for this slot is thrown away */
        p->url[0] = 0;
    }
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI pimg_thread(void *unused)
{
    char url[1900];
    (void)unused;
    for (;;) {
        int i, pick = -1, gen = 0, status, len, w, h;
        char *reply;
        unsigned int *px = NULL;
        EnterCriticalSection(&g_lock);
        for (i = 0; i < N_PIMG; i++)
            if (g_pimg[i].state == PI_WANTED && (pick < 0 || g_pimg[i].asked > g_pimg[pick].asked)) pick = i;
        if (pick >= 0) {
            g_pimg[pick].state = PI_LOADING;
            gen = g_pimg[pick].gen;
            strcpy(url, g_pimg[pick].url);
        }
        LeaveCriticalSection(&g_lock);
        if (pick < 0) { Sleep(30); continue; }
        if (!sbhttp_fetch("GET", url, NULL, NULL, 0, &status, &reply, &len)) {
            if (status == 200) px = sbimage_decode_jpeg((unsigned char *)reply, len, &w, &h);
            sbhttp_free(reply);
        }
        EnterCriticalSection(&g_lock);
        if (g_pimg[pick].gen == gen && g_pimg[pick].state == PI_LOADING) {
            if (px) { g_pimg[pick].pix = px; g_pimg[pick].w = w; g_pimg[pick].h = h; g_pimg[pick].state = PI_PIXELS; px = NULL; }
            else g_pimg[pick].state = PI_FAILED;
        }
        LeaveCriticalSection(&g_lock);
        if (px) sbimage_free(px);
    }
}

/* which preview sheet shows time t, and where its frame sits on that sheet (-1 = this video has none) */
static int sheet_for(double t, float *sx, float *sy)
{
    char url[1900];
    int i, per, sheet, cell;
    if (!g_pl.sb) return -1;
    i = (int)(t / g_pl.sb_interval);
    if (i >= g_pl.sb_count) i = g_pl.sb_count - 1;
    if (i < 0) i = 0;
    per = g_pl.sb_cols * g_pl.sb_rows;
    sheet = i / per;
    cell = i % per;
    *sx = (float)((cell % g_pl.sb_cols) * g_pl.sb_w);
    *sy = (float)((cell / g_pl.sb_cols) * g_pl.sb_h);
    if (g_pl.sb_numbered) _snprintf(url, sizeof(url) - 1, "%s%d%s", g_pl.sb_pre, sheet, g_pl.sb_post);
    else _snprintf(url, sizeof(url) - 1, "%s", g_pl.sb_pre);
    url[sizeof(url) - 1] = 0;
    return pimg_want(url);
}

static int thumb_for(const char *id)
{
    char url[128];
    _snprintf(url, sizeof(url) - 1, "https://i.ytimg.com/vi/%s/mqdefault.jpg", id);
    url[sizeof(url) - 1] = 0;
    return pimg_want(url);
}

/* ---- player screen state (main thread only) ---- */
enum { PF_BAR, PF_BUTTONS, PF_MORE, PF_QUEUE };  /* PF_QUEUE = the playing list's row (above More Videos) */
/* buttons: channel (-> channel page, later), previous, play, next, settings (gear), description (speech bubble) */
enum { PB_CHANNEL, PB_PREV, PB_PLAY, PB_NEXT, PB_SETTINGS, PB_DESCRIPTION, PB_COUNT };
/* panels: the description, the settings list and its three lists */
enum { PP_NONE, PP_DETAILS, PP_SETTINGS, PP_SPEED, PP_QUALITY, PP_CAPTIONS };
#define N_SPEEDS 8
static const float SPEEDS[N_SPEEDS] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f };
typedef struct {
    int focus, button, rel, rel_first, panel, qsel;     /* qsel = the row chosen in the open list */
    float rel_sx, slide;                /* (animated) More Videos scroll / how far the controls moved up */
    int qcur, qfirst;                   /* the list row: chosen tile (0 = the next video) / first one in view */
    float q_sx;
    int scrubbing, put_away;            /* put_away: B / Up hid the controls (stay hidden even while paused) */
    double scrub_t, scrub_last, overlay_until, upnext_at;
    double resume_t, resume_until;      /* "Resume watching?" (resume_until 0 = not asking) */
    char toast[140];
    double toast_until;
} PSTATE;
static PSTATE g_ps;
static const float BTN_X[PB_COUNT] = { 23, 532, 614, 702, 1098, 1178 };
#define QROW_H 250.0f                   /* the list row's height (header + tiles) */
static int queue_left(void) { return g_queue_n - g_queue_pos - 1; }   /* videos still to come in the list */

/* when the player closes, play this one next (More Videos / next / previous) */
static VIDEO g_chain;
static int g_chain_on;
static VIDEO g_hist[8];                 /* videos played before this one (for "previous") */
static int g_nhist;

static void toast(const char *text)
{
    copy_str(g_ps.toast, sizeof(g_ps.toast), text);
    g_ps.toast_until = now_s() + 3;
}

/* a band from one screen edge to the other (y, h in the layout's safe-area coordinates) */
static void band(float y, float h, unsigned int top, unsigned int bottom)
{
    p360_draw_safe(0);
    p360_rect_v(0, 36 + 0.9f * y, 1280, 0.9f * h, top, bottom);
    p360_draw_safe(1);
}

/* the description panel (owner's mockup 2026-11-22): a dark panel on the right, full height, red line on top, the
   video still playing on the left. Everything from the title down scrolls with the right stick; the button hints
   stay at the bottom. The end of the scroll is where comments will go (later update). Drawn in SCREEN pixels. */
static float g_det_scroll, g_det_h;

/* what the details panel shows: filled from the playing video (player) or the hold-A panel's video (menu) */
typedef struct {
    const VIDEO *v;
    int have_next, have_votes, nchap;
    double likes_n, dislikes_n;
    const char *published, *subs, *avatar, *likes, *desc, *cm_count, *cm_token;
    const COMMENT *cm;
    int ncm, cm_started, cm_loading;
} DINFO;

/* the details panel itself (shared by the player's description and the menu's hold-A panel). menu = 1: its hints
   say A plays, and the menu's things (no offline note for a playing file) */
static void draw_dinfo(const DINFO *d, float *scroll, float *height, int menu)
{
    char line[220];
    float px = 687, x = 709, w = 545, top = 30, bottom = 632, vis = bottom - top, ty, start, maxs;
    const VIDEO *v = d->v;
    p360_draw_safe(0);
    p360_rect(px, 0, 1280 - px, 720, menu ? 0xF4181818 : 0xE8181818);
    p360_rect(px, 0, 1280 - px, 4, C_RED);
    maxs = *height - vis > 0 ? *height - vis : 0;
    if (*scroll > maxs) *scroll = maxs;
    if (*scroll < 0) *scroll = 0;
    p360_clip(x - 4, top, 1280 - x, vis);
    ty = start = top + 5 - *scroll;
    ty += p360_text_wrap(F_BOLD, x, ty, 490, 28, 3, 0xFFFFFFFF, v->title) * p360_text_line(F_BOLD, 28) + 10;
    _snprintf(line, sizeof(line) - 1, "%s   |   %s   |   %s", v->channel, v->views, d->published[0] ? d->published : v->age);
    line[sizeof(line) - 1] = 0;
    p360_text_fit(F_REG, x, ty, 490, 18, 0xFFCCCCCC, line);
    ty += 45;
    if (!d->have_next) {
        spinner(x + 14, ty + 12, 12, 0xFFFFFFFF);
        p360_text(F_REG, x + 38, ty, 18, 0xFFCCCCCC, "Loading the details...");
        ty += 40;
    } else {
        p360_image *av = d->avatar[0] ? pimg_get(pimg_want(d->avatar)) : NULL;
        p360_image_draw(av ? av : IMG_AVATAR, x, ty, 58, 58, 0xFFFFFFFF);
        p360_text_fit(F_BOLD, x + 72, ty + 6, w - 72, 22, 0xFFFFFFFF, v->channel);
        p360_text_fit(F_REG, x + 72, ty + 36, w - 72, 16, 0xFFAAAAAA, d->subs);
        ty += 79;
        _snprintf(line, sizeof(line) - 1, "%s  \xC2\xB7  %s", v->views, d->published[0] ? d->published : v->age);
        line[sizeof(line) - 1] = 0;
        p360_text_fit(F_REG, x, ty, w, 17, 0xFFDDDDDD, line);
        ty += 29;
        if (d->have_votes) {                    /* likes green, dislikes red (Return YouTube Dislike's estimate) */
            double all = d->likes_n + d->dislikes_n;
            float bar = 270, part = all > 0 ? (float)(bar * d->likes_n / all) : bar;
            char lk[24], dk[24];
            fmt_count(lk, d->likes_n);
            fmt_count(dk, d->dislikes_n);
            p360_rect(x, ty, bar, 6, 0xFFCC2A2A);
            p360_rect(x, ty, part, 6, 0xFF4CAF2D);
            _snprintf(line, sizeof(line) - 1, "%s likes   %s dislikes", lk, dk);
            line[sizeof(line) - 1] = 0;
            p360_text(F_REG, x, ty + 13, 16, 0xFFBBBBBB, line);
            ty += 40;
        } else if (d->likes[0]) {               /* (no dislike estimate: YouTube's likes only, all green) */
            p360_rect(x, ty, 270, 6, 0xFF4CAF2D);
            _snprintf(line, sizeof(line) - 1, "%s likes", d->likes);
            line[sizeof(line) - 1] = 0;
            p360_text(F_REG, x, ty + 13, 16, 0xFFBBBBBB, line);
            ty += 40;
        }
        p360_rect(x, ty, 1260 - x, 1, 0xFF3A3A3A);
        ty += 12;
        ty += p360_text_wrap(F_REG, x, ty, w, 15, 0, 0xFFDDDDDD, d->desc[0] ? d->desc : "No description.") *
              p360_text_line(F_REG, 15);
        ty += 22;                               /* COMMENTS: below the description, in the same scroll */
        p360_rect(x, ty, 1260 - x, 1, 0xFF3A3A3A);
        ty += 12;
        p360_text(F_BOLD, x, ty, 20, 0xFFFFFFFF, "Comments");
        if (d->cm_count[0])
            p360_text(F_REG, x + p360_text_width(F_BOLD, 20, "Comments") + 12, ty + 4, 15, 0xFF888888, d->cm_count);
        ty += 38;
        {
            int k;
            for (k = 0; k < d->ncm; k++) {
                const COMMENT *c = &d->cm[k];
                int on = ty > top - 200 && ty < bottom;     /* (pictures only for comments on screen) */
                float aw = p360_text_width(F_BOLD, 15, c->author);
                p360_image *pic = on && c->avatar[0] ? pimg_get(pimg_want(c->avatar)) : NULL;
                p360_image_draw(pic ? pic : IMG_AVATAR, x, ty, 36, 36, 0xFFFFFFFF);
                p360_text_fit(F_BOLD, x + 48, ty, w - 48, 15, 0xFFFFFFFF, c->author);
                if (aw < w - 160) p360_text_fit(F_REG, x + 48 + aw + 10, ty + 1, w - 58 - aw, 13, 0xFF888888, c->age);
                ty += 21;
                ty += p360_text_wrap(F_REG, x + 48, ty, w - 48, 15, 8, 0xFFDDDDDD, c->text) * p360_text_line(F_REG, 15);
                if (c->replies)
                    _snprintf(line, sizeof(line) - 1, "%s likes   \xC2\xB7   %d %s", c->likes[0] ? c->likes : "0",
                              c->replies, c->replies == 1 ? "reply" : "replies");
                else _snprintf(line, sizeof(line) - 1, "%s likes", c->likes[0] ? c->likes : "0");
                line[sizeof(line) - 1] = 0;
                p360_text(F_REG, x + 48, ty + 3, 13, 0xFF888888, line);
                ty += 34;
            }
            if (d->cm_loading || (!d->cm_started && d->cm_token[0])) {
                spinner(x + 12, ty + 10, 9, 0xFFFFFFFF);
                p360_text(F_REG, x + 32, ty, 15, 0xFFAAAAAA, "Loading comments...");
                ty += 34;
            } else if (!d->ncm) {
                p360_text(F_REG, x, ty, 15, 0xFFAAAAAA, d->cm_started ? "No comments could be loaded." :
                          "Comments are turned off for this video.");
                ty += 34;
            } else if (d->cm_token[0] && d->ncm >= 60) {
                p360_text(F_REG, x, ty, 15, 0xFF888888, "Showing the first 60 comments.");
                ty += 34;
            } else if (d->cm_token[0]) {
                p360_text(F_REG, x, ty, 15, 0xFF888888, "Scroll down for more comments.");
                ty += 34;
            }
        }
    }
    p360_clip_off();
    *height = ty - start;
    if (maxs > 0) {                             /* where you are in it */
        float sh = vis * vis / *height;
        p360_rect(1264, top, 3, vis, 0x40FFFFFF);
        p360_rect(1264, top + (vis - sh) * *scroll / maxs, 3, sh, 0xB0FFFFFF);
    }
    /* button hints (fixed) */
    p360_text(F_REG, x, 642, 16, 0xFF888888, d->nchap ? "LB  Start a Mix      (in the video: LB / RB = chapters)" :
              "LB  Start a Mix (endless similar videos)");
    {
        int dl = dl_find(v->id);
        p360_text(F_REG, x, 665, 16, 0xFF888888,
                  !menu && g_pl_offline ? "Playing from this console's hard drive (Offline Player)."
                  : own_id_shape(v->id) ? "Your own file (from the offline folder)"
                  : dl >= 0 ? "RB  Cancel saving" : offline_find(v->id) >= 0 ? "RB  Delete from this console"
                  : "RB  Save to this console (watch offline)");
    }
    p360_text(F_REG, x, 690, 16, 0xFF888888, menu ? "A  Play    B  Close" : "A / B  Close");
    p360_text(F_REG, 858, 690, 16, 0xFF888888, list_has(0, v->id) >= 0 ? "X  Remove from Watch later" : "X  Watch later");
    p360_text(F_REG, 1092, 690, 16, 0xFF888888, "Y  Playlists...");
    p360_draw_safe(1);
}

static void draw_details(void)                  /* the player's description panel */
{
    DINFO d;
    memset(&d, 0, sizeof(d));
    d.v = &g_pl.v;
    d.have_next = g_pl.have_next; d.have_votes = g_pl.have_votes; d.nchap = g_pl.nchap;
    d.likes_n = g_pl.likes_n; d.dislikes_n = g_pl.dislikes_n;
    d.published = g_pl.published; d.subs = g_pl.subs; d.avatar = g_pl.avatar; d.likes = g_pl.likes; d.desc = g_pl.desc;
    d.cm_count = g_pl.cm_count; d.cm_token = g_pl.cm_token; d.cm = g_pl.cm;
    d.ncm = g_pl.ncm; d.cm_started = g_pl.cm_started; d.cm_loading = g_pl.cm_loading;
    draw_dinfo(&d, &g_det_scroll, &g_det_h, 0);
}

static void draw_md_panel(const VIDEO *v)       /* the menu's hold-A panel: the same panel for the chosen tile */
{
    DINFO d;
    int mine = !strcmp(g_md.id, v->id);
    memset(&d, 0, sizeof(d));
    d.v = v;
    d.have_next = mine && g_md.have_next; d.have_votes = mine && g_md.have_votes;
    d.likes_n = g_md.likes_n; d.dislikes_n = g_md.dislikes_n;
    d.published = mine ? g_md.published : ""; d.subs = mine ? g_md.subs : ""; d.avatar = mine ? g_md.avatar : "";
    d.likes = mine ? g_md.likes : ""; d.desc = mine ? g_md.desc : "";
    d.cm_count = mine ? g_md.cm_count : ""; d.cm_token = mine ? g_md.cm_token : ""; d.cm = g_md.cm;
    d.ncm = mine ? g_md.ncm : 0; d.cm_started = mine && g_md.cm_started; d.cm_loading = mine && g_md.cm_loading;
    draw_dinfo(&d, &g_md_scroll, &g_md_h, 1);
}

static void md_open(const VIDEO *v)             /* g_lock held: hold A opened the panel on v -> fetch its details */
{
    static int gens;
    g_md_scroll = 0;
    if (!v || v->list[0] || own_id_shape(v->id) || !strcmp(g_md.id, v->id)) return;   /* (same video: kept) */
    memset(&g_md, 0, sizeof(g_md));
    copy_str(g_md.id, sizeof(g_md.id), v->id);
    g_md.gen = ++gens;
    copy_str(g_md_job, sizeof(g_md_job), v->id);
    SetEvent(g_job_event);
}

#define STRIP_Y 534.0f                  /* top of the dark panel (the progress bar sits inside its top) */

static void speed_name(char *out, int n, float sp)
{
    if (sp > 0.99f && sp < 1.01f) copy_str(out, n, "Normal");
    else { _snprintf(out, n - 1, "%gx", sp); out[n - 1] = 0; }
}

static int speed_index(void)
{
    int k;
    for (k = 0; k < N_SPEEDS; k++) if (SPEEDS[k] > g_speed - 0.01f && SPEEDS[k] < g_speed + 0.01f) return k;
    return 3;
}

/* the text of row k of the open list: label (+ value on the right for the settings list) */
static int list_rows(int panel)
{
    switch (panel) {
    case PP_SETTINGS: return 6;                     /* speed, quality, captions, loop, shuffle, stats for nerds */
    case PP_SPEED: return N_SPEEDS;
    case PP_QUALITY: return g_pl.nq;
    case PP_CAPTIONS: return g_pl.ncap + 1;         /* "Off" + the tracks */
    }
    return 0;
}

static void list_row(int panel, int k, char *label, int ln, char *value, int vn, int *current)
{
    value[0] = 0;
    *current = 0;
    switch (panel) {
    case PP_SETTINGS:
        if (k == 0) { copy_str(label, ln, "Playback speed"); speed_name(value, vn, g_speed); }
        else if (k == 1) {
            copy_str(label, ln, "Quality");
            if (g_pl.nq) { _snprintf(value, vn - 1, "%dp", g_pl.q_height[g_pl.q]); value[vn - 1] = 0; }
        } else if (k == 3) {
            static const char *LOOP[3] = { "Off", "This video", "The whole list" };
            copy_str(label, ln, "Loop");
            copy_str(value, vn, LOOP[g_loop]);
        } else if (k == 4) {
            copy_str(label, ln, "Shuffle the list");
            copy_str(value, vn, g_queue_n > g_queue_pos + 1 ? "Shuffle now" : "No list playing");
        } else if (k == 5) {
            copy_str(label, ln, "Stats for nerds");
            copy_str(value, vn, g_stats ? "On" : "Off");
        } else {
            copy_str(label, ln, "Captions");
            if (g_cap_state == CS_OFF || g_cap_track >= g_pl.ncap) copy_str(value, vn, g_pl.ncap ? "Off" : "None");
            else if (g_cap_state == CS_LOADING) copy_str(value, vn, "Loading...");
            else if (g_cap_state == CS_FAILED) copy_str(value, vn, "Failed");
            else copy_str(value, vn, g_pl.cap_name[g_cap_track]);
        }
        break;
    case PP_SPEED:
        speed_name(label, ln, SPEEDS[k]);
        *current = k == speed_index();
        break;
    case PP_QUALITY:
        _snprintf(label, ln - 1, g_pl.q_height[k] >= 720 ? "%dp  HD" : "%dp", g_pl.q_height[k]);
        label[ln - 1] = 0;
        *current = k == g_pl.q;
        break;
    case PP_CAPTIONS:
        copy_str(label, ln, k == 0 ? "Off" : g_pl.cap_name[k - 1]);
        *current = k == 0 ? g_cap_state == CS_OFF : (g_cap_state != CS_OFF && g_cap_track == k - 1);
        break;
    }
}

/* the settings list / one of its lists: a box above the gear, the chosen row highlighted, a red dot = in use */
#define LIST_SHOW 9
static void draw_list(void)
{
    static const char *TITLES[] = { "", "", "Settings", "Playback speed", "Quality", "Captions" };
    int n = list_rows(g_ps.panel), first = 0, shown, k, cur;
    float row = 40, w = g_ps.panel == PP_SETTINGS ? 380.0f : 320.0f, x = 1250 - w, h, y;
    char label[80], value[80];
    shown = n < LIST_SHOW ? n : LIST_SHOW;
    if (g_ps.qsel > first + shown - 1) first = g_ps.qsel - shown + 1;
    if (first > n - shown) first = n - shown;
    if (first < 0) first = 0;
    h = shown * row + 58;
    y = STRIP_Y - g_ps.slide - 12 - h;
    p360_rect(x, y, w, h, 0xF21A1A1A);
    p360_frame_rect(x, y, w, h, 1, 0xFF3C3C3C);
    p360_text(F_BOLD, x + 18, y + 12, 20, 0xFFFFFFFF, TITLES[g_ps.panel]);
    p360_rect(x + 10, y + 42, w - 20, 1, 0xFF3C3C3C);
    for (k = first; k < first + shown; k++) {
        float ry = y + 50 + (k - first) * row;
        list_row(g_ps.panel, k, label, sizeof(label), value, sizeof(value), &cur);
        if (k == g_ps.qsel) p360_rect(x + 4, ry, w - 8, row, 0xFF3A3A3A);
        if (g_ps.panel == PP_SETTINGS) {
            float vw;
            copy_str(value + strlen(value), (int)(sizeof(value) - strlen(value)), "  \xC2\xBB");
            vw = p360_text_width(F_REG, 19, value);
            p360_text(F_REG, x + 18, ry + 9, 20, 0xFFFFFFFF, label);
            p360_text_fit(F_REG, x + w - 18 - (vw < 190 ? vw : 190), ry + 10, 190, 19, 0xFFAAAAAA, value);
        } else {
            if (cur) p360_rect(x + 16, ry + row / 2 - 5, 10, 10, C_RED);
            p360_text_fit(cur ? F_BOLD : F_REG, x + 38, ry + 9, w - 56, 20, 0xFFFFFFFF, label);
        }
    }
    if (first > 0) p360_rect(x + w / 2 - 20, y + 46, 40, 3, 0xFF777777);                 /* more above */
    if (first + shown < n) p360_rect(x + w / 2 - 20, y + h - 6, 40, 3, 0xFF777777);      /* more below */
}

/* the caption lines due at time t (up to 2 overlapping ones, like YouTube's rolling captions) */
static void draw_captions(double t, float bottom)
{
    int k, found[2], nf = 0, i;
    float y;
    if (g_cap_state != CS_READY || !g_cap_n) return;
    for (k = 0; k < g_cap_n && nf < 2; k++) {
        if (g_cap[k].start > t) break;
        if (t < g_cap[k].end) found[nf++] = k;
    }
    y = bottom - nf * 40;                       /* (two lines: the older one on top) */
    for (i = 0; i < nf; i++) {
        const char *s = g_cap_text + g_cap[found[i]].text;
        float w = p360_text_width(F_REG, 28, s), x;
        if (w > 1100) w = 1100;
        x = 640 - w / 2;
        p360_rect(x - 12, y + i * 40, w + 24, 40, 0xC8080808);
        p360_text_fit(F_REG, x, y + i * 40 + 5, 1100, 28, 0xFFFFFFFF, s);
    }
}

static void draw_player(double clock, double dur, int paused, int waiting)
{
    float s = g_ps.slide, pos, kx, ks;
    double shown_t = g_ps.scrubbing ? g_ps.scrub_t : clock;
    char t1[16], t2[16], line[300];
    int i;
    pos = dur > 0 ? (float)(shown_t / dur) : 0;
    if (g_play_info.live) pos = 1;                  /* live: the bar is always full (we're at "now") */
    if (pos < 0) pos = 0;
    if (pos > 1) pos = 1;
    kx = 1280 * pos;
    if (g_ps.panel == PP_DETAILS) {                 /* description open: only it over the playing video (owner) */
        draw_details();
        if (waiting) spinner(340, 300, 22, 0xFFFFFFFF);
        return;
    }

    /* top: title + channel | views | age */
    band(-40, 220, 0xD8000000, 0x00000000);
    p360_text_fit(F_BOLD, 24, 26, 1220, 34, 0xFFFFFFFF, g_pl.v.title);
    _snprintf(line, sizeof(line) - 1, "%s   |   %s   |   %s", g_pl.v.channel, g_pl.v.views, g_pl.v.age);
    line[sizeof(line) - 1] = 0;
    p360_text_fit(F_REG, 34, 84, 1200, 20, 0xFFCCCCCC, line);

    /* time box (on the picture) */
    band(430 - s, STRIP_Y - 430, 0x00000000, 0xB0000000);
    if (g_play_info.live) {                         /* live: a red LIVE box + how long we've been watching */
        fmt_time(t1, shown_t);
        _snprintf(line, sizeof(line) - 1, "watching %s", t1);
        line[sizeof(line) - 1] = 0;
        p360_rect(10, 492 - s, p360_text_width(F_BOLD, 20, "LIVE") + 20, 32, 0xF0CC1A1A);
        p360_text(F_BOLD, 20, 497 - s, 20, 0xFFFFFFFF, "LIVE");
        p360_rect(p360_text_width(F_BOLD, 20, "LIVE") + 36, 492 - s, p360_text_width(F_REG, 20, line) + 20, 32,
                  0xC0000000);
        p360_text(F_REG, p360_text_width(F_BOLD, 20, "LIVE") + 46, 497 - s, 20, 0xFFEEEEEE, line);
    } else {
        int ch = chapter_at(shown_t);
        fmt_time(t1, shown_t);
        fmt_time(t2, dur);
        if (ch >= 0) _snprintf(line, sizeof(line) - 1, "%s / %s   \xC2\xB7   %s", t1, t2, g_pl.chap_title[ch]);
        else _snprintf(line, sizeof(line) - 1, "%s / %s", t1, t2);
        line[sizeof(line) - 1] = 0;
        p360_rect(10, 492 - s, p360_text_width(F_REG, 20, line) + 20, 32, 0xC0000000);
        p360_text(F_REG, 20, 497 - s, 20, 0xFFEEEEEE, line);
    }

    /* the dark panel, with the progress bar inside its top edge (drawn after the panel = on top of it) */
    band(STRIP_Y - s, 645 - STRIP_Y, 0xFF303030, 0xFF121212);
    p360_rect(0, STRIP_Y + 9 - s, 1280, 14, 0xFF1A1A1A);
    p360_rect(0, STRIP_Y + 10 - s, 1280, 12, 0xFF5E5E5E);
    p360_rect_v(0, STRIP_Y + 10 - s, kx, 12, 0xFFE8262C, 0xFF9C0E12);
    for (i = 1; i < g_pl.nchap && dur > 0; i++)     /* chapters: small gaps in the bar, like YouTube's */
        p360_rect(1280 * (float)(g_pl.chap_t[i] / dur) - 1, STRIP_Y + 10 - s, 3, 12, 0xFF1A1A1A);
    ks = g_ps.focus == PF_BAR ? 36.0f : 28.0f;
    p360_image_draw(IMG_KNOB, kx - ks / 2, STRIP_Y + 16 - s - ks / 2, ks, ks, 0xFFFFFFFF);
    for (i = 0; i < PB_COUNT; i++) {
        float x = BTN_X[i], y = 583 - s, sz = 46;
        int focused = g_ps.focus == PF_BUTTONS && g_ps.button == i;
        unsigned int tint = focused ? 0xFFFFFFFF : 0xFF8E8E8E;
        p360_image *icon;
        if (i == PB_CHANNEL) { y = 578 - s; sz = 55; }
        if (focused) {
            p360_rect(x - 7, y - 7, sz + 14, sz + 14, 0x30FFFFFF);
            p360_frame_rect(x - 7, y - 7, sz + 14, sz + 14, 2, 0xFFFFFFFF);
        }
        switch (i) {
        case PB_CHANNEL:
            icon = g_pl.have_next ? pimg_get(pimg_want(g_pl.avatar)) : NULL;
            p360_image_draw(icon ? icon : IMG_AVATAR, x, y, sz, sz, 0xFFFFFFFF);
            continue;
        case PB_PREV: icon = IMG_PREV; break;
        case PB_PLAY: icon = paused ? IMG_PLAY : IMG_PAUSE; break;
        case PB_NEXT: icon = IMG_NEXT; break;
        case PB_SETTINGS: icon = IMG_GEAR; break;
        default: icon = IMG_COMMENT; break;
        }
        p360_image_draw(icon, x, y, sz, sz, tint);
        if (i == PB_SETTINGS && g_pl.nq && g_pl.q_height[g_pl.q] >= 720)
            p360_image_draw(IMG_HD, x + 24, y - 10, 33, 22, 0xFFFFFFFF);
    }

    /* the light "More Videos" strip: its tiles always peek in from the bottom edge, Down slides them fully up */
    band(645 - s, 140 + s, 0xFFEDEDED, 0xFFE0E0E0);
    if (queue_left() > 0) {                         /* the playing playlist / Mix: its next videos, above More Videos */
        char cnt[48];
        float nw;
        p360_text_fit(g_ps.focus == PF_QUEUE ? F_BOLD : F_REG, 23, 652 - s, 820, 26, 0xFF222222, g_queue_name);
        nw = p360_text_width(g_ps.focus == PF_QUEUE ? F_BOLD : F_REG, 26, g_queue_name);
        if (nw > 820) nw = 820;
        if (g_queue_mix) _snprintf(cnt, sizeof(cnt) - 1, "Mix  \xC2\xB7  keeps going");
        else _snprintf(cnt, sizeof(cnt) - 1, "video %d of %d", g_queue_pos + 1, g_queue_n);
        cnt[sizeof(cnt) - 1] = 0;
        p360_text(F_REG, 23 + nw + 16, 659 - s, 18, 0xFF777777, cnt);
        p360_clip(0, 684 - s, 1280, QROW_H - 40);
        for (i = 0; i < queue_left(); i++) {
            VIDEO v;
            float x = 23 + i * TILE_STEP - g_ps.q_sx;
            if (x < -TILE_STEP || x > 1280) continue;
            v = g_queue[g_queue_pos + 1 + i];
            v.img = pimg_get(thumb_for(v.id));
            draw_tile(&v, x, 694 - s, g_ps.focus == PF_QUEUE && i == g_ps.qcur);
        }
        p360_clip_off();
        s -= QROW_H;                                /* (More Videos sits one row lower) */
    }
    p360_text(g_ps.focus == PF_MORE ? F_BOLD : F_REG, 23, 652 - s, 26, 0xFF222222, "More Videos");
    {
        float ty = 694 - s;
        if (!g_pl.have_next) {
            spinner(200, 666 - s, 10, 0xFF666666);
        } else if (g_pl.nrel == 0)
            p360_text(F_REG, 200, 656 - s, 20, C_SECTION, "No more videos found.");
        p360_clip(0, 684 - s, 1280, 120 + s);
        for (i = 0; i < g_pl.nrel; i++) {
            VIDEO v;
            float x = 23 + i * TILE_STEP - g_ps.rel_sx;
            if (x < -TILE_STEP || x > 1280) continue;
            v = g_pl.rel[i];
            v.img = pimg_get(thumb_for(v.id));
            draw_tile(&v, x, ty, g_ps.focus == PF_MORE && i == g_ps.rel);
        }
        p360_clip_off();
    }
    s = g_ps.slide;                                 /* (back from "one row lower") */

    /* seeking: the preview picture above the knob (mockup: red frame with a point) */
    if (g_ps.scrubbing) {
        float bx = kx - 150, by = STRIP_Y - 190 - s, sx = 0, sy = 0, px = kx, r;
        p360_image *sheet = NULL;
        if (bx < 0) bx = 0;
        if (bx > 980) bx = 980;
        if (px < bx + 20) px = bx + 20;
        if (px > bx + 280) px = bx + 280;
        if (g_pl.sb) sheet = pimg_get(sheet_for(g_ps.scrub_t, &sx, &sy));
        p360_rect(bx, by, 300, 174, C_RED);
        p360_rect(bx + 6, by + 6, 288, 162, 0xFF000000);
        if (sheet) p360_image_draw_part(sheet, sx, sy, (float)g_pl.sb_w, (float)g_pl.sb_h, bx + 6, by + 6, 288, 162,
                                        0xFFFFFFFF);
        else if (g_pl.sb) spinner(bx + 150, by + 87, 14, 0xFFFFFFFF);
        fmt_time(t1, g_ps.scrub_t);
        if (chapter_at(g_ps.scrub_t) >= 0) {        /* the chapter you'd land in */
            p360_rect(bx + 6, by + 6, 288, 28, 0xC0000000);
            p360_text_fit(F_BOLD, bx + 14, by + 10, 272, 17, 0xFFFFFFFF, g_pl.chap_title[chapter_at(g_ps.scrub_t)]);
        }
        p360_rect(bx + 150 - p360_text_width(F_BOLD, 20, t1) / 2 - 8, by + 136, p360_text_width(F_BOLD, 20, t1) + 16,
                  28, 0xC0000000);
        p360_text(F_BOLD, bx + 150 - p360_text_width(F_BOLD, 20, t1) / 2, by + 139, 20, 0xFFFFFFFF, t1);
        for (r = 0; r < 14; r++) p360_rect(px - 15 + r, by + 174 + r, 30 - 2 * r, 1, C_RED);
    }

    if (g_stats) {                                  /* stats for nerds */
        char st[6][120];
        int shown = 0, dropped = 0, k2;
        sbplayer_stats(&shown, &dropped);
        _snprintf(st[0], 119, "Video  %d x %d  (%dp quality)", g_play_info.width, g_play_info.height,
                  g_pl.nq ? g_pl.q_height[g_pl.q] : g_play_info.height);
        _snprintf(st[1], 119, "Codecs  H.264  /  %s", g_play_info.has_audio ? "sound decoded to 16-bit PCM" : "no sound");
        _snprintf(st[2], 119, "Sound  %d Hz, %d channel%s", g_play_info.sample_rate, g_play_info.channels,
                  g_play_info.channels == 1 ? "" : "s");
        _snprintf(st[3], 119, "Pictures  %d shown, %d dropped (too late)", shown, dropped);
        _snprintf(st[4], 119, "Source  %s", g_play_info.live ? "live stream (HLS)" : own_id_shape(g_pl.v.id) ? "your own file"
                  : g_pl_offline ? "this console's hard drive" : "streamed from YouTube");
        _snprintf(st[5], 119, "Speed  %gx    Loop  %s    Chapters  %d    Video id  %s", g_speed,
                  g_loop == 0 ? "off" : g_loop == 1 ? "video" : "list", g_pl.nchap, g_pl.v.id);
        p360_rect(24, 120, 560, 6 * 24 + 20, 0xD0000000);
        for (k2 = 0; k2 < 6; k2++) { st[k2][119] = 0; p360_text_fit(F_REG, 36, 130 + k2 * 24, 540, 17, 0xFFE0E0E0, st[k2]); }
    }
    if (g_ps.panel == PP_DETAILS) draw_details();
    if (g_ps.panel >= PP_SETTINGS) draw_list();
    if (waiting) spinner(640, 300, 22, 0xFFFFFFFF);
}

/* the video after this one: the next of a "Play all" list, else the first of More Videos (NULL = none yet) */
static VIDEO *up_next(void)
{
    if (g_queue_pos + 1 < g_queue_n) return &g_queue[g_queue_pos + 1];
    if (g_loop == 2 && g_queue_n > 0) return &g_queue[0];          /* loop the whole list: back to its start */
    return g_pl.nrel > 0 ? &g_pl.rel[0] : NULL;
}

static void draw_upnext(double left)
{
    char line[64];
    VIDEO v = *up_next();
    float x = 820, y = 300;
    p360_rect(x, y, 440, 190, 0xF2181818);
    p360_rect(x, y, 440, 4, C_RED);
    _snprintf(line, sizeof(line) - 1, "Up next in %d", (int)(left + 0.99));
    line[sizeof(line) - 1] = 0;
    p360_text(F_REG, x + 20, y + 16, 18, 0xFFAAAAAA, line);
    v.img = pimg_get(thumb_for(v.id));
    if (v.img) p360_image_draw(v.img, x + 20, y + 46, 160, 90, 0xFFFFFFFF);
    else p360_rect(x + 20, y + 46, 160, 90, 0xFF333333);
    p360_text_wrap(F_BOLD, x + 196, y + 46, 226, 19, 3, 0xFFFFFFFF, v.title);
    p360_text_fit(F_REG, x + 196, y + 116, 226, 16, 0xFFAAAAAA, v.channel);
    p360_text(F_REG, x + 20, y + 152, 18, 0xFFCCCCCC, "A  Play now      B  Stop");
}

static IXAudio2SourceVoice *make_voice(sbplayer_info *info, int start)
{
    IXAudio2SourceVoice *v = NULL;
    WAVEFORMATEX wf;
    if (!info->has_audio || !g_xa) return NULL;
    ZeroMemory(&wf, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)info->channels;
    wf.nSamplesPerSec = info->sample_rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(info->channels * 2);
    wf.nAvgBytesPerSec = info->sample_rate * wf.nBlockAlign;
    if (FAILED(IXAudio2_CreateSourceVoice(g_xa, &v, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL)))
        return NULL;
    if (g_speed < 0.99f || g_speed > 1.01f) IXAudio2SourceVoice_SetFrequencyRatio(v, g_speed, XAUDIO2_COMMIT_NOW);
    if (start) IXAudio2SourceVoice_Start(v, 0, XAUDIO2_COMMIT_NOW);
    return v;
}

static void kill_voice(IXAudio2SourceVoice **v)
{
    if (!*v) return;
    IXAudio2SourceVoice_Stop(*v, 0, XAUDIO2_COMMIT_NOW);
    IXAudio2SourceVoice_DestroyVoice(*v);
    *v = NULL;
}

/* captions: track k of this video (-1 = off). remember = keep the choice for the next videos */
static void caption_choose(int k, int remember)
{
    EnterCriticalSection(&g_lock);
    g_cap_req++;
    caption_free();
    if (k < 0 || k >= g_pl.ncap) {
        g_cap_state = CS_OFF;
        if (remember) g_cap_pref[0] = 0;
    } else {
        g_cap_state = CS_LOADING;
        g_cap_track = k;
        if (remember) copy_str(g_cap_pref, sizeof(g_cap_pref), g_pl.cap_name[k]);
        g_cap_job = g_cap_req;
        strcpy(g_cap_job_url, g_pl.cap_url[k]);
        SetEvent(g_job_event);
    }
    LeaveCriticalSection(&g_lock);
}

/* leave the player for another video (it starts once the player has closed) */
static void chain_to(const VIDEO *v, int remember_this_one)
{
    if (remember_this_one) {
        if (g_nhist == 8) { memmove(g_hist, g_hist + 1, 7 * sizeof(VIDEO)); g_nhist = 7; }
        g_hist[g_nhist++] = g_pl.v;
    }
    g_chain = *v;
    g_chain.img = NULL;
    g_chain.pix = NULL;
    g_chain_on = 1;
}

static void go_next(void)               /* g_lock held: on to up_next() (moving along a "Play all" list) */
{
    if (g_queue_pos + 1 < g_queue_n) { g_queue_pos++; chain_to(&g_queue[g_queue_pos], 1); }
    else if (g_loop == 2 && g_queue_n > 0) { g_queue_pos = 0; chain_to(&g_queue[0], 1); }
    else if (g_pl.nrel > 0) { g_queue_n = 0; g_queue_mix = 0; chain_to(&g_pl.rel[0], 1); }
    if (g_queue_mix && g_queue_n && g_queue_pos >= g_queue_n - 3 && !g_mix_job) {  /* a Mix running low: top it up */
        g_mix_job = 2;
        copy_str(g_mix_id, sizeof(g_mix_id), g_queue[g_queue_n - 1].id);
        SetEvent(g_job_event);
    }
}

static void play_loop(void)
{
    sbplayer_info *info = &g_play_info;
    IXAudio2SourceVoice *voice = NULL;
    XAUDIO2_VOICE_STATE st;
    sbdec_frame fr;
    double t_base = 0, wall_start = now_s(), paused_at = 0, paused_total = 0, clock = 0, pts, dur, seek_to = -1;
    int next_buf = 0, paused = 0, waiting = 0, shown, dropped, quit = 0, finished = 0, new_q = -1, k, cap = -1, hide;
    float new_speed = 0;
    double last_save = now_s();
    int resume, live_restart = 0;
    float speed_before = g_speed;
    double last_live = now_s() - 8;     /* first live refresh ~7 s in: the starting address was fetched before the
                                           video opened, and it expires ~40 s after that */

    memset(&g_ps, 0, sizeof(g_ps));
    EnterCriticalSection(&g_lock);
    g_live_fresh = 0;
    LeaveCriticalSection(&g_lock);
    g_ps.focus = PF_BUTTONS;                            /* the controls always open on Play/Pause */
    g_ps.button = PB_PLAY;
    g_ps.overlay_until = now_s() + 5;
    st.BuffersQueued = 0;
    if (!g_set.keep_speed) g_speed = 1.0f;              /* Settings: every video starts at normal speed */
    speed_before = g_speed;
    if (info->live) g_speed = 1.0f;                     /* live: always normal speed (put back afterwards) */
    for (k = 0; k < g_pl.ncap && g_cap_pref[0] && g_set.captions; k++)  /* captions on last time: same language */
        if (!strcmp(g_pl.cap_name[k], g_cap_pref)) { cap = k; break; }
    caption_choose(cap, 0);
    EnterCriticalSection(&g_lock);
    resume = watch_resume(g_pl.v.id);
    LeaveCriticalSection(&g_lock);
    if (resume && (g_set.resume == 2 || info->live)) resume = 0;   /* Settings: always from the start; live: now */
    if (resume && g_set.resume == 1) {                  /* Settings: always resume, no question */
        seek_to = resume;
        say("resume at %d s (setting)", resume);
        resume = 0;
    }
    if (resume) {                                       /* watched part of it before: ask (it waits paused) */
        paused = 1;
        paused_at = now_s();
        g_ps.resume_t = resume;
        g_ps.resume_until = now_s() + 8;
    }
    voice = make_voice(info, !paused);
    for (;;) {
        double now = now_s();
        int visible, any;
        read_pad();
        any = g_pressed;
        dur = g_play_seconds > 0 ? g_play_seconds : info->duration;
        /* (paused keeps the controls up, unless they were put away with B / Up) */
        visible = (paused && !g_ps.put_away) || now < g_ps.overlay_until || g_ps.focus == PF_MORE || g_ps.focus == PF_QUEUE || g_ps.panel ||
                  g_ps.scrubbing || waiting || g_ps.upnext_at > 0;
        if (!visible) { g_ps.focus = PF_BUTTONS; g_ps.button = PB_PLAY; }   /* hidden: next time back on Play */
        hide = 0;

        /* ---- LB / RB: previous / next chapter (no chapters: 10 s back / ahead), whenever no panel is open ---- */
        if ((g_pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) && !g_pick &&
            g_ps.resume_until <= 0 && g_ps.upnext_at <= 0 && g_ps.panel == PP_NONE && !waiting) {
            int fwd = (g_pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0, ch = chapter_at(clock);
            double target = -1;
            char msg[96];
            msg[0] = 0;
            if (info->live) copy_str(msg, sizeof(msg), "This is a live stream: it can't skip back or ahead.");
            else if (ch >= 0) {
                if (fwd && ch + 1 < g_pl.nchap) target = g_pl.chap_t[ch + 1], ch++;
                else if (fwd) copy_str(msg, sizeof(msg), "That's the last chapter.");
                else if (clock - g_pl.chap_t[ch] > 3 || ch == 0) target = g_pl.chap_t[ch];   /* (a few s in: its start) */
                else target = g_pl.chap_t[ch - 1], ch--;
                if (target >= 0) { _snprintf(msg, sizeof(msg) - 1, "Chapter %d: %s", ch + 1, g_pl.chap_title[ch]); msg[sizeof(msg) - 1] = 0; }
            } else {
                target = clock + (fwd ? 10 : -10);
                if (target < 0) target = 0;
                if (dur > 1 && target > dur - 1) target = dur - 1;
                copy_str(msg, sizeof(msg), fwd ? "10 seconds ahead" : "10 seconds back");
            }
            if (target >= 0) seek_to = target;
            if (msg[0]) toast(msg);
            g_pressed &= ~(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
        }

        /* ---- controller ---- */
        check_keyboard();                           /* (a new playlist's name from the Xbox keyboard) */
        if (g_pressed & XINPUT_GAMEPAD_BACK) { quit = 1; g_pick = 0; }
        else if (g_pick) {                          /* "Save to..." is open: it has the pad */
            EnterCriticalSection(&g_lock);
            pick_input();
            LeaveCriticalSection(&g_lock);
            g_ps.overlay_until = now + 5;
        } else if (g_ps.resume_until > 0) {         /* "Resume watching?": A (or waiting 8 s) = resume, B = start over */
            if ((g_pressed & XINPUT_GAMEPAD_A) || now > g_ps.resume_until) {
                paused = 0;
                seek_to = g_ps.resume_t;
                say("resume at %.0f s", g_ps.resume_t);
                g_ps.resume_until = 0;
            } else if (g_pressed & XINPUT_GAMEPAD_B) {
                paused = 0;
                seek_to = -2;                           /* (= play from here, the start) */
                g_ps.resume_until = 0;
            }
            g_ps.overlay_until = now + 3;
        } else if (g_ps.upnext_at > 0) {                                 /* the video ended: "Up next" */
            if (g_pressed & XINPUT_GAMEPAD_A) { EnterCriticalSection(&g_lock); go_next(); LeaveCriticalSection(&g_lock); quit = 1; }
            else if (g_pressed & XINPUT_GAMEPAD_B) quit = 1;
        } else if (!visible) {              /* controls hidden: any button just shows them (on Play); B leaves */
            if (g_pressed & XINPUT_GAMEPAD_B) quit = 1;
            else if (any) { g_ps.overlay_until = now + 5; g_ps.put_away = 0; }
        } else {
            if (any) g_ps.overlay_until = now + 5;
            if (g_ps.panel == PP_DETAILS) {
                g_det_scroll -= g_rsy * 14;         /* right stick: everything from the title down */
                EnterCriticalSection(&g_lock);      /* comments: the first 20 when it opens, 20 more near the bottom */
                if (g_pl.have_next && g_pl.cm_token[0] && !g_pl.cm_loading && g_pl.ncm < 60 && !g_cmt_job &&
                    (!g_pl.cm_started || g_det_scroll >= g_det_h - (632 - 30) - 200)) {
                    g_pl.cm_started = 1;
                    g_pl.cm_loading = 1;
                    g_cmt_job = g_pl.seq;
                    g_cmt_target = 0;
                    copy_str(g_cmt_tok, sizeof(g_cmt_tok), g_pl.cm_token);
                    SetEvent(g_job_event);
                }
                LeaveCriticalSection(&g_lock);
                if (g_pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B)) g_ps.panel = PP_NONE;
                else if (g_pressed & (XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y)) {
                    EnterCriticalSection(&g_lock);
                    if (g_pressed & XINPUT_GAMEPAD_X) watch_later_toggle(&g_pl.v);
                    else pick_open(&g_pl.v);
                    LeaveCriticalSection(&g_lock);
                } else if (g_pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) {
                    EnterCriticalSection(&g_lock);
                    if (g_pl_offline) note("It's playing from the hard drive now: delete it from the Offline Player afterwards.");
                    else offline_toggle(&g_pl.v);
                    LeaveCriticalSection(&g_lock);
                } else if (g_pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) {          /* LB = a Mix from this video */
                    EnterCriticalSection(&g_lock);
                    if (own_id_shape(g_pl.v.id) || info->live) note("Mixes are made from YouTube videos.");
                    else if (g_mix_job) note("A Mix is being made already...");
                    else {
                        g_mix_job = 3;
                        copy_str(g_mix_id, sizeof(g_mix_id), g_pl.v.id);
                        SetEvent(g_job_event);
                        note("Making a Mix from this video...");
                    }
                    LeaveCriticalSection(&g_lock);
                }
            } else if (g_ps.panel >= PP_SETTINGS) {                    /* the settings list or one of its lists */
                int n = list_rows(g_ps.panel);
                if ((g_repeat & XINPUT_GAMEPAD_DPAD_UP) && g_ps.qsel > 0) g_ps.qsel--;
                if ((g_repeat & XINPUT_GAMEPAD_DPAD_DOWN) && g_ps.qsel < n - 1) g_ps.qsel++;
                if (g_pressed & XINPUT_GAMEPAD_B) {                     /* back one level */
                    if (g_ps.panel == PP_SETTINGS) g_ps.panel = PP_NONE;
                    else { g_ps.qsel = g_ps.panel - PP_SPEED; g_ps.panel = PP_SETTINGS; }
                } else if (g_ps.panel == PP_SETTINGS && g_ps.qsel == 3 &&
                           (g_pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT))) {
                    g_loop = (g_loop + ((g_pressed & XINPUT_GAMEPAD_DPAD_LEFT) ? 2 : 1)) % 3;   /* Left / Right too */
                } else if (g_pressed & XINPUT_GAMEPAD_A) switch (g_ps.panel) {
                case PP_SETTINGS:
                    if (g_ps.qsel == 0) { g_ps.panel = PP_SPEED; g_ps.qsel = speed_index(); }
                    else if (g_ps.qsel == 1) {
                        if (g_pl.nq > 1) { g_ps.panel = PP_QUALITY; g_ps.qsel = g_pl.q; }
                        else toast("This video has only one quality.");
                    } else if (g_ps.qsel == 3) {
                        g_loop = (g_loop + 1) % 3;                  /* (the list stays open: see the value change) */
                        if (g_loop == 2 && g_queue_n == 0) toast("No list is playing: \"The whole list\" works with Play all and Mixes.");
                    } else if (g_ps.qsel == 4) {
                        EnterCriticalSection(&g_lock);
                        if (g_queue_n > g_queue_pos + 2) {          /* the videos still to come, in random order */
                            int a, b;
                            VIDEO tmp;
                            for (a = g_queue_n - 1; a > g_queue_pos + 1; a--) {
                                b = g_queue_pos + 1 + rand() % (a - g_queue_pos);
                                tmp = g_queue[a]; g_queue[a] = g_queue[b]; g_queue[b] = tmp;
                            }
                            toast("The rest of the list is shuffled.");
                        } else toast("No list is playing (Play all or a Mix).");
                        LeaveCriticalSection(&g_lock);
                    } else if (g_ps.qsel == 5) {
                        g_stats = !g_stats;
                    } else {
                        if (g_pl.ncap) {
                            g_ps.panel = PP_CAPTIONS;
                            g_ps.qsel = g_cap_state == CS_OFF ? 0 : g_cap_track + 1;
                        } else toast("This video has no captions.");
                    }
                    break;
                case PP_SPEED:
                    if (info->live) toast("Live streams play at normal speed.");
                    else new_speed = SPEEDS[g_ps.qsel];
                    g_ps.panel = PP_NONE;
                    break;
                case PP_QUALITY: if (g_ps.qsel != g_pl.q) new_q = g_ps.qsel; g_ps.panel = PP_NONE; break;
                case PP_CAPTIONS: caption_choose(g_ps.qsel - 1, 1); g_ps.panel = PP_NONE; break;
                }
            } else if (g_ps.focus == PF_BAR) {
                if ((g_pressed & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) && info->live)
                    toast("This is a live stream: it can't skip back or ahead.");
                else if (g_repeat & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) {
                    double step = now - g_dir_since > 1.5 ? 30 : 10;    /* held: bigger steps */
                    if (!g_ps.scrubbing) { g_ps.scrubbing = 1; g_ps.scrub_t = clock; }
                    g_ps.scrub_t += (g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) ? -step : step;
                    g_ps.scrub_last = now;
                }
                if (g_pressed & XINPUT_GAMEPAD_A) {
                    if (g_ps.scrubbing) g_ps.scrub_last = 0;             /* jump now */
                    else { paused = !paused; seek_to = -2; }
                }
                if (g_pressed & XINPUT_GAMEPAD_B) { if (g_ps.scrubbing) g_ps.scrubbing = 0; else hide = 1; }
                if (g_pressed & XINPUT_GAMEPAD_DPAD_DOWN) { if (g_ps.scrubbing) g_ps.scrub_last = 0; g_ps.focus = PF_BUTTONS; }
                if (g_pressed & XINPUT_GAMEPAD_DPAD_UP) { g_ps.scrubbing = 0; hide = 1; }
            } else if (g_ps.focus == PF_BUTTONS) {
                if ((g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) && g_ps.button > 0) g_ps.button--;
                if ((g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) && g_ps.button < PB_COUNT - 1) g_ps.button++;
                if (g_pressed & XINPUT_GAMEPAD_DPAD_UP) g_ps.focus = PF_BAR;
                if (g_pressed & XINPUT_GAMEPAD_B) hide = 1;             /* B = put the controls away */
                if (g_pressed & XINPUT_GAMEPAD_DPAD_DOWN) g_ps.focus = queue_left() > 0 ? PF_QUEUE : PF_MORE;
                if (g_pressed & XINPUT_GAMEPAD_A) switch (g_ps.button) {
                case PB_CHANNEL:                                    /* the channel page (opens once we've closed) */
                    if (g_pl.channel_id[0]) { copy_str(g_open_channel, sizeof(g_open_channel), g_pl.channel_id); quit = 1; }
                    else toast(g_pl.have_next ? "This video's channel isn't known." : "Still loading the channel...");
                    break;
                case PB_PREV:
                    if (clock > 5 || g_nhist == 0) seek_to = 0;
                    else { g_nhist--; chain_to(&g_hist[g_nhist], 0); quit = 1; }
                    break;
                case PB_PLAY: paused = !paused; seek_to = -2; break;
                case PB_NEXT:
                    EnterCriticalSection(&g_lock);
                    if (up_next()) { go_next(); quit = 1; }
                    LeaveCriticalSection(&g_lock);
                    if (!quit) toast(g_pl.have_next ? "There's no next video." : "Still finding the next video...");
                    break;
                case PB_SETTINGS: g_ps.panel = PP_SETTINGS; g_ps.qsel = 0; break;
                default: g_ps.panel = PP_DETAILS; g_det_scroll = 0; break;  /* the description */
                }
            } else if (g_ps.focus == PF_QUEUE) {                    /* the playing list's row */
                int left = queue_left();
                if (left <= 0) g_ps.focus = PF_MORE;
                if ((g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) && g_ps.qcur > 0) g_ps.qcur--;
                if ((g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) && g_ps.qcur < left - 1) g_ps.qcur++;
                if (g_pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_B)) g_ps.focus = PF_BUTTONS;
                if (g_pressed & XINPUT_GAMEPAD_DPAD_DOWN) g_ps.focus = PF_MORE;
                if ((g_pressed & XINPUT_GAMEPAD_A) && g_ps.qcur < left) {     /* jump there, the list goes on after it */
                    EnterCriticalSection(&g_lock);
                    g_queue_pos += 1 + g_ps.qcur;
                    chain_to(&g_queue[g_queue_pos], 1);
                    if (g_queue_mix && g_queue_pos >= g_queue_n - 3 && !g_mix_job) {   /* (a Mix tops itself up) */
                        g_mix_job = 2;
                        copy_str(g_mix_id, sizeof(g_mix_id), g_queue[g_queue_n - 1].id);
                        SetEvent(g_job_event);
                    }
                    LeaveCriticalSection(&g_lock);
                    quit = 1;
                }
            } else {                                                /* More Videos */
                if ((g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) && g_ps.rel > 0) g_ps.rel--;
                if ((g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) && g_ps.rel < g_pl.nrel - 1) g_ps.rel++;
                if (g_pressed & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_B)) g_ps.focus = queue_left() > 0 ? PF_QUEUE : PF_BUTTONS;
                if ((g_pressed & XINPUT_GAMEPAD_A) && g_ps.rel < g_pl.nrel) {
                    EnterCriticalSection(&g_lock);
                    g_queue_n = 0; g_queue_mix = 0;                                  /* picked by hand: the "Play all" list ends */
                    chain_to(&g_pl.rel[g_ps.rel], 1);
                    LeaveCriticalSection(&g_lock);
                    quit = 1;
                }
            }
        }
        if (hide) {                                                 /* B / Up: controls away (back on Play) */
            g_ps.overlay_until = 0;
            g_ps.put_away = 1;
            g_ps.focus = PF_BUTTONS;
            g_ps.button = PB_PLAY;
        }
        if (g_ps.scrubbing) {
            if (g_ps.scrub_t < 0) g_ps.scrub_t = 0;
            if (dur > 1 && g_ps.scrub_t > dur - 1) g_ps.scrub_t = dur - 1;
            if (now - g_ps.scrub_last > 0.9) { seek_to = g_ps.scrub_t; g_ps.scrubbing = 0; }   /* stopped pressing: jump */
        }
        if (quit) break;

        /* ---- speed: the sound plays faster (XAudio2's frequency ratio, so the pitch goes up too); the clock follows
           the sound, so the pictures keep up by themselves ---- */
        if (new_speed > 0) {
            if (voice) IXAudio2SourceVoice_SetFrequencyRatio(voice, new_speed, XAUDIO2_COMMIT_NOW);
            else { t_base = clock; wall_start = now; paused_total = 0; if (paused) paused_at = now; }
            say("speed %.2fx", new_speed);
            g_speed = new_speed;
            new_speed = 0;
        }

        /* ---- pause / seek / quality ---- */
        if (seek_to == -2) {                                        /* pause or play */
            if (voice) {
                if (paused) IXAudio2SourceVoice_Stop(voice, 0, XAUDIO2_COMMIT_NOW);
                else IXAudio2SourceVoice_Start(voice, 0, XAUDIO2_COMMIT_NOW);
            }
            if (paused) paused_at = now; else paused_total += now - paused_at;
            seek_to = -1;
        }
        if (new_q >= 0) {                                           /* another quality: reopen at the same time */
            int old = g_pl.q, r;
            char msg[64];
            _snprintf(msg, sizeof(msg) - 1, "Switching to %dp...", g_pl.q_height[new_q]);
            msg[sizeof(msg) - 1] = 0;
            p360_frame_begin(0xFF000000);
            p360_draw_safe(1);
            spinner(640, 330, 22, 0xFFFFFFFF);
            p360_text(F_REG, 640 - p360_text_width(F_REG, 24, msg) / 2, 380, 24, 0xFFFFFFFF, msg);
            p360_frame_end();
            kill_voice(&voice);
            sbplayer_close();
            say("quality: %dp -> %dp at %.1f s", g_pl.q_height[old], g_pl.q_height[new_q], clock);
            r = sbplayer_open(g_pl.q_url[new_q], g_pl.audio[0] ? g_pl.audio : NULL, lib_log, 4, info);
            if (r < 0) {
                sbplayer_close();
                toast("That quality couldn't be opened.");
                new_q = old;
                r = sbplayer_open(g_pl.q_url[old], g_pl.audio[0] ? g_pl.audio : NULL, lib_log, 4, info);
                if (r < 0) { sbplayer_close(); say("quality: reopening failed too (%d)", r); new_q = -1; break; }
            }
            g_pl.q = new_q;
            new_q = -1;
            seek_to = clock;
            if (info->live) { seek_to = 0; live_restart = 1; }   /* live: the new stream's clock starts again at 0 */
            finished = 0;
            g_ps.upnext_at = 0;
        }
        if (seek_to >= 0 && info->live && !live_restart) {         /* (previous button, resume ...) */
            seek_to = -1;
            toast("This is a live stream: it can't skip back or ahead.");
        }
        live_restart = 0;
        if (seek_to >= 0) {
            sbplayer_seek(seek_to);
            kill_voice(&voice);
            waiting = 1;
            t_base = clock = seek_to;
            finished = 0;
            g_ps.upnext_at = 0;
            say("seek to %.1f s", seek_to);
            seek_to = -1;
        }
        if (waiting && !sbplayer_seeking()) {                       /* the jump is done: sound + clock restart */
            waiting = 0;
            voice = make_voice(info, !paused);
            next_buf = 0;
            st.BuffersQueued = 0;
            wall_start = now;
            paused_total = 0;
            if (paused) paused_at = now;
        }

        /* ---- sound + clock ---- */
        if (waiting) clock = t_base;
        else if (voice) {
            IXAudio2SourceVoice_GetState(voice, &st, 0);
            while (st.BuffersQueued < BUFS - 1) {
                XAUDIO2_BUFFER b;
                int n = sbplayer_audio(g_abuf[next_buf], BUF_FRAMES);
                if (n <= 0) break;
                ZeroMemory(&b, sizeof(b));
                b.AudioBytes = n * info->channels * 2;
                b.pAudioData = (BYTE *)g_abuf[next_buf];
                IXAudio2SourceVoice_SubmitSourceBuffer(voice, &b, NULL);
                next_buf = (next_buf + 1) % BUFS;
                st.BuffersQueued++;
            }
            clock = t_base + (double)st.SamplesPlayed / info->sample_rate;
        } else
            clock = t_base + ((paused ? paused_at : now) - wall_start - paused_total) * g_speed;

        /* ---- live: fresh addresses every LIVE_REFRESH s (the old ones expire), handed to the engine ---- */
        if (info->live) {
            EnterCriticalSection(&g_lock);
            if (now - last_live > LIVE_REFRESH && !g_live_job) {
                g_live_job = g_play_seq;
                copy_str(g_live_job_id, sizeof(g_live_job_id), g_pl.v.id);
                SetEvent(g_job_event);
                last_live = now;
            }
            if (g_live_fresh) {
                g_live_fresh = 0;
                sbplayer_live_url(g_pl.q_url[g_pl.q]);
            }
            LeaveCriticalSection(&g_lock);
        }

        /* ---- remember where we are (history: resume + "Continue watching"), every 30 s ---- */
        if (now - last_save > 30 && !waiting && !g_ps.resume_until && clock > 5 && !info->live) {
            EnterCriticalSection(&g_lock);
            watch_update(&g_pl.v, (int)clock, (int)dur);
            LeaveCriticalSection(&g_lock);
            last_save = now;
        }

        /* ---- the video ended: next one in a few seconds ---- */
        if (!waiting && !finished && sbplayer_finished() && (!voice || st.BuffersQueued == 0)) {
            finished = 1;
            if (g_loop == 1 && !info->live) { seek_to = 0; say("loop: again from the start"); }    /* loop this video */
            else if (up_next() && (g_set.autoplay || g_queue_pos + 1 < g_queue_n || (g_loop == 2 && g_queue_n))) {
                g_ps.upnext_at = now + 8; g_ps.focus = PF_BAR; g_ps.panel = PP_NONE;
            }
            else quit = 1;
        }
        if (g_ps.upnext_at > 0 && now > g_ps.upnext_at) {
            EnterCriticalSection(&g_lock);
            go_next();
            LeaveCriticalSection(&g_lock);
            quit = 1;
        }

        /* ---- animation ---- */
        g_ps.slide += ((g_ps.focus == PF_QUEUE ? 250.0f : g_ps.focus == PF_MORE ? 250.0f + (queue_left() > 0 ? QROW_H : 0) : 0.0f)
                       - g_ps.slide) * 0.25f;
        if (g_ps.qcur < g_ps.qfirst) g_ps.qfirst = g_ps.qcur;
        if (g_ps.qcur > g_ps.qfirst + 4) g_ps.qfirst = g_ps.qcur - 4;
        g_ps.q_sx += (g_ps.qfirst * TILE_STEP - g_ps.q_sx) * 0.25f;
        if (g_ps.rel < g_ps.rel_first) g_ps.rel_first = g_ps.rel;
        if (g_ps.rel > g_ps.rel_first + 4) g_ps.rel_first = g_ps.rel - 4;
        g_ps.rel_sx += (g_ps.rel_first * TILE_STEP - g_ps.rel_sx) * 0.25f;
        if (g_pl.sb && (visible || g_ps.scrubbing)) {                  /* have the preview sheet for here ready */
            float sx, sy;
            sheet_for(g_ps.scrubbing ? g_ps.scrub_t : clock, &sx, &sy);
        }

        /* ---- draw ---- */
        if (!waiting && sbplayer_video(clock, &fr, &pts) && fr.pix_fmt == 0)
            p360_video_upload(fr.width, fr.height, fr.y, fr.u, fr.v, fr.y_stride, fr.uv_stride);
        p360_frame_begin(0xFF000000);
        p360_draw_safe(0);
        p360_video_draw(0, 0, 1280, 720);
        p360_draw_safe(1);
        EnterCriticalSection(&g_lock);
        if (visible) draw_player(clock, dur, paused, waiting);
        if (g_ps.panel == PP_NONE && !waiting)                          /* above the time box / near the bottom */
            draw_captions(clock, visible ? 486 - g_ps.slide : 650);
        if (g_ps.upnext_at > 0 && up_next()) draw_upnext(g_ps.upnext_at - now);
        pick_draw();                                                /* "Save to..." (when open) */
        LeaveCriticalSection(&g_lock);
        if (g_ps.resume_until > 0) {                                /* "Resume watching?" */
            char at[16], line[64];
            fmt_time(at, g_ps.resume_t);
            _snprintf(line, sizeof(line) - 1, "You stopped at %s.", at);
            line[sizeof(line) - 1] = 0;
            p360_rect(390, 230, 500, 160, 0xF2181818);
            p360_rect(390, 230, 500, 4, C_RED);
            p360_text(F_BOLD, 420, 254, 26, 0xFFFFFFFF, "Resume watching?");
            p360_text(F_REG, 420, 294, 20, 0xFFBBBBBB, line);
            p360_text(F_REG, 420, 342, 20, 0xFFEEEEEE, "A  Resume          B  Start over");
        }
        if (now < g_ps.toast_until) {
            float w = p360_text_width(F_REG, 22, g_ps.toast) + 48;
            p360_rect(640 - w / 2, 250, w, 54, 0xF0181818);
            p360_text(F_REG, 640 - w / 2 + 24, 264, 22, 0xFFFFFFFF, g_ps.toast);
        }
        p360_frame_end();
        if (quit) break;
    }
    sbplayer_stats(&shown, &dropped);
    say("played to %.1f s: %d pictures shown, %d dropped", clock, shown, dropped);
    if (info->live) g_speed = speed_before;     /* (live forced normal speed) */
    else if (!g_ps.resume_until || clock > 1) { /* (left at the resume question: keep the old place) */
        EnterCriticalSection(&g_lock);
        watch_update(&g_pl.v, (int)(finished ? dur : clock), (int)dur);
        LeaveCriticalSection(&g_lock);
    }
    {
        int fh, fms, rh, rms;
        sbhttp_handshake_stats(&fh, &fms, &rh, &rms);
        say("secure connections so far: %d full (avg %d ms), %d with a kept session (avg %d ms)", fh, fms, rh, rms);
    }
    kill_voice(&voice);
    sbplayer_close();
    pimg_reset();
}

/* ---------------------------------------------------------------- playlist picker ("Save to...") */
/* Opened with Y from the menu's info panel or the player's description panel: every playlist (Watch later first)
   with a tick when the video is in it, plus "New playlist..." (named with the Xbox keyboard). A = in / out,
   X twice = delete that playlist, B = done. */
static int g_pick_sel, g_pick_del = -1;
static double g_pick_del_t;
static VIDEO g_pick_v;

static void note(const char *text)          /* the menu's message + the player's (whichever is on screen) */
{
    mtoast(text);
    copy_str(g_ps.toast, sizeof(g_ps.toast), text);
    g_ps.toast_until = now_s() + 3;
}

static void library_changed(void)           /* g_lock held: the Account page shows the lists: rebuild it */
{
    if (g_side_page == 6) account_rows();
    if (g_cache[6].used) cache_free(&g_cache[6]);
}

static void pick_open(const VIDEO *v)       /* g_lock held */
{
    g_pick_v = *v;
    g_pick_v.img = NULL; g_pick_v.pix = NULL; g_pick_v.thumb = 0;
    g_pick = 1;
    g_pick_sel = 0;
    g_pick_del = -1;
}

static void pick_new_list(const char *name) /* the Xbox keyboard gave the new playlist's name */
{
    char msg[160];
    int li;
    EnterCriticalSection(&g_lock);
    li = list_new(name);
    if (li < 0) note("That's the most playlists NewTube keeps (16).");
    else {
        list_toggle(li, &g_pick_v);
        _snprintf(msg, sizeof(msg) - 1, "Made \"%s\" and saved the video to it.", g_lists[li].name);
        msg[sizeof(msg) - 1] = 0;
        note(msg);
        g_pick_sel = li;
        library_changed();
    }
    LeaveCriticalSection(&g_lock);
}

static void pick_draw(void)                 /* g_lock held */
{
    int n = g_nlists + 1, first = 0, shown = n < 8 ? n : 8, k;
    float x = 400, w = 480, row = 42, h, y;
    if (!g_pick) return;
    if (g_pick_sel > first + shown - 1) first = g_pick_sel - shown + 1;
    h = shown * row + 120;
    y = 360 - h / 2;
    p360_draw_safe(0);
    p360_rect(0, 0, 1280, 720, 0x90000000);
    p360_draw_safe(1);
    p360_rect(x, y, w, h, 0xF4202020);
    p360_rect(x, y, w, 4, C_GREEN);
    p360_text(F_BOLD, x + 20, y + 16, 22, 0xFFFFFFFF, "Save to...");
    p360_text_fit(F_REG, x + 20, y + 46, w - 40, 17, 0xFFAAAAAA, g_pick_v.title);
    for (k = first; k < first + shown; k++) {
        float ry = y + 76 + (k - first) * row;
        if (k == g_pick_sel) p360_rect(x + 6, ry, w - 12, row, 0xFF3A3A3A);
        if (k < g_nlists) {
            int in = list_has(k, g_pick_v.id) >= 0;
            p360_frame_rect(x + 20, ry + 11, 20, 20, 2, 0xFFCCCCCC);              /* the tick box */
            if (in) p360_rect(x + 25, ry + 16, 10, 10, C_GREEN);
            p360_text_fit(in ? F_BOLD : F_REG, x + 54, ry + 10, w - 90, 20, 0xFFFFFFFF, g_lists[k].name);
        } else
            p360_text(F_REG, x + 54, ry + 10, 20, 0xFF8FD460, "+  New playlist...");
    }
    p360_text(F_REG, x + 20, y + h - 34, 16, 0xFF999999, "A  Add / remove      X  Delete playlist      B  Done");
}

static int pick_input(void)                 /* g_lock held. 1 = the picker took the buttons */
{
    int n = g_nlists + 1;
    char msg[160];
    if (!g_pick) return 0;
    if ((g_repeat & XINPUT_GAMEPAD_DPAD_UP) && g_pick_sel > 0) g_pick_sel--;
    if ((g_repeat & XINPUT_GAMEPAD_DPAD_DOWN) && g_pick_sel < n - 1) g_pick_sel++;
    if (g_pressed & XINPUT_GAMEPAD_B) { g_pick = 0; return 1; }
    if (g_pressed & XINPUT_GAMEPAD_A) {
        if (g_pick_sel == g_nlists) {           /* New playlist...: its name from the Xbox keyboard */
            if (!g_kb_open) {
                ZeroMemory(&g_kb_ov, sizeof(g_kb_ov));
                g_kb_buf[0] = 0;
                if (XShowKeyboardUI(0, VKBD_DEFAULT, L"", L"New playlist", L"A name for the new playlist:", g_kb_buf,
                                    60, &g_kb_ov) == ERROR_IO_PENDING) { g_kb_open = 1; g_kb_mode = 1; }
            }
        } else {
            int in = list_toggle(g_pick_sel, &g_pick_v);
            _snprintf(msg, sizeof(msg) - 1, in ? "Saved to %s." : "Removed from %s.", g_lists[g_pick_sel].name);
            msg[sizeof(msg) - 1] = 0;
            note(msg);
            library_changed();
        }
    }
    if ((g_pressed & XINPUT_GAMEPAD_X) && g_pick_sel > 0 && g_pick_sel < g_nlists) {
        if (g_pick_del == g_pick_sel && now_s() - g_pick_del_t < 3) {
            _snprintf(msg, sizeof(msg) - 1, "Deleted the playlist \"%s\".", g_lists[g_pick_sel].name);
            msg[sizeof(msg) - 1] = 0;
            list_delete(g_pick_sel);
            if (g_pick_sel > g_nlists) g_pick_sel = g_nlists;
            g_pick_del = -1;
            note(msg);
            library_changed();
        } else {
            g_pick_del = g_pick_sel;
            g_pick_del_t = now_s();
            note("Press X again to delete this playlist.");
        }
    } else if ((g_pressed & XINPUT_GAMEPAD_X) && g_pick_sel == 0)
        note("Watch later can't be deleted (it empties itself as you remove videos).");
    return 1;
}

static void watch_later_toggle(const VIDEO *v)  /* g_lock held: X in the info / description panels */
{
    char msg[160];
    int in = list_toggle(0, v);
    _snprintf(msg, sizeof(msg) - 1, in ? "Saved to Watch later." : "Removed from Watch later.");
    msg[sizeof(msg) - 1] = 0;
    note(msg);
    library_changed();
}

/* ---------------------------------------------------------------- search page */
/* Laid out after the modern YouTube TV search page (owner's reference; NewTube look to follow): what you type shows
   in the top bar's search box; left = recent searches (box empty) or suggestions; right = a letter keyboard with
   Back / &123 / More... (the Xbox keyboard, for other languages) and SPACE / CLEAR / SEARCH; below = a "Results
   for ..." row that fills in when you stop typing. X = backspace, Y = space, START = search, B = sidebar. */
static const char *KB_ABC[4] = { "ABCDEFG", "HIJKLMN", "OPQRSTU", "VWXYZ-'" };
static const char *KB_SYM[4] = { "1234567", "890.,!?", "@#$%&*(", ")_+=/:;" };
static int g_sf;                    /* focus: 0 keyboard, 1 list (recent / suggestions), 2 results row */
static int g_kr, g_kc;              /* key: row 0-4 (4 = SPACE / CLEAR / SEARCH), column */
static int g_ksym;                  /* 1 = numbers + symbols on the keys */
static int g_sl;                    /* list row */
static double g_q_changed = -1;     /* when the text last changed (-1 = its suggestions / results are asked) */
static char g_sug_asked[Q_MAX], g_preview_q[Q_MAX];
#define KB_X 736.0f
#define KB_Y 126.0f
#define KB_W 62.0f
#define KB_H 60.0f
#define LIST_ROWS 7
#define RES_Y 480.0f                /* the results row, on the paper under the dark block */
#define DARK_Y1 457.0f              /* bottom of the dark block (owner's mockup 2026-11-22) */
/* short names for the keyboard's Filter key (same order as SFILTERS) */
static const char *SFILTER_SHORT[] = { "All", "Today", "This week", "This month", "This year", "Under 4 min",
                                       "Over 20 min", "Newest", "Most viewed" };
static int g_preview_f = -1;        /* the filter the results row was made with */

static void open_search_page(void)
{
    int keep;
    EnterCriticalSection(&g_lock);
    /* coming back from the full results of the same words: keep their first row as the results row (no reload) */
    keep = ON_RESULTS && g_nsec && g_sec[0].state == 2 && !strcmp(g_sec[0].query, g_q);
    if (keep) {
        int i, k;
        for (i = 1; i < g_nsec; i++) row_free_images(&g_sec[i]);
        for (k = 0; k < g_sec[0].n; k++) if (g_sec[0].v[k].thumb == 1) g_sec[0].v[k].thumb = 0;  /* (restarts) */
        g_gen++;
        g_nsec = 1;
    } else {
        leave_page();
        g_gen++;
        g_nsec = 0;
    }
    g_message[0] = 0;
    g_focus_row = 0;
    g_sy = 0;
    g_side_page = SEARCH_PAGE;
    g_side_focus = 0;
    g_sf = 0; g_sl = 0;
    if (keep) { g_kr = 4; g_kc = 2; } else { g_kr = 0; g_kc = 0; }   /* (back from results: on SEARCH) */
    g_sug_asked[0] = 0;
    if (keep) {
        copy_str(g_preview_q, Q_MAX, g_q);
        g_preview_f = g_sfilter;
        g_q_changed = now_s() - 0.3;        /* (suggestions again, the row stays) */
    } else {
        g_preview_q[0] = 0;
        g_q_changed = now_s();      /* (the text kept from last time gets its suggestions + results again) */
    }
    LeaveCriticalSection(&g_lock);
}

static int list_count(void)         /* g_lock held */
{
    int n = g_q[0] ? (g_sug_for[0] ? g_nsug : 0) : g_nrecent;
    return n < LIST_ROWS ? n : LIST_ROWS;
}
static const char *list_item(int i) { return g_q[0] ? g_sug[i] : g_recent[i]; }

static void typed(void) { g_q_changed = now_s(); }

static void type_text(const char *s)
{
    int n = (int)strlen(g_q), k = (int)strlen(s);
    if (n + k >= Q_MAX) return;
    memcpy(g_q + n, s, k + 1);
    typed();
}

static void backspace(void)
{
    int n = (int)strlen(g_q);
    if (!n) return;
    do n--; while (n > 0 && ((unsigned char)g_q[n] & 0xC0) == 0x80);  /* (a whole UTF-8 letter) */
    g_q[n] = 0;
    typed();
}

static void search_go(const char *q)    /* the full results page (two rows, as before) */
{
    char text[Q_MAX];
    if (!q || !q[0]) return;
    copy_str(text, Q_MAX, q);
    say("search: %s", text);
    open_search(text);
}

/* once per frame on the search page: suggestions 1/4 s after typing stops, the results row after 0.9 s */
static void search_tick(void)
{
    double idle;
    EnterCriticalSection(&g_lock);
    if (g_side_page != SEARCH_PAGE || g_q_changed < 0) { LeaveCriticalSection(&g_lock); return; }
    idle = now_s() - g_q_changed;
    if (idle > 0.25 && strcmp(g_q, g_sug_asked)) {
        copy_str(g_sug_asked, Q_MAX, g_q);
        g_sug_req++;
        if (g_q[0]) {
            g_sug_job = g_sug_req;
            copy_str(g_sug_job_q, Q_MAX, g_q);
            SetEvent(g_job_event);
        } else { g_nsug = 0; g_sug_for[0] = 0; }
    }
    if (idle > 0.9) {
        g_q_changed = -1;
        if (strcmp(g_q, g_preview_q) || g_preview_f != g_sfilter) {
            copy_str(g_preview_q, Q_MAX, g_q);
            g_preview_f = g_sfilter;
            if (g_nsec) row_free_images(&g_sec[0]);
            g_gen++;                            /* (older results / thumbnails still on the way are dropped) */
            g_nsec = 0;
            if (g_q[0]) {
                memset(&g_sec[0], 0, sizeof(SECTION));
                if (g_sfilter) _snprintf(g_sec[0].title, sizeof(g_sec[0].title) - 1, "Results for \"%.40s\" (%s)", g_q,
                                         SFILTERS[g_sfilter].name);
                else _snprintf(g_sec[0].title, sizeof(g_sec[0].title) - 1, "Results for \"%s\"", g_q);
                copy_str(g_sec[0].query, sizeof(g_sec[0].query), g_q);
                g_sec[0].state = 1;
                g_nsec = 1;
                post_job(JOB_PREVIEW, g_q);
            }
            if (g_sf == 2) g_sf = 0;
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void draw_key(float x, float y, float w, float h, const char *label, int focused)
{
    float size = strlen(label) == 1 ? 26.0f : 18.0f;
    if (focused) p360_rect_v(x, y, w, h, 0xFF72CC33, 0xFF4E9A1A);
    else {                                  /* dark keys, bright letters (they sit on the dark block) */
        p360_rect_v(x, y, w, h, 0xFF4C4C4C, 0xFF3B3B3B);
        p360_frame_rect(x, y, w, h, 1, 0xFF262626);
    }
    p360_text(F_BOLD, x + (w - p360_text_width(F_BOLD, size, label)) / 2, y + (h - size) / 2 - 2, size,
              focused ? 0xFFFFFFFF : 0xFFE8E8E8, label);
}

static void draw_search_page(void)          /* g_lock held */
{
    static const char *BOTTOM[3] = { "SPACE", "CLEAR", "SEARCH" };
    static const float BX[3] = { KB_X, KB_X + 188.0f, KB_X + 346.0f }, BW[3] = { 180.0f, 150.0f, 180.0f };
    int k;
    float ry = RES_Y - (1 - g_kb_t) * KB_SLIDE;    /* (the results row moves up as the keyboard slides away) */
    draw_kb_block();

    /* the results row */
    if (g_nsec) {
        SECTION *s = &g_sec[0];
        p360_text_fit(F_REG, 266, ry, 900, 24, C_SECTION, s->title);
        if (s->state < 2) spinner(TILE_X0 + 30, ry + 110, 14, 0xFF666666);
        else if (s->state == 3 || !s->n) p360_text(F_REG, 266, ry + 50, 20, 0xFF995555, s->err[0] ? s->err : "Nothing found.");
        p360_clip(250, ry + 30, 1030, 760 - ry - 30);
        for (k = 0; k < s->n; k++) {
            float x = TILE_X0 + k * TILE_STEP - s->sx;
            if (x < TILE_X0 - TILE_STEP || x > 1280) continue;
            draw_tile(&s->v[k], x, ry + 40, !g_side_focus && g_sf == 2 && k == s->col);
        }
        p360_clip_off();
    }
}

/* the dark block with the suggestions + keyboard, slid up by (1 - g_kb_t) * KB_SLIDE (the top bar, drawn after,
   covers what has gone up); also drawn over the full results page while it slides away / back. g_lock held */
static void draw_kb_block(void)
{
    static const char *BOTTOM[3] = { "SPACE", "CLEAR", "SEARCH" };
    static const float BX[3] = { KB_X, KB_X + 188.0f, KB_X + 346.0f }, BW[3] = { 180.0f, 150.0f, 180.0f };
    int r, c, n = list_count(), i;
    char key[2] = { 0, 0 };
    const char **rows = g_ksym ? KB_SYM : KB_ABC;
    float d = -(1 - g_kb_t) * KB_SLIDE;
    if (g_kb_t < 0.005f) return;

    /* the dark block behind the list + keyboard (from the top bar to DARK_Y1, to the screen's right edge) */
    p360_draw_safe(0);
    p360_rect(64 + SIDE_W * 0.9f, 36 + (110 + d) * 0.9f, 1280, (DARK_Y1 - 110) * 0.9f, 0xFF333333);
    p360_rect(64 + SIDE_W * 0.9f, 36 + (DARK_Y1 + d) * 0.9f, 1280, 5, 0xFF262626);
    p360_draw_safe(1);

    /* left: recent searches / suggestions */
    p360_text(F_BOLD, 266, 120 + d, 20, 0xFFBBBBBB, g_q[0] ? (n ? "Suggestions" : "") :
              (n ? "Recent searches" : "Your searches will show up here."));
    for (i = 0; i < n; i++) {
        float y = 152.0f + i * 44.0f + d;
        int focused = !g_side_focus && g_sf == 1 && g_sl == i;
        if (focused) p360_rect(258, y - 5, 452, 40, C_SIDE_CUR);
        if (focused) p360_rect(258, y - 5, 5, 40, C_GREEN);
        p360_image_draw(IMG_SIDE[0], 270, y + 2, 24, 24, focused ? 0xFFFFFFFF : 0xFF9A9A9A);
        p360_text_fit(F_REG, 306, y, 396, 24, focused ? 0xFFFFFFFF : 0xFFE4E4E4, list_item(i));
    }

    /* the keyboard */
    for (r = 0; r < 4; r++)
        for (c = 0; c < 8; c++) {
            int focused = !g_side_focus && g_sf == 0 && g_kr == r && g_kc == c;
            float y = KB_Y + r * KB_H + d;
            if (c < 7) {
                key[0] = rows[r][c];
                draw_key(KB_X + c * KB_W, y, KB_W - 4, KB_H - 4, key, focused);
            } else if (r < 3)
                draw_key(KB_X + 7 * KB_W + 6, y, 84, KB_H - 4, r == 0 ? "Back" : r == 1 ? (g_ksym ? "ABC" : "&123") :
                         "More...", focused);
            else {                                                  /* Filter: what the search looks for */
                float fx = KB_X + 7 * KB_W + 6, fw = 84, fh = KB_H - 4;
                const char *nm = SFILTER_SHORT[g_sfilter];
                draw_key(fx, y, fw, fh, "", focused);
                p360_text(F_REG, fx + (fw - p360_text_width(F_REG, 14, "Filter")) / 2, y + 7, 14,
                          focused ? 0xFFFFFFFF : 0xFFAAAAAA, "Filter");
                p360_text_fit(F_BOLD, fx + 4 + (fw - 8 - p360_text_width(F_BOLD, 15, nm) > 0 ?
                              (fw - 8 - p360_text_width(F_BOLD, 15, nm)) / 2 : 0), y + 28, fw - 8, 15,
                              focused ? 0xFFFFFFFF : g_sfilter ? 0xFF8FD460 : 0xFFE8E8E8, nm);
            }
        }
    for (c = 0; c < 3; c++)
        draw_key(BX[c], KB_Y + 4 * KB_H + d, BW[c], KB_H - 4, BOTTOM[c], !g_side_focus && g_sf == 0 && g_kr == 4 && g_kc == c);
    p360_text(F_REG, KB_X, KB_Y + 5 * KB_H - 2 + d, 15, 0xFF9A9A9A, "X  backspace   Y  space   LB/RB  filter   START  search");
}

static void search_input(void)              /* g_lock held */
{
    SECTION *s = g_nsec ? &g_sec[0] : NULL;
    int n = list_count();
    const char **rows = g_ksym ? KB_SYM : KB_ABC;
    if (g_pressed & XINPUT_GAMEPAD_B) { g_side_focus = 1; g_side_cur = 0; return; }
    if (g_pressed & XINPUT_GAMEPAD_X) backspace();
    if (g_pressed & XINPUT_GAMEPAD_Y) type_text(" ");
    if (g_pressed & XINPUT_GAMEPAD_START) { search_go(g_q); return; }
    if (g_pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)) {   /* LB / RB = filter */
        g_sfilter = (g_sfilter + N_SFILTERS + ((g_pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) ? -1 : 1)) % N_SFILTERS;
        g_q_changed = now_s() - 0.6;        /* (the results row is made again with it shortly) */
    }
    if (g_sf == 0) {                        /* keyboard */
        if (g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) {
            if (g_kc > 0) g_kc--;
            else if (n) { g_sf = 1; if (g_sl >= n) g_sl = n - 1; }
            else { g_side_focus = 1; g_side_cur = 0; }
        }
        if (g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) {
            if (g_kr == 4) { if (g_kc < 2) g_kc++; }
            else if (g_kc < 7) g_kc++;
        }
        if (g_repeat & XINPUT_GAMEPAD_DPAD_UP) {
            if (g_kr == 4) { g_kr = 3; g_kc = g_kc == 0 ? 1 : g_kc == 1 ? 3 : 5; }
            else if (g_kr > 0) g_kr--;
        }
        if (g_repeat & XINPUT_GAMEPAD_DPAD_DOWN) {
            if (g_kr < 3) g_kr++;
            else if (g_kr == 3) { g_kr = 4; g_kc = g_kc <= 2 ? 0 : g_kc <= 4 ? 1 : 2; }
            else if (s && s->state == 2 && s->n) g_sf = 2;
        }
        if (g_pressed & XINPUT_GAMEPAD_A) {
            if (g_kr == 4) {
                if (g_kc == 0) type_text(" ");
                else if (g_kc == 1) { g_q[0] = 0; typed(); }
                else search_go(g_q);
            } else if (g_kc < 7) {
                char ch[2] = { rows[g_kr][g_kc], 0 };
                if (ch[0] >= 'A' && ch[0] <= 'Z') ch[0] += 'a' - 'A';
                type_text(ch);
            } else if (g_kr == 0) backspace();
            else if (g_kr == 1) g_ksym = !g_ksym;
            else if (g_kr == 2) start_keyboard();   /* More...: the Xbox keyboard (other languages) */
            else {                                  /* Filter: the next one */
                g_sfilter = (g_sfilter + 1) % N_SFILTERS;
                g_q_changed = now_s() - 0.6;
            }
        }
    } else if (g_sf == 1) {                 /* recent searches / suggestions */
        if (!n) { g_sf = 0; return; }
        if (g_sl >= n) g_sl = n - 1;
        if ((g_repeat & XINPUT_GAMEPAD_DPAD_UP) && g_sl > 0) g_sl--;
        if (g_repeat & XINPUT_GAMEPAD_DPAD_DOWN) {
            if (g_sl < n - 1) g_sl++;
            else if (s && s->state == 2 && s->n) g_sf = 2;
        }
        if (g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) g_sf = 0;
        if (g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) { g_side_focus = 1; g_side_cur = 0; }
        if (g_pressed & XINPUT_GAMEPAD_A) search_go(list_item(g_sl));
    } else {                                /* the results row */
        if (!s || !s->n) { g_sf = 0; return; }
        if (g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) {
            if (s->col > 0) s->col--;
            else if (n) g_sf = 1;
        }
        if ((g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) && s->col + 1 < s->n) s->col++;
        if (g_repeat & XINPUT_GAMEPAD_DPAD_UP) { g_sf = 0; g_kr = 4; g_kc = 1; }
        if (g_pressed & XINPUT_GAMEPAD_A) { g_queue_n = 0; g_queue_mix = 0; start_play(&s->v[s->col]); }
    }
}

/* ---------------------------------------------------------------- channel page */
/* Owner's mockup: the top bar, a banner (the channel's TV banner, its middle band), the avatar in a dark frame
   overlapping the banner's bottom, a dark strip with the name + subscribers + views, Subscribe and Play all buttons
   on the paper background, then shelves: star badge + "title >>" + Play all, 4 tiles across. Everything below the
   top bar scrolls with the focus. Controls: D-pad, A play / use the button, X play all of a shelf, B back. */

static void open_channel(const char *id)    /* main thread */
{
    EnterCriticalSection(&g_lock);
    leave_page();                           /* (the page we came from stays in the cache: B is instant) */
    if (g_side_page != CHAN_PAGE) g_chan_back = g_side_page;
    g_gen++;
    g_nsec = 0;
    g_message[0] = 0;
    g_focus_row = 0;
    g_sy = 0;
    g_side_page = CHAN_PAGE;
    g_side_focus = 0;
    memset(&g_chan, 0, sizeof(g_chan));
    copy_str(g_chan.id, sizeof(g_chan.id), id);
    g_chan.state = 1;
    g_chan.gen = g_gen;
    g_chan_row = -1;
    g_chan_btn = 0;
    post_job(JOB_CHANNEL, id);
    LeaveCriticalSection(&g_lock);
    say("channel %s", id);
}

static void close_channel(void)             /* B: back to where we came from */
{
    int back = g_chan_back;
    if (back < 1 || back >= N_SIDE || !SIDE[back].page) back = 1;  /* (search results aren't kept: Home) */
    pimg_reset();                           /* banner + avatar */
    open_page(back);
}

static void play_list(SECTION *s, int from)  /* g_lock held: "Play all" = the shelf's videos one after another */
{
    int k;
    if (!s || s->n == 0) return;
    g_queue_n = 0; g_queue_mix = 0;
    for (k = from; k < s->n && g_queue_n < MAX_QUEUE; k++) {
        if (s->v[k].list[0]) continue;      /* (a shelf of playlists: each one opens on its own) */
        g_queue[g_queue_n] = s->v[k];
        g_queue[g_queue_n].img = NULL;
        g_queue[g_queue_n].pix = NULL;
        g_queue_n++;
    }
    if (!g_queue_n) {                       /* only playlists here: X plays the one picked */
        if (s->v[s->col].list[0]) open_playlist(&s->v[s->col], 1);
        return;
    }
    g_queue_pos = 0;
    copy_str(g_queue_name, sizeof(g_queue_name), s->title);
    start_play(&g_queue[0]);
}

/* YOUTUBE PLAYLISTS ------------------------------------------------------------------------------------------------ */
static void play_rows(int row, int col)     /* g_lock held: the playlist page's videos from (row, col) to the end */
{
    int i, k;
    g_queue_n = 0; g_queue_mix = 0;
    for (i = row; i < g_nsec; i++)
        for (k = i == row ? col : 0; k < g_sec[i].n && g_queue_n < MAX_QUEUE; k++) {
            g_queue[g_queue_n] = g_sec[i].v[k];
            g_queue[g_queue_n].img = NULL;
            g_queue[g_queue_n].pix = NULL;
            g_queue_n++;
        }
    if (!g_queue_n) return;
    g_queue_pos = 0;
    copy_str(g_queue_name, sizeof(g_queue_name), g_plp.title[0] ? g_plp.title : "Playlist");
    start_play(&g_queue[0]);
}

static void open_playlist(const VIDEO *v, int autoplay)     /* any thread holding g_lock or none (it's recursive) */
{
    VIDEO tile = *v;                        /* (v points into the page we're about to leave) */
    EnterCriticalSection(&g_lock);
    if (g_side_page != PL_PAGE) {           /* where B goes back to */
        g_plp.back = g_side_page;
        g_plp.back_text[0] = 0;
        if (g_side_page == CHAN_PAGE) copy_str(g_plp.back_text, sizeof(g_plp.back_text), g_chan.id);
        if (g_side_page == 0 || g_side_page == SEARCH_PAGE) copy_str(g_plp.back_text, sizeof(g_plp.back_text), g_q);
    }
    if (g_side_page == CHAN_PAGE) pimg_reset();    /* (the channel's banner + avatar) */
    leave_page();
    g_gen++;
    g_nsec = 0;
    g_message[0] = 0;
    g_focus_row = 0;
    g_sy = 0;
    g_side_page = PL_PAGE;
    g_side_focus = 0;
    g_info = 0;
    g_plp.gen = g_gen;
    g_plp.state = 1;
    g_pl_top = 0; g_pl_sy = 0; g_pl_moved = now_s();
    g_plp.autoplay = autoplay;
    copy_str(g_plp.id, sizeof(g_plp.id), tile.list);
    copy_str(g_plp.title, sizeof(g_plp.title), tile.title);
    copy_str(g_plp.channel, sizeof(g_plp.channel), tile.channel);
    copy_str(g_plp.count, sizeof(g_plp.count), tile.dur);
    post_job(JOB_PLAYLIST, tile.list);
    LeaveCriticalSection(&g_lock);
    say("playlist %s \"%s\"%s", tile.list, tile.title, autoplay ? " (play all)" : "");
}

static void close_playlist(void)            /* main thread: B on the playlist page */
{
    int back = g_plp.back;
    char text[120];
    copy_str(text, sizeof(text), g_plp.back_text);
    pimg_reset();                           /* (the big pictures of the videos you rested on) */
    if (back == CHAN_PAGE && text[0]) open_channel(text);
    else if (back == SEARCH_PAGE) open_search_page();
    else if (back == 0 && text[0]) open_search(text);
    else open_page(back >= 1 && back < N_SIDE && SIDE[back].page ? back : 1);
}

/* SETTINGS PAGE (the sidebar gear) --------------------------------------------------------------------------------- */
#define SET_ROWS 10
#define SET_FIRST_CLEAR 8           /* rows from here on are "Clear ..." actions */
static int g_set_row;               /* focused row */
static int g_set_armed = -1;        /* a "Clear ..." row waiting for its second A (-1 = none) */
static double g_set_armed_until;

static void open_settings(void)     /* main thread */
{
    EnterCriticalSection(&g_lock);
    leave_page();
    g_gen++;
    g_nsec = 0;
    g_message[0] = 0;
    g_focus_row = 0;
    g_sy = 0;
    g_side_page = SETTINGS_PAGE;
    g_side_focus = 0;
    g_set_row = 0;
    g_set_armed = -1;
    LeaveCriticalSection(&g_lock);
}

static void set_row_text(int r, const char **label, char *value, int vn)    /* what row r says */
{
    static const char *RESUME[3] = { "Ask me", "Always resume", "Always start over" };
    value[0] = 0;
    switch (r) {
    case 0: *label = "Starting video quality";
        _snprintf(value, vn - 1, "%dp%s", QUALITY_CHOICES[g_set.quality], g_set.quality == 3 ? " (may stutter)" : "");
        break;
    case 1: *label = "Autoplay the next video"; copy_str(value, vn, g_set.autoplay ? "On" : "Off"); break;
    case 2: *label = "Captions"; copy_str(value, vn, g_set.captions ? "Last language used" : "Always off"); break;
    case 3: *label = "Playback speed"; copy_str(value, vn, g_set.keep_speed ? "Keep the last speed" : "Always normal"); break;
    case 4: *label = "Resume where I stopped"; copy_str(value, vn, RESUME[g_set.resume]); break;
    case 5: *label = "Videos for the region";
        if (g_set.region < 0) {
            const char *c = region_code();
            _snprintf(value, vn - 1, "Automatic (%s)", c[0] ? region_name(c) : "Worldwide");
        } else copy_str(value, vn, REGIONS[g_set.region].name);
        break;
    case 6: *label = "Text size";
        if (g_set.textsize == 0) _snprintf(value, vn - 1, "Automatic (%s)", text_larger() ? "Larger" : "Normal");
        else copy_str(value, vn, g_set.textsize == 2 ? "Larger" : "Normal");
        break;
    case 7: *label = "Automatic YouTube fixes"; copy_str(value, vn, g_set.autoupdate ? "On (checked at start)" : "Off"); break;
    case 8: *label = "Clear watch history"; break;
    default: *label = "Clear search history"; break;
    }
    value[vn - 1] = 0;
}

static void settings_change(int r, int dir)     /* g_lock held: Left/Right (or A) on a value row */
{
    switch (r) {
    case 0: g_set.quality = (g_set.quality + 4 + dir) % 4; break;
    case 1: g_set.autoplay = !g_set.autoplay; break;
    case 2: g_set.captions = !g_set.captions; break;
    case 3: g_set.keep_speed = !g_set.keep_speed; break;
    case 4: g_set.resume = (g_set.resume + 3 + dir) % 3; break;
    case 5: {                                   /* Automatic, Worldwide, then the countries A-Z (wraps round) */
        int i;
        g_set.region = (g_set.region + 1 + (N_REGIONS + 1) + dir) % (N_REGIONS + 1) - 1;
        g_region_ver++;
        for (i = 0; i < N_SIDE; i++) if (g_cache[i].used) cache_free(&g_cache[i]);  /* kept pages: other country's */
        break;
    }
    case 6: g_set.textsize = (g_set.textsize + 3 + dir) % 3; apply_text_size(); break;
    case 7: g_set.autoupdate = !g_set.autoupdate; break;
    default: return;
    }
    settings_save();
}

static void settings_clear(int r)               /* g_lock held: the second A on a "Clear" row */
{
    if (r == SET_FIRST_CLEAR) {
        g_nwatch = 0;
        watch_save();
        if (g_cache[1].used) cache_free(&g_cache[1]);   /* Home's "Continue watching" + red bars: rebuilt fresh */
        mtoast("Watch history cleared.");
        say("settings: watch history cleared");
    } else {
        FILE *f = fopen(RECENT_FILE, "wb");
        g_nrecent = 0;
        if (f) fclose(f);
        mtoast("Search history cleared.");
        say("settings: search history cleared");
    }
}

static void draw_settings_page(void)            /* g_lock held */
{
    int r;
    float y0 = 206, step = 46;
    char value[80], about[200];
    const char *label;
    p360_image_draw(IMG_BADGE, TILE_X0, 131, 60, 64, 0xFFFFFFFF);
    p360_text(F_REG, 337, 148, 26, C_SECTION, "Settings \xC2\xBB");
    for (r = 0; r < SET_ROWS; r++) {
        float y = y0 + r * step;
        int foc = !g_side_focus && r == g_set_row, action = r >= SET_FIRST_CLEAR;
        set_row_text(r, &label, value, sizeof(value));
        if (foc) p360_rect(TILE_X0 - 8, y - 6, 990, step - 6, C_CARD);
        else {
            p360_rect(TILE_X0 - 8, y - 6, 990, step - 6, 0xC0FFFFFF);
            p360_frame_rect(TILE_X0 - 8, y - 6, 990, step - 6, 1, 0xFFD6D6D6);
        }
        p360_text(F_REG, TILE_X0 + 10, y + 7, 22, foc ? 0xFFFFFFFF : C_TITLE, label);
        if (action) {
            const char *hint = g_set_armed == r ? "Press A again to clear" : (foc ? "A  Clear" : "");
            p360_text(F_REG, 1230 - p360_text_width(F_REG, 19, hint), y + 9, 19,
                      g_set_armed == r ? 0xFFFF8A80 : 0xFFBBBBBB, hint);
        } else {
            float vw = p360_text_width(F_BOLD, 20, value);
            unsigned int c = foc ? 0xFFFFFFFF : 0xFF4A8A1E;
            p360_text(F_BOLD, 1200 - vw, y + 8, 20, c, value);
            if (foc) {
                p360_text(F_BOLD, 1200 - vw - 26, y + 6, 22, 0xFF8FD460, "\xE2\x80\xB9");
                p360_text(F_BOLD, 1212, y + 6, 22, 0xFF8FD460, "\xE2\x80\xBA");
            }
        }
    }
    _snprintf(about, sizeof(about) - 1, "NewTube %s   \xC2\xB7   YouTube script %s%s%s", NEWTUBE_VERSION,
              g_script_ver[0] ? g_script_ver : "loading...", g_update_note[0] ? "   \xC2\xB7   " : "", g_update_note);
    about[sizeof(about) - 1] = 0;
    p360_text_fit(F_REG, TILE_X0, y0 + SET_ROWS * step + 14, 960, 17, C_META, about);
    p360_text(F_REG, TILE_X0, y0 + SET_ROWS * step + 40, 17, C_META,
              "Up / Down  choose      Left / Right  change      B  back to the menu");
}

static void settings_input(void)                /* g_lock held */
{
    if (g_set_armed >= 0 && now_s() > g_set_armed_until) g_set_armed = -1;
    if (g_pressed & XINPUT_GAMEPAD_B) { g_side_focus = 1; g_side_cur = N_SIDE; g_set_armed = -1; return; }
    if (g_repeat & XINPUT_GAMEPAD_DPAD_UP) { g_set_row = (g_set_row + SET_ROWS - 1) % SET_ROWS; g_set_armed = -1; }
    if (g_repeat & XINPUT_GAMEPAD_DPAD_DOWN) { g_set_row = (g_set_row + 1) % SET_ROWS; g_set_armed = -1; }
    if (g_set_row < SET_FIRST_CLEAR) {
        if (g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) settings_change(g_set_row, -1);
        if (g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) settings_change(g_set_row, 1);
        if (g_pressed & XINPUT_GAMEPAD_A) settings_change(g_set_row, 1);
    } else {
        if (g_pressed & XINPUT_GAMEPAD_DPAD_LEFT) { g_side_focus = 1; g_side_cur = N_SIDE; g_set_armed = -1; return; }
        if (g_pressed & XINPUT_GAMEPAD_A) {
            if (g_set_armed == g_set_row) { settings_clear(g_set_row); g_set_armed = -1; }
            else { g_set_armed = g_set_row; g_set_armed_until = now_s() + 4; }
        }
    }
}

static void pill(float x, float y, float w, float h, const char *label, int focused)    /* the mockup's Play all */
{
    /* the owner's rounded pill (art pill_dark: white rim, dark inside); a label starting "X  " gets the X button
       picture instead of the triangle (X is what plays it) */
    int with_x = label[0] == 'X' && label[1] == ' ';
    const char *text = with_x ? label + 3 : label;
    float ic = h * 0.62f;
    if (focused) p360_frame_rect(x - 4, y - 4, w + 8, h + 8, 3, C_GREEN);
    if (IMG_PILL) p360_image_draw(IMG_PILL, x, y, w, h, 0xFFFFFFFF);
    else { p360_rect(x, y, w, h, 0xFFFFFFFF); p360_rect_v(x + 3, y + 3, w - 6, h - 6, 0xFF3C3C3C, 0xFF101010); }
    if (with_x && IMG_XBTN) p360_image_draw(IMG_XBTN, x + h * 0.32f, y + (h - ic) / 2, ic, ic, 0xFFFFFFFF);
    else p360_image_draw(IMG_PLAY, x + h * 0.38f, y + h / 2 - 8, 16, 16, 0xFFFFFFFF);
    p360_text(F_BOLD, x + h * 0.32f + ic + 8, y + h / 2 - 10, 18, 0xFFFFFFFF, text);
}

static void count_pair(float cx, float y, const char *number, const char *word)    /* "18.3M" over "subscribers" */
{
    p360_text(F_REG, cx - p360_text_width(F_REG, 20, number) / 2, y, 20, 0xFFD0D0D0, number);
    p360_text(F_REG, cx - p360_text_width(F_REG, 20, word) / 2, y + 24, 20, 0xFFD0D0D0, word);
}

static void draw_channel(void)              /* g_lock held */
{
    float oy = -g_sy, y;
    int i, k;
    char t[120], num[48];
    p360_image *img;
    p360_clip(-71, 110, 1422, 700);

    /* banner: the middle band of the TV banner (16:9), edge to edge */
    img = g_chan.banner[0] ? pimg_get(pimg_want(g_chan.banner)) : NULL;
    if (img) {
        int iw, ih;
        float crop;
        p360_image_size(img, &iw, &ih);
        crop = iw * 302.0f / 1422.0f;
        p360_image_draw_part(img, 0, (ih - crop) / 2, (float)iw, crop, -71, 110 + oy, 1422, 302, 0xFFFFFFFF);
    } else
        p360_rect(-71, 110 + oy, 1422, 302, 0xFFC2C2C2);

    /* the dark strip: name, subscribers, views */
    p360_rect_v(-71, 412 + oy, 1422, 60, 0xFF4E4E4E, 0xFF2A2A2A);
    p360_rect(-71, 412 + oy, 1422, 1, 0xFF6A6A6A);
    p360_text_fit(F_REG, 200, 426 + oy, 680, 30, 0xFFFFFFFF, g_chan.title[0] ? g_chan.title : "Loading...");
    if (g_chan.subs[0]) {                   /* "18.3M subscribers" -> "18.3M" over "subscribers" */
        const char *sp = strchr(g_chan.subs, ' ');
        int n = sp ? (int)(sp - g_chan.subs) : (int)strlen(g_chan.subs);
        if (n >= (int)sizeof(num)) n = sizeof(num) - 1;
        memcpy(num, g_chan.subs, n);
        num[n] = 0;
        count_pair(1016, 418 + oy, num, sp ? sp + 1 : "");
    }
    if (g_chan.views[0]) count_pair(1193, 418 + oy, g_chan.views, "views");

    /* avatar in its dark frame, over the banner's bottom edge */
    p360_rect(50, 332 + oy, 133, 133, 0xFF2A2A2A);
    img = g_chan.avatar[0] ? pimg_get(pimg_want(g_chan.avatar)) : NULL;
    p360_image_draw(img ? img : IMG_AVATAR, 55, 337 + oy, 123, 123, 0xFFFFFFFF);

    /* Subscribe + Play all */
    if (g_chan_row < 0 && g_chan_btn == 0) p360_frame_rect(45, 480 + oy, 144, 50, 3, C_GREEN);
    if (sub_find(g_chan.id) >= 0) {         /* subscribed: the 2012 grey "Subscribed" button */
        p360_rect_v(49, 484 + oy, 136, 42, 0xFFF4F4F4, 0xFFD8D8D8);
        p360_frame_rect(49, 484 + oy, 136, 42, 1, 0xFFB0B0B0);
        p360_text(F_REG, 117 - p360_text_width(F_REG, 21, "Subscribed") / 2, 493 + oy, 21, 0xFF555555, "Subscribed");
    } else {
        p360_rect_v(49, 484 + oy, 136, 42, 0xFFFFE47A, 0xFFF2B705);
        p360_frame_rect(49, 484 + oy, 136, 42, 1, 0xFFD69E00);
        p360_text(F_REG, 117 - p360_text_width(F_REG, 22, "Subscribe") / 2, 492 + oy, 22, 0xFF6A3B00, "Subscribe");
    }
    if (g_nsec) pill(205, 486 + oy, 126, 38, "Play all", g_chan_row < 0 && g_chan_btn == 1);

    if (g_chan.state == 1) {
        spinner(80, 600 + oy, 16, 0xFF555555);
        p360_text(F_REG, 110, 588 + oy, 24, C_SECTION, "Loading the channel...");
    } else if (g_chan.state == 3)
        p360_text_wrap(F_REG, 51, 580 + oy, 1150, 22, 3, 0xFF995555, g_chan.err[0] ? g_chan.err : "This channel couldn't be loaded.");
    else if (g_nsec == 0)
        p360_text(F_REG, 51, 580 + oy, 22, C_SECTION, "This channel has no videos to show here.");

    /* shelves */
    for (i = 0; i < g_nsec; i++) {
        SECTION *s = &g_sec[i];
        y = CH_SEC_Y0 + i * CH_STEP + oy;
        if (y > 780 || y + CH_STEP < 110) continue;
        p360_image_draw(IMG_BADGE, 51, y, 71, 71, 0xFFFFFFFF);
        _snprintf(t, sizeof(t) - 1, "%s \xC2\xBB", s->title);
        t[sizeof(t) - 1] = 0;
        p360_text_fit(F_REG, 137, y + 18, 900, 28, 0xFF222222, t);
        pill(1078, y + 14, 130, 41, "X  Play all", 0);
        for (k = 0; k < s->n; k++) {
            float x = 51 + k * CH_TILE_STEP - s->sx;
            if (x < -CH_TILE_STEP || x > 1350) continue;
            draw_tile_w(&s->v[k], x, y + 87, CH_TILE_W, g_chan_row >= 0 && i == g_focus_row && k == s->col);
        }
        if (s->first + 4 < s->n) {          /* more to the right */
            p360_rect(1252, y + 87 + 77 - 20, 14, 40, 0x80000000);
            p360_text(F_BOLD, 1253, y + 87 + 77 - 13, 22, 0xFFFFFFFF, "\xC2\xBB");
        }
    }
    p360_clip_off();
}

static void channel_input(void)             /* g_lock held */
{
    SECTION *s = g_chan_row >= 0 && g_focus_row < g_nsec ? &g_sec[g_focus_row] : NULL;
    if (g_pressed & XINPUT_GAMEPAD_B) { close_channel(); return; }
    if (g_repeat & XINPUT_GAMEPAD_DPAD_UP) {
        if (g_chan_row >= 0 && g_focus_row > 0) g_focus_row--;
        else g_chan_row = -1;
    }
    if ((g_repeat & XINPUT_GAMEPAD_DPAD_DOWN) && g_nsec) {
        if (g_chan_row < 0) { g_chan_row = 0; g_focus_row = 0; }
        else if (g_focus_row + 1 < g_nsec) g_focus_row++;
    }
    if (g_chan_row < 0) {                   /* the buttons */
        if (g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) g_chan_btn = 0;
        if ((g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) && g_nsec) g_chan_btn = 1;
        if (g_pressed & XINPUT_GAMEPAD_A) {
            if (g_chan_btn == 0) {
                char msg[160];
                if (g_chan.state != 2) mtoast("Still loading the channel...");
                else {
                    int on = sub_toggle(g_chan.id, g_chan.title);
                    if (on && g_chan.avatar[0] && strlen(g_chan.avatar) < sizeof(g_subs[0].avatar)) {   /* + its picture */
                        copy_str(g_subs[0].avatar, sizeof(g_subs[0].avatar), g_chan.avatar);
                        subs_save();
                    }
                    _snprintf(msg, sizeof(msg) - 1, on ? "Subscribed to %s." : "Unsubscribed from %s.", g_chan.title);
                    msg[sizeof(msg) - 1] = 0;
                    mtoast(msg);
                    say("%s %s (%d subscriptions)", on ? "subscribed to" : "unsubscribed from", g_chan.title, g_nsubs);
                    if (g_cache[5].used) cache_free(&g_cache[5]);   /* the kept Subscriptions page is out of date */
                }
            } else if (g_nsec) play_list(&g_sec[0], 0);
        }
        return;
    }
    if (!s) return;
    if ((g_repeat & XINPUT_GAMEPAD_DPAD_LEFT) && s->col > 0) s->col--;
    if ((g_repeat & XINPUT_GAMEPAD_DPAD_RIGHT) && s->col + 1 < s->n) s->col++;
    if ((g_pressed & XINPUT_GAMEPAD_A) && s->n) { g_queue_n = 0; g_queue_mix = 0; start_play(&s->v[s->col]); }
    if (g_pressed & XINPUT_GAMEPAD_X) play_list(s, 0);
}

/* ---------------------------------------------------------------- start */
static p360_image *ui(const char *name)
{
    char path[96];
    _snprintf(path, sizeof(path) - 1, "game:\\ui\\%s.sbi", name);
    path[sizeof(path) - 1] = 0;
    return p360_image_load(path);
}

/* a background thread: pinned to one hardware thread (not 0, the screen's). NORMAL priority on purpose: v0.6 used
   LOWEST, and the 360 never lets a starved low-priority thread run - one holding a lock while a decoder thread kept
   its core busy froze everything that waited for that lock (console hang) */
static void helper(HANDLE h, int hw_thread)
{
    if (!h) return;
    XSetThreadProcessor(h, hw_thread);
    ResumeThread(h);
    CloseHandle(h);
}

static DWORD WINAPI app(void *unused)
{
    IXAudio2MasteringVoice *speakers;
    int i, frames = 0;
    double t_report;
    (void)unused;
    say("NewTube (09_newtube) starting");
    InitializeCriticalSection(&g_lock);
    subs_load();
    settings_load();
    watch_load();
    recent_load();
    lists_load();
    offline_load();
    say("%d subscriptions, %d videos in the history, %d saved offline", g_nsubs, g_nwatch, g_noff);
    if (!p360_video_init(1) || !p360_draw_init()) { say("graphics failed"); return 0; }
    apply_text_size();
    /* Liberation Sans (SIL Open Font License: free to share; same letter widths as Arial), Latin + Greek + Cyrillic */
    F_REG = p360_font_load("game:\\fonts\\liberation_sans.sbf");
    F_BOLD = p360_font_load("game:\\fonts\\liberation_sans_bold.sbf");
    IMG_LOGO = ui("logo"); IMG_PAPER = ui("paper"); IMG_BADGE = ui("badge_star");
    IMG_AVATAR = ui("avatar"); IMG_YBTN = ui("y_button"); IMG_GEAR = ui("icon_gear");
    IMG_XBTN = ui("x_button"); IMG_PILL = ui("pill_dark");
    for (i = 0; i < N_SIDE; i++) IMG_SIDE[i] = ui(SIDE[i].icon);
    IMG_KNOB = ui("knob"); IMG_PREV = ui("icon_prev"); IMG_NEXT = ui("icon_next"); IMG_PLAY = ui("icon_play");
    IMG_PAUSE = ui("icon_pause"); IMG_COMMENT = ui("icon_comment"); IMG_HD = ui("hd_badge");
    if (!F_REG || !F_BOLD) { say("fonts missing"); return 0; }
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(IXAudio2_CreateMasteringVoice(g_xa, &speakers, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE,
                                             0, 0, NULL)))
        say("XAudio2 failed (no sound)");
    if (sbhttp_init("game:\\cacert.pem", lib_log)) {
        copy_str(g_message, sizeof(g_message), "The network couldn't be started (cacert.pem missing?).");
    } else {
        g_job_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        /* helpers OFF hardware thread 0 (the screen + sound loop lives there). Without this, reading the "More
           Videos" reply and downloading pictures while a video played made it stutter (v0.5: 37 late in 45 s) */
        helper(CreateThread(NULL, 1024 * 1024, script_thread, NULL, CREATE_SUSPENDED, NULL), 2);
        for (i = 0; i < 3; i++) helper(CreateThread(NULL, 256 * 1024, thumb_thread, NULL, CREATE_SUSPENDED, NULL), 3 + i);
        for (i = 0; i < 2; i++) helper(CreateThread(NULL, 256 * 1024, pimg_thread, NULL, CREATE_SUSPENDED, NULL), 4 + i);
        g_dl_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        /* hw 5: the video decoder's 4 threads use 1-4, so saving never slows a video down */
        helper(CreateThread(NULL, 256 * 1024, dl_thread, NULL, CREATE_SUSPENDED, NULL), 5);
        open_page(1);
    }
    t_report = now_s();
    for (;;) {
        read_pad();
        if (g_mode == M_MENU) {
            if ((g_pressed & XINPUT_GAMEPAD_BACK) && !g_kb_open && g_play_state != PS_LOADING) break;
            menu_input();
            search_tick();
            menu_animate();
            upload_thumbs();
            EnterCriticalSection(&g_lock);  /* a save started / finished / failed: the Offline Player page shows it */
            if (g_off_dirty && g_side_page == OFFLINE_PAGE && !g_info) { offline_rows(); g_off_dirty = 0; }
            LeaveCriticalSection(&g_lock);
            draw_menu();
            if (g_play_state == PS_READY) {
                g_play_state = PS_IDLE;
                EnterCriticalSection(&g_lock);
                g_dl_hold = !g_pl_offline;      /* streaming: saving waits (the stream gets the whole network) */
                LeaveCriticalSection(&g_lock);
                play_loop();
                EnterCriticalSection(&g_lock);
                g_dl_hold = 0;
                LeaveCriticalSection(&g_lock);
                SetEvent(g_dl_event);
                g_paused_thumbs = 0;
                EnterCriticalSection(&g_lock);  /* the page we're back on: history rows as they are now */
                if (g_side_page == 1 || g_side_page == 6) {
                    int i, settled = 1;
                    for (i = 0; i < g_nsec; i++) if (g_sec[i].state < 2) settled = 0;   /* (still loading: leave) */
                    if (settled && g_side_page == 1 && g_nsec) g_focus_row += refresh_continue(g_sec, &g_nsec);
                    if (settled && g_side_page == 6) account_rows();
                    if (g_focus_row >= g_nsec) g_focus_row = g_nsec ? g_nsec - 1 : 0;
                    if (g_focus_row < 0) g_focus_row = 0;
                }
                LeaveCriticalSection(&g_lock);
                if (g_chain_on) {               /* More Videos / next / previous: straight on to that one */
                    g_chain_on = 0;
                    EnterCriticalSection(&g_lock);
                    start_play(&g_chain);
                    LeaveCriticalSection(&g_lock);
                }
                if (g_open_channel[0]) {        /* the player's channel button */
                    char id[40];
                    copy_str(id, sizeof(id), g_open_channel);
                    g_open_channel[0] = 0;
                    open_channel(id);
                }
            }
        }
        frames++;
        if (now_s() - t_report > 10) {
            say("menu: %.1f fps, thumbnails %ld ok / %ld failed", frames / (now_s() - t_report), g_thumbs_ok,
                g_thumbs_bad);
            frames = 0;
            t_report = now_s();
        }
    }
    say("BACK: quitting");
    return 0;
}

void __cdecl main(void)
{
    HANDLE t = CreateThread(NULL, 1024 * 1024, app, NULL, CREATE_SUSPENDED, NULL);
    XSetThreadProcessor(t, 0);                  /* the screen + sound loop: hardware thread 0, to itself */
    ResumeThread(t);
    WaitForSingleObject(t, INFINITE);
    p360_log_close();
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
}
