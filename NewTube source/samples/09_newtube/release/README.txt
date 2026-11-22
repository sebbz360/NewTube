NewTube v1  -  a YouTube app for the Xbox 360, in the style of 2012 YouTube
=========================================================================

Made by sebbz360. Not affiliated with, endorsed by or connected to YouTube or Google.
Needs a console that runs homebrew (RGH / JTAG / similar) and an internet connection.


INSTALLING
----------
1. Copy the "NewTube" folder from this zip to your console, for example to
      Hdd1:\Games\NewTube
   (by FTP while Aurora / FSD is open, or with a USB stick).
2. In Aurora: let it scan the Games folder (or add the folder as a scan path), then start NewTube.
   Or start NewTube\default.xex from any file manager.
3. The console must be online (YouTube, the update check and the dislike counts all need the internet).

Updating NewTube later: copy the new files over the old ones. Your history, subscriptions, playlists, saved videos
and settings are separate files and stay where they are.


WHAT IT DOES
------------
- Home (YouTube's recommended rows for your region + "Because you watched..."), Gaming, Music, Movies
- Search with an on-screen keyboard, suggestions, recent searches and filters (date, length, newest, most viewed)
- Subscriptions kept on your console (no account): every channel's Latest / Popular / Live / Playlists
- Channel pages, YouTube playlists, your own playlists, "Watch later", watch history with "Continue watching"
- The player: 240p - 720p, speed 0.25x - 2x, captions, chapters, loop, shuffle, Mixes, "Stats for nerds"
- Description panel with likes / dislikes and comments
- Live streams
- Offline Player: save videos to the hard drive and watch them without internet; also plays your own video files
  (put .mp4 / .mkv / .mov / .m4v / .ts files into NewTube\offline\ - H.264 8-bit video, AAC / AC-3 sound)
- Settings: starting quality, autoplay, captions, speed, resume, region, text size, automatic fixes


CONTROLS
--------
Menu        D-pad / left stick = move, A = play, hold A = details (description, comments), B = back,
            Y = search, LB / RB = filter (search results)
Details     right stick = scroll, LB = start a Mix, RB = save to the console, X = Watch later, Y = playlists
Player      A = pause, D-pad = the bar (seek) and the buttons, Down = more videos, B = hide / back,
            LB / RB = previous / next chapter (or 10 s back / ahead), BACK = leave the video
Text size   Settings > Text size: "Larger" makes small text easier to read (automatic on standard-definition TVs)


AUTOMATIC FIXES
---------------
YouTube changes how its service works from time to time. NewTube's YouTube knowledge lives in one script
(youtube.lua) that NewTube refreshes by itself at start from
   https://github.com/sebbz360/NewTube
so it keeps working without you copying files. It only ever keeps a new script that loads correctly; the previous
one is kept as youtube.lua.bak. You can turn this off in Settings > Automatic YouTube fixes.


WHAT IT CONNECTS TO
-------------------
- YouTube (videos, search, channels, comments)
- returnyoutubedislikeapi.com (the dislike estimates; only the video's id is sent)
- raw.githubusercontent.com (the automatic fixes)
NewTube has no account and sends nothing about you. Everything it remembers is kept in files in its own folder.


KNOWN LIMITS
------------
- 720p can stutter on busy videos (the Xbox 360 decodes the video in software); 480p is the default
- Chinese, Japanese, Korean, Arabic and Hebrew letters aren't shown yet (Latin, Greek and Cyrillic are)
- Signing in to a YouTube account is planned for a later update


CREDITS AND LICENSES (see the LICENSES folder)
----------------------------------------------
NewTube uses these free projects, with thanks:
- FFmpeg 3.4.14 (video and sound decoding) - LGPL 2.1. NewTube contains FFmpeg built for the Xbox 360; FFmpeg's
  source is at https://ffmpeg.org, and NewTube's source with the exact FFmpeg build steps is at
  https://github.com/sebbz360/NewTube (see BUILDING.md there), so it can be rebuilt with another FFmpeg.
- Mbed TLS 2.28.10 (secure connections) - Apache License 2.0
- Lua 5.1.5 (the YouTube script) - MIT license, Copyright (C) 1994-2012 Lua.org, PUC-Rio
- Liberation Sans 2.1.5 (the letters) - SIL Open Font License 1.1
- Mozilla's list of trusted certificates (cacert.pem, as packaged by curl) - Mozilla Public License 2.0
- Return YouTube Dislike (dislike estimates) - returnyoutubedislike.com
