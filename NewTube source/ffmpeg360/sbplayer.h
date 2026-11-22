/* sbplayer.h - a small video player engine for 360 programs, on top of FFmpeg (sbplayer.c, inside ffmpeg360.lib).

   A background thread reads the MP4 file(s), decodes the video into a queue of finished pictures and the sound into
   a buffer of 16-bit samples. The program only asks two things, many times a second:
     sbplayer_audio(...)     "give me the sound that's ready" (it feeds XAudio2 with it)
     sbplayer_video(now,..)  "which picture is due at this moment?" (now = the playback clock, in seconds)

     sbplayer_open(video_path, audio_path, log, threads, &info)
         video_path: an MP4 with video (and maybe sound). audio_path: a second file with the sound (YouTube serves
         video and audio separately), or NULL. threads = video decoder threads (1..5). 0 = OK, below 0 = error.
         "hls:https://..." = a live stream's media playlist (MPEG-TS pieces with picture + sound; sbhls.c).
     sbplayer_finished()     1 when everything has been read, decoded and handed out
     sbplayer_close()        stop the thread, free everything */
#ifndef SBPLAYER_H
#define SBPLAYER_H
#include "sbdec.h"          /* sbdec_frame: a decoded picture (Y/U/V planes) */

typedef struct {
    int has_video, width, height;
    int has_audio, sample_rate, channels;   /* the sound handed out: 16-bit, channels interleaved (L R L R ...) */
    double duration;                        /* seconds (0 = unknown) */
    int live;                               /* 1 = a live stream ("hls:" address): no seeking, no end time */
} sbplayer_info;

#ifdef __cplusplus
extern "C" {
#endif

int sbplayer_open(const char *video_path, const char *audio_path, void (*log)(const char *line), int threads,
                  sbplayer_info *info);
int sbplayer_audio(short *out, int max_frames);         /* returns how many frames (one sample per channel) it wrote */
int sbplayer_video(double now, sbdec_frame *out, double *pts);   /* 1 = a new picture is due; it stays valid until
                                                                    the next call. *pts = its time in seconds */
int sbplayer_finished(void);
/* jump to a time (seconds). Until sbplayer_seeking() says 0, no pictures / sound are handed out; then playback
   continues exactly from there (the program restarts its clock at that time) */
void sbplayer_seek(double seconds);
int sbplayer_seeking(void);
/* live only: a fresh address for the playing quality (YouTube's expire after ~40 s; see sbhls.h) */
void sbplayer_live_url(const char *url);
void sbplayer_stats(int *shown, int *dropped);          /* pictures shown / dropped (too late) so far */
void sbplayer_close(void);

#ifdef __cplusplus
}
#endif

#endif
