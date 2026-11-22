--[[ youtube.lua - everything the app knows about YouTube lives HERE, not in the program.
     When YouTube changes something, a new copy of this file fixes the app (no rebuild).

     The program gives this script:
       http(method, url, headers, body)  -> status, reply text      (nil, "reason" on failure)
       log(text)                         -> a line in game:\log.txt
     and calls:
       yt_streams(video)  video = an ID ("dQw4w9WgXcQ") or a youtube.com / youtu.be address
                          -> { video = url, audio = url, title, author, seconds, height, fps }  or  nil, "reason"
]]

SCRIPT_VERSION = "2026-11-22 t"

-- WHO WE ASK AS: YouTube answers differently depending on which of its apps asks. These all give plain,
-- ready-to-use links (no JavaScript needed). If one stops working (blocked, "not a bot" check, scrambled links,
-- a reply we can't read), the next one in ORDER is tried automatically, and the one that worked is remembered
-- for the rest of the session. Checked from a PC on 2026-11-19: all three play; IOS's lists are a different,
-- huge layout, so it's only a playback backup.
local UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) " ..
           "Version/26.0 Safari/605.1.15"
local function client(name, id, version, ua, extra)
  return { name = name, id = id, version = version, ua = ua,
           ctx = '{"clientName":"' .. name .. '","clientVersion":"' .. version .. '",' .. extra ..
                 ',"userAgent":"' .. ua .. '","hl":"en","timeZone":"UTC","utcOffsetMinutes":0}' }
end

-- REGION: the country YouTube picks videos for ("gl", e.g. "GB"); "" = no country = the whole world. The program
-- calls set_region() with the one chosen in Settings (Automatic = the console's own country). The LANGUAGE stays
-- English ("hl":"en"): this script reads English words in YouTube's replies ("1.2M views", "3 days ago") and the
-- app's letters are Latin only.
REGION = ""
local visitor                         -- (declared here so a region change can drop it; used below)
function set_region(code)
  code = tostring(code or ""):upper()
  if not code:match("^%u%u$") then code = "" end
  if code ~= REGION then visitor = nil end      -- the visitor ID carries a country too: get one for the new region
  REGION = code
end
local function ctx_of(c)              -- the client's "context", with the region added
  if REGION == "" then return c.ctx end
  return (c.ctx:gsub('"hl":"en"', '"hl":"en","gl":"' .. REGION .. '"', 1))
end
CLIENTS = {
  VISIONOS = client("VISIONOS", "101", "1.02", UA,
    '"deviceMake":"Apple","deviceModel":"RealityDevice17,1","osName":"visionOS","osVersion":"26.5.23O471"'),
  ANDROID_VR = client("ANDROID_VR", "28", "1.65.10",
    "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip",
    '"deviceMake":"Oculus","deviceModel":"Quest 3","androidSdkVersion":32,"osName":"Android","osVersion":"12L"'),
  IOS = client("IOS", "5", "20.10.4", "com.google.ios.youtube/20.10.4 (iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)",
    '"deviceMake":"Apple","deviceModel":"iPhone16,2","osName":"iPhone","osVersion":"18.3.2.22D82"'),
}
ORDER = {                              -- per kind of request, first = preferred
  player = { "VISIONOS", "ANDROID_VR", "IOS" },
  next   = { "ANDROID_VR", "VISIONOS" },     -- (ANDROID_VR's "More Videos" reply is 70 KB instead of 450 KB)
  search = { "VISIONOS", "ANDROID_VR" },
  browse = { "VISIONOS", "ANDROID_VR" },
}
LINK_CHECK = true                      -- try 2 bytes of the chosen video link before handing it to the player
FORCE_FAIL = {}                        -- (testing: FORCE_FAIL.VISIONOS = true pretends that client is blocked)
local working = {}                     -- kind -> the client that last worked
WORKING = working                      -- (the same table, visible to tests and the log)

MAX_HEIGHT = 480        -- the quality a video STARTS in: 480p30 plays perfectly on the 360's decoder
MAX_CHOICE_HEIGHT = 720 -- the best offered in the player's quality menu (720p30 decodes at ~40 pictures/s)
MAX_FPS = 30            -- 60 fps is too much for the decoder

---------------------------------------------------------------- JSON (reading only)
local json = {}
do
  local sub, byte, find, char, tonumber = string.sub, string.byte, string.find, string.char, tonumber
  local esc = { ['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' }
  local value

  local function utf8(cp)
    if cp < 0x80 then return char(cp)
    elseif cp < 0x800 then return char(0xC0 + math.floor(cp / 64), 0x80 + cp % 64)
    elseif cp < 0x10000 then
      return char(0xE0 + math.floor(cp / 4096), 0x80 + math.floor(cp / 64) % 64, 0x80 + cp % 64)
    else
      return char(0xF0 + math.floor(cp / 262144), 0x80 + math.floor(cp / 4096) % 64,
                  0x80 + math.floor(cp / 64) % 64, 0x80 + cp % 64)
    end
  end

  local function skip(s, i)
    local _, e = find(s, "^[ \n\r\t]*", i)
    return e + 1
  end

  local function str(s, i)            -- i is just after the opening quote
    local parts, n = {}, 0
    while true do
      local a, b = find(s, '["\\]', i)
      if not a then error("unterminated string") end
      if a > i then n = n + 1; parts[n] = sub(s, i, a - 1) end
      if sub(s, a, a) == '"' then return table.concat(parts), a + 1 end
      local c = sub(s, a + 1, a + 1)
      if c == "u" then
        local cp = tonumber(sub(s, a + 2, a + 5), 16)
        i = a + 6
        if cp >= 0xD800 and cp <= 0xDBFF and sub(s, i, i + 1) == "\\u" then   -- a pair of UTF-16 halves
          local lo = tonumber(sub(s, i + 2, i + 5), 16)
          cp = 0x10000 + (cp - 0xD800) * 1024 + (lo - 0xDC00)
          i = i + 6
        end
        n = n + 1; parts[n] = utf8(cp)
      else
        n = n + 1; parts[n] = esc[c] or c
        i = a + 2
      end
    end
  end

  value = function(s, i)
    i = skip(s, i)
    local c = sub(s, i, i)
    if c == "{" then
      local t = {}
      i = skip(s, i + 1)
      if sub(s, i, i) == "}" then return t, i + 1 end
      while true do
        if sub(s, i, i) ~= '"' then error("key expected at " .. i) end
        local k
        k, i = str(s, i + 1)
        i = skip(s, i)
        if sub(s, i, i) ~= ":" then error("':' expected at " .. i) end
        t[k], i = value(s, i + 1)
        i = skip(s, i)
        c = sub(s, i, i)
        if c == "}" then return t, i + 1 end
        if c ~= "," then error("',' expected at " .. i) end
        i = skip(s, i + 1)
      end
    elseif c == "[" then
      local t, n = {}, 0
      i = skip(s, i + 1)
      if sub(s, i, i) == "]" then return t, i + 1 end
      while true do
        n = n + 1
        t[n], i = value(s, i)
        i = skip(s, i)
        c = sub(s, i, i)
        if c == "]" then return t, i + 1 end
        if c ~= "," then error("',' expected at " .. i) end
        i = i + 1
      end
    elseif c == '"' then return str(s, i + 1)
    elseif sub(s, i, i + 3) == "true" then return true, i + 4
    elseif sub(s, i, i + 4) == "false" then return false, i + 5
    elseif sub(s, i, i + 3) == "null" then return nil, i + 4
    else
      local a, b = find(s, "^-?%d+%.?%d*[eE]?[-+]?%d*", i)
      if not a then error("unexpected '" .. c .. "' at " .. i) end
      return tonumber(sub(s, a, b)), b + 1
    end
  end

  function json.decode(s)
    local ok, v = pcall(value, s, 1)
    if ok then return v end
    return nil, v
  end
end

local function quote(s)              -- a Lua string as a JSON string
  return '"' .. s:gsub('[%c"\\]', function(c) return string.format("\\u%04x", c:byte()) end) .. '"'
end

---------------------------------------------------------------- YouTube

-- (visitor: declared with REGION above) one visitor ID works for many videos: fetched once per session / region

local function get_visitor()
  if visitor then return visitor end
  -- the service-worker data file: 3 KB (the watch page would be 1.3 MB)
  local status, body = http("GET", "https://www.youtube.com/sw.js_data",
                            "User-Agent: " .. UA .. "\r\nCookie: PREF=hl=en&tz=UTC" ..
                            (REGION ~= "" and "&gl=" .. REGION or "") .. "; SOCS=CAI\r\n", nil)
  if status ~= 200 then return nil, "visitor ID: HTTP " .. tostring(status) .. " " .. tostring(body) end
  visitor = body:match('"(Cg[%w_%-%%=]+)"')
  if not visitor or #visitor < 20 then visitor = nil; return nil, "visitor ID not found in sw.js_data" end
  log("visitor ID " .. visitor:sub(1, 12) .. "... (" .. #visitor .. " chars)")
  return visitor
end

local function video_id(v)
  return v:match("[?&]v=([%w_%-]+)") or v:match("youtu%.be/([%w_%-]+)") or v:match("/shorts/([%w_%-]+)")
      or v:match("^([%w_%-]+)$")
end

-- ask YouTube's API (kind = "player" / "next" / "search" / "browse"; fields = the JSON after the client, starting
-- with a comma). check(reply table, reply text, client name) decides whether the answer is usable:
--   returns result                -> done (this client is remembered)
--   returns nil, why              -> try the next client
--   returns nil, why, true        -> stop: no client will do better (e.g. the video was deleted)
--   returns result, nil, nil, true -> usable but empty: kept, the next client is still tried
-- A "not a bot" answer from every client = the visitor ID went stale: one fresh ID, one more round.
local function ask(kind, fields, check)
  local first_err, empty
  for round = 1, 2 do
    local vis, err = get_visitor()
    if not vis then return nil, err end
    local order, bot = {}, false
    if working[kind] then order[1] = working[kind] end
    for _, n in ipairs(ORDER[kind]) do if n ~= working[kind] then order[#order + 1] = n end end
    for _, name in ipairs(order) do
      local c = CLIENTS[name]
      local status, reply
      if FORCE_FAIL[name] then
        status, reply = 403, "(FORCE_FAIL test)"
      else
        local headers = "User-Agent: " .. c.ua .. "\r\nContent-Type: application/json\r\n" ..
                        "X-Youtube-Client-Name: " .. c.id .. "\r\nX-Youtube-Client-Version: " .. c.version ..
                        "\r\nOrigin: https://www.youtube.com\r\nX-Goog-Visitor-Id: " .. vis .. "\r\n"
        status, reply = http("POST", "https://www.youtube.com/youtubei/v1/" .. kind .. "?prettyPrint=false", headers,
                             '{"context":{"client":' .. ctx_of(c) .. '}' .. fields .. '}')
      end
      local result, why, final, soft
      if status == 200 then
        local r, jerr = json.decode(reply)
        if r then
          local ok, a, b, d, e = pcall(check, r, reply, name)
          if ok then result, why, final, soft = a, b, d, e else why = "couldn't read the reply: " .. tostring(a) end
        else
          why = "reply unreadable: " .. tostring(jerr)
        end
      else
        why = "HTTP " .. tostring(status) .. " " .. tostring(reply):sub(1, 120)
      end
      if result and not soft then
        if working[kind] ~= name then log(kind .. ": asking as " .. name .. " from now on") end
        working[kind] = name
        return result
      end
      if result then
        empty = empty or result
      else
        if why and why:find("LOGIN_REQUIRED") then bot = true end
        first_err = first_err or why
        log(kind .. " as " .. name .. ": " .. tostring(why))
        if final then return nil, why end
      end
    end
    if not bot or round == 2 then break end
    visitor = nil                                  -- every client said "not a bot": a fresh visitor ID, once
    log(kind .. ": fetching a new visitor ID and trying again")
  end
  if empty then return empty end
  return nil, first_err
end

local function link_works(url, name)            -- 2 bytes of the link: 200/206 = the player will get it too
  if not LINK_CHECK then return true end
  -- (the same User-Agent the player's stream reader sends)
  local status = http("GET", url, "User-Agent: Mozilla/5.0\r\nRange: bytes=0-1\r\n", nil)
  return status == 200 or status == 206, status
end

function yt_streams(v)
  local id = video_id(v)
  if not id then return nil, "not a video ID or address: " .. v end
  local out, err = ask("player", ',"videoId":' .. quote(id) .. ',"contentCheckOk":true,"racyCheckOk":true',
                       function(r, reply, name) return read_player(r, id, name) end)
  return out, err
end

-- LIVE NOW: YouTube's "hlsManifestUrl" = a small text list of qualities (HLS master playlist); each quality is
-- another list of ~5-second MPEG-TS pieces that keeps growing (the program's sbhls.c follows it). Only qualities
-- with picture AND sound in the same pieces are used (CODECS has both, no separate AUDIO="..." group): the
-- VISIONOS identity sends sound separately as raw AAC, which can't be kept in step -> the next identity is asked.
-- The program gets "hls:" + the quality's address.
local live_client                    -- the identity whose live list worked (the 20-second refresh asks it again)

-- the master list -> {height, url = "hls:...", fps} per height, best first (nil, why if none is usable)
local function live_qualities(master)
  local status, text = http("GET", master, "User-Agent: Mozilla/5.0\r\n", nil)
  if status ~= 200 or not text or not text:find("^#EXTM3U") then
    return nil, "live quality list answer HTTP " .. tostring(status)
  end
  local by_height, info = {}, nil
  for line in text:gmatch("[^\r\n]+") do
    if line:find("^#EXT%-X%-STREAM%-INF:") then info = line
    elseif info and not line:find("^#") then
      local codecs = info:match('CODECS="([^"]*)"') or ""
      local h = tonumber(info:match("RESOLUTION=%d+x(%d+)")) or 0
      local fps = tonumber(info:match("FRAME%-RATE=([%d%.]+)")) or 30
      local bw = tonumber(info:match("BANDWIDTH=(%d+)")) or 0
      local lc = codecs:find("mp4a%.40%.2") and 1 or 0           -- plain AAC preferred over HE-AAC (mp4a.40.5)
      if codecs:find("avc1") and codecs:find("mp4a") and not info:find('AUDIO="') and h > 0 and
         h <= MAX_CHOICE_HEIGHT and fps <= MAX_FPS + 1 then
        local old = by_height[h]
        if not old or lc > old.lc or (lc == old.lc and bw > old.bw) then
          by_height[h] = { height = h, url = "hls:" .. line, fps = math.floor(fps + 0.5), lc = lc, bw = bw }
        end
      end
      info = nil
    end
  end
  local quality = {}
  for _, q in pairs(by_height) do quality[#quality + 1] = { height = q.height, url = q.url, fps = q.fps } end
  table.sort(quality, function(a, b) return a.height > b.height end)
  if #quality == 0 then return nil, "no live quality with picture + sound together" end
  return quality
end

function read_live(r, id, name)
  local vd = r.videoDetails or {}
  local master = (r.streamingData or {}).hlsManifestUrl
  if not master then return nil, "live stream without an HLS address" end
  local quality, why = live_qualities(master)
  if not quality then return nil, why end
  live_client = name
  local pick
  for i, q in ipairs(quality) do if not pick and q.height <= MAX_HEIGHT then pick = i end end
  pick = pick or #quality
  log(string.format("%s [%s]: LIVE \"%s\" by %s; %dp%d (%d qualities)", id, name, tostring(vd.title),
      tostring(vd.author), quality[pick].height, quality[pick].fps, #quality))
  return { video = quality[pick].url, title = vd.title, author = vd.author, seconds = 0, live = true,
           height = quality[pick].height, fps = quality[pick].fps, quality = quality, pick = pick,
           views = commas(vd.viewCount) .. " views", captions = {} }
end

-- the 20-second live refresh: fresh addresses only. The player answer is a few hundred KB; reading it all as JSON
-- every 20 s took enough processor time to make the picture stutter, so this only LOOKS for the one address in
-- the text. Anything unexpected -> the full yt_streams (slower, but always right).
function yt_live_refresh(v)
  local id = video_id(v)
  local c = live_client and CLIENTS[live_client]
  local vis = c and get_visitor()
  if c and vis and not FORCE_FAIL[live_client] then
    local headers = "User-Agent: " .. c.ua .. "\r\nContent-Type: application/json\r\n" ..
                    "X-Youtube-Client-Name: " .. c.id .. "\r\nX-Youtube-Client-Version: " .. c.version ..
                    "\r\nOrigin: https://www.youtube.com\r\nX-Goog-Visitor-Id: " .. vis .. "\r\n"
    local status, reply = http("POST", "https://www.youtube.com/youtubei/v1/player?prettyPrint=false", headers,
                               '{"context":{"client":' .. ctx_of(c) .. '},"videoId":' .. quote(id) ..
                               ',"contentCheckOk":true,"racyCheckOk":true}')
    local master = status == 200 and reply and reply:match('"hlsManifestUrl"%s*:%s*"([^"]+)"')
    if master then
      master = master:gsub("\\u0026", "&"):gsub("\\/", "/")
      local quality = live_qualities(master)
      if quality then return { quality = quality, live = true } end
    end
    log("live refresh: quick way didn't work (HTTP " .. tostring(status) .. "), asking the full way")
  end
  return yt_streams(v)
end

function read_player(r, id, name)
  local ps = r.playabilityStatus or {}
  if ps.status ~= "OK" then
    local why = "YouTube says " .. tostring(ps.status) .. ": " .. tostring(ps.reason)
    -- ERROR = removed / private / doesn't exist: no other client will find it either
    return nil, why, ps.status == "ERROR"
  end
  local vd = r.videoDetails or {}
  if vd.isLive then return read_live(r, id, name) end
  local fmts = (r.streamingData or {}).adaptiveFormats or {}
  local by_height, best_a = {}, nil         -- the best H.264 stream for each picture height (the quality menu)
  for _, f in ipairs(fmts) do
    local mime = f.mimeType or ""
    if f.url then
      local h = f.height or 0
      if mime:find('^video/mp4; codecs="avc1') and h <= MAX_CHOICE_HEIGHT and (f.fps or 0) <= MAX_FPS then
        if not by_height[h] or (f.bitrate or 0) > (by_height[h].bitrate or 0) then by_height[h] = f end
      elseif mime:find("^audio/mp4") and not f.isDrc then          -- AAC; skip the volume-levelled copies
        if not best_a or audio_rank(f) > audio_rank(best_a) or
           (audio_rank(f) == audio_rank(best_a) and (f.bitrate or 0) > (best_a.bitrate or 0)) then best_a = f end
      end
    end
  end
  local quality = {}
  for h, f in pairs(by_height) do quality[#quality + 1] = { height = h, url = f.url, fps = f.fps } end
  table.sort(quality, function(a, b) return a.height > b.height end)
  local pick
  for i, q in ipairs(quality) do if not pick and q.height <= MAX_HEIGHT then pick = i end end
  pick = pick or #quality
  if pick == 0 then
    -- (formats there but none with a plain link = scrambled: another client may give plain ones)
    return nil, "no ready-to-use H.264 video at " .. MAX_CHOICE_HEIGHT .. "p" .. MAX_FPS .. " or below (" .. #fmts ..
                " formats)"
  end
  local best_v = quality[pick]
  local ok, st = link_works(best_v.url, name)
  if not ok then return nil, "the video link was refused (HTTP " .. tostring(st) .. ")" end
  log(string.format("%s [%s]: \"%s\" by %s, %s s; video %dp%d (%d qualities), audio itag %s %s",
      id, name, tostring(vd.title), tostring(vd.author), tostring(vd.lengthSeconds), best_v.height, best_v.fps or 0,
      #quality, best_a and tostring(best_a.itag) or "none",
      best_a and best_a.audioTrack and ("(" .. tostring(best_a.audioTrack.displayName) .. ")") or ""))
  local out = { video = best_v.url, audio = best_a and best_a.url or nil, title = vd.title, author = vd.author,
                seconds = tonumber(vd.lengthSeconds), height = best_v.height, fps = best_v.fps,
                quality = quality, pick = pick, views = commas(vd.viewCount) .. " views" }
  storyboard(r, out, tonumber(vd.lengthSeconds) or 0)
  -- caption tracks: { name = "English (auto-generated)", url = ... } (downloaded only when chosen: yt_captions)
  out.captions = {}
  local ct = ((r.captions or {}).playerCaptionsTracklistRenderer or {}).captionTracks or {}
  for _, t in ipairs(ct) do
    if t.baseUrl then
      local u = t.baseUrl
      if u:sub(1, 1) == "/" then u = "https://www.youtube.com" .. u end
      local name = t.name and (t.name.simpleText or (t.name.runs and t.name.runs[1] and t.name.runs[1].text)) or
                   t.languageCode or "?"
      out.captions[#out.captions + 1] = { name = name, url = u }
    end
  end
  return out
end

-- one caption track -> list of { start = s, dur = s, text = "..." } (YouTube's plain timed-text XML:
-- <text start="2.46" dur="4.4">because we need to discuss</text>; HTML-escaped, sometimes with <font> tags inside)
local function unescape(s)
  for _ = 1, 2 do                      -- (some tracks are escaped twice: "&amp;#39;")
    s = s:gsub("&lt;", "<"):gsub("&gt;", ">"):gsub("&quot;", '"'):gsub("&#39;", "'")
         :gsub("&#(%d+);", function(n) n = tonumber(n); return n < 128 and string.char(n) or "" end)
         :gsub("&amp;", "&")
  end
  return s
end

function yt_captions(url)
  local status, body = http("GET", url, "User-Agent: Mozilla/5.0\r\n", nil)
  if status ~= 200 then return nil, "captions: HTTP " .. tostring(status) end
  local out = {}
  for start, dur, text in body:gmatch('<text start="([%d%.]+)" dur="([%d%.]+)"[^>]*>(.-)</text>') do
    text = unescape(text):gsub("<[^>]*>", "")                       -- no colour / bold tags
    text = text:gsub("\226\128\139", "")                            -- (zero-width spaces)
    text = text:gsub("^%s+", ""):gsub("%s+$", "")
    local last = out[#out]
    if text ~= "" and not (last and last.text == text and last.start == tonumber(start)) then   -- (styled copies)
      out[#out + 1] = { start = tonumber(start), dur = tonumber(dur), text = text }
    end
  end
  log(string.format("captions: %d lines (%d bytes)", #out, #body))
  return out
end

-- many videos carry YouTube's dubbed sound tracks (Arabic, German, Hindi ... at the same quality as the real one):
-- the ORIGINAL language wins (YouTube marks it audioIsDefault, its name ends in "original"), then a video's only track
function audio_rank(f)
  local t = f.audioTrack
  if not t then return 1 end                                        -- just one track: that's the original
  if t.audioIsDefault then return 3 end
  if tostring(t.displayName or ""):lower():find("original") then return 2 end
  return 0                                                          -- a dub
end

function commas(n)                    -- "1234567" -> "1,234,567"
  local s = tostring(n or "0"):match("%d+") or "0"
  return (s:reverse():gsub("(%d%d%d)", "%1,"):reverse():gsub("^,", ""))
end

-- the preview pictures shown while seeking ("storyboards"): sheets of small frames, one frame every few seconds.
-- spec = "<url with $L and $N>|w#h#count#cols#rows#interval_ms#name#sigh|..." (one part per size). We take the
-- largest size up to 160 wide. Sheet k's address = sb_pre .. k .. sb_post (sb_numbered = 0: only one sheet)
function storyboard(r, out, secs)
  local spec = (((r.storyboards or {}).playerStoryboardSpecRenderer) or {}).spec
  if not spec then return end
  local parts = {}
  for p in (spec .. "|"):gmatch("([^|]*)|") do parts[#parts + 1] = p end
  local best, best_l
  for i = 2, #parts do
    local w, h, count, cols, rows, interval, name, sigh =
      parts[i]:match("^(%d+)#(%d+)#(%d+)#(%d+)#(%d+)#(%d+)#([^#]*)#(.*)$")
    if w and tonumber(w) <= 160 and (not best or tonumber(w) >= best.w) then
      best = { w = tonumber(w), h = tonumber(h), count = tonumber(count), cols = tonumber(cols), rows = tonumber(rows),
               interval = tonumber(interval), name = name, sigh = sigh }
      best_l = i - 2
    end
  end
  if not best or best.count < 1 then return end
  local url = parts[1]:gsub("%$L", tostring(best_l))
  local a, b = url:match("^(.-)%$N(.*)$")
  if not a then return end
  local na, nb = best.name:match("^(.-)%$M(.*)$")
  local sig = best.sigh ~= "" and ("&sigh=" .. best.sigh) or ""
  if na then
    out.sb_pre, out.sb_post, out.sb_numbered = a .. na, nb .. b .. sig, 1
  else
    out.sb_pre, out.sb_post, out.sb_numbered = a .. best.name .. b .. sig, "", 0
  end
  out.sb_w, out.sb_h, out.sb_count, out.sb_cols, out.sb_rows = best.w, best.h, best.count, best.cols, best.rows
  -- interval 0 = the frames are spread evenly over the whole video
  out.sb_interval = best.interval > 0 and best.interval / 1000 or (secs > 0 and secs / best.count or 10)
end

---------------------------------------------------------------- lists (search + the menu pages)

-- search filters ("params"): sort by view count + uploaded today / this week + videos only
local TODAY_TOP = "CAMSBAgCEAE="
local WEEK_TOP = "CAMSBAgDEAE="

-- what each sidebar page shows: sections of { title, search words, filter }. Change freely.
PAGES = {
  Home = {
    { "Most Viewed Today", "official video", TODAY_TOP },
    { "Popular Music This Week", "official music video", WEEK_TOP },
    { "Popular in Gaming", "gaming", WEEK_TOP },
    { "New Movie Trailers", "official trailer", WEEK_TOP },
  },
  Gaming = {
    { "Most Viewed in Gaming", "gaming", WEEK_TOP },
    { "Minecraft", "minecraft", WEEK_TOP },
    { "Grand Theft Auto", "gta", WEEK_TOP },
    { "Let's Plays", "let's play", WEEK_TOP },
    { "Speedruns", "speedrun", WEEK_TOP },
  },
  Music = {
    { "Top Music Videos This Week", "official music video", WEEK_TOP },
    { "Hip Hop", "hip hop official video", WEEK_TOP },
    { "Pop", "pop official video", WEEK_TOP },
    { "Rock", "rock official video", WEEK_TOP },
    { "Electronic", "edm official video", WEEK_TOP },
  },
  Movies = {
    { "New Trailers", "official trailer", WEEK_TOP },
    { "Movie Clips", "movie clip", WEEK_TOP },
    { "Movie Reviews", "movie review", WEEK_TOP },
  },
}

function yt_page(name)        -- -> list of { title = , query = , filter = }
  local out = {}
  -- HOME = YouTube's own home feed for the chosen region ("Recommended", "Recently uploaded", topic shelves),
  -- fetched in one go; its rows are handed out by yt_search(i, "@home"). If YouTube won't give it: the search rows.
  if name == "Home" and home_feed then
    local ok, rows = pcall(home_feed)
    if ok and rows and #rows >= 2 then
      for i, r in ipairs(rows) do out[#out + 1] = { title = r.title, query = tostring(i), filter = "@home" } end
      return out
    end
    log("home feed unavailable (" .. tostring(ok and "too few rows" or rows) .. "): using the search rows")
  end
  for _, s in ipairs(PAGES[name] or {}) do out[#out + 1] = { title = s[1], query = s[2], filter = s[3] } end
  return out
end

-- YouTube's text comes in several shapes: "x", {simpleText="x"}, {runs={{text="x"},...}}, and in the newer
-- layouts {content="x"} or {text={content="x"}}
local function txt(t)
  if type(t) ~= "table" then return t and tostring(t) or "" end
  if t.simpleText then return t.simpleText end
  if t.runs then
    local p = {}
    for i, r in ipairs(t.runs) do p[i] = r.text or "" end
    return table.concat(p)
  end
  if type(t.content) == "string" then return t.content end
  if t.text then return txt(t.text) end
  return ""
end

local function find_key(o, key, depth)   -- the first table under `key` anywhere inside o
  depth = depth or 0
  if depth > 30 or type(o) ~= "table" then return nil end
  if type(o[key]) == "table" then return o[key] end
  for _, x in pairs(o) do
    local r = find_key(x, key, depth + 1)
    if r then return r end
  end
end

local function all_text(o, out, depth)   -- every piece of text inside a block (for layouts we only half know)
  depth = depth or 0
  if depth > 25 or type(o) ~= "table" then return out end
  for k, x in pairs(o) do
    if (k == "content" or k == "text" or k == "simpleText") and type(x) == "string" then out[#out + 1] = x
    elseif k ~= "accessibility" and k ~= "accessibilityLabel" then all_text(x, out, depth + 1) end
  end
  return out
end

local function seconds(d)        -- "1:02:15" -> 3735
  local s = 0
  for part in tostring(d):gmatch("%d+") do s = s * 60 + tonumber(part) end
  return s
end

-- a YouTube playlist as a tile: id = the video its picture comes from (so thumbnails work as for videos), list = the
-- playlist's id, duration = "40 videos" (shown where a video shows its length)
local function playlist_tile(p)
  local url = (((p.thumbnail or {}).thumbnails or {})[1] or {}).url or ""
  local count = txt(p.videoCountShortText)
  if count == "" then count = txt(p.videoCountText):match("[%d,]+") or "" end
  return { id = url:match("/vi/([^/]+)/") or "", list = p.playlistId, title = txt(p.title),
           channel = txt(p.shortBylineText or p.ownerText), views = "Playlist", age = "",
           duration = count ~= "" and (count .. " videos") or "Playlist", seconds = 0 }
end

-- ONE VIDEO TILE from whatever block YouTube sent. Known names (compactVideoRenderer ...) AND any block that simply
-- has a videoId + a title + a length are read: when YouTube renames a block, the fields inside usually stay.
-- Field names seen: title / headline; ownerText / shortBylineText / longBylineText / bylineText; viewCountText /
-- shortViewCountText; publishedTimeText; videoInfo or metadataText ("268K views · 3 weeks ago"); lengthText or the
-- time label on the picture (thumbnailOverlays).
local DOT = "\194\183"                 -- "·"
local function split_meta(s)            -- "268K views · 3 weeks ago" / "17M views • 14 years ago" -> views, age
  local views, age = "", ""
  for part in (s .. DOT):gsub("\226\128\162", DOT):gmatch("(.-)" .. DOT) do
    part = part:gsub("^%s+", ""):gsub("%s+$", "")
    if part:find("view") then views = part elseif part:find("ago") or part:find("Streamed") then age = part end
  end
  return views, age
end

local function read_video(v)
  if type(v.videoId) ~= "string" or #v.videoId ~= 11 then return nil end
  local title = txt(v.title or v.headline)
  if title == "" then return nil end
  local dur, live = txt(v.lengthText), false
  if dur == "" then
    for _, ov in ipairs(v.thumbnailOverlays or {}) do
      if type(ov) == "table" and ov.thumbnailOverlayTimeStatusRenderer then
        local o = ov.thumbnailOverlayTimeStatusRenderer
        dur = txt(o.text)
        if o.style == "LIVE" then live = true end
      end
    end
  end
  local views, age = txt(v.viewCountText or v.shortViewCountText), txt(v.publishedTimeText)
  -- LIVE NOW: no length; the picture says "Live now" (style LIVE) and the count says "782 watching"
  if live or (dur == "" and views:find("watching")) then
    return { id = v.videoId, title = title, views = views, age = "", duration = "LIVE", seconds = 0, live = true,
             channel = txt(v.ownerText or v.shortBylineText or v.longBylineText or v.bylineText) }
  end
  local secs = seconds(dur)
  if dur == "" or not dur:find("^%d+:%d") or secs <= 60 then return false end   -- no Shorts
  if views == "" and v.videoInfo and v.videoInfo.runs then          -- a playlist's video: "17M views", " • ", "14 years ago"
    views = (v.videoInfo.runs[1] or {}).text or ""
    age = (v.videoInfo.runs[3] or {}).text or ""
  end
  if views == "" and v.metadataText then views, age = split_meta(txt(v.metadataText)) end
  return { id = v.videoId, title = title, views = views, age = age, duration = dur, seconds = secs,
           channel = txt(v.ownerText or v.shortBylineText or v.longBylineText or v.bylineText) }
end

-- the newest layout ("lockups", already used by YouTube's TV app): contentId + title + rows of small text parts
local function read_lockup(o, lists)
  local m = (o.metadata or {}).lockupMetadataViewModel or {}
  local title = txt(m.title)
  if title == "" then return nil end
  local parts = {}
  for _, row in ipairs(((m.metadata or {}).contentMetadataViewModel or {}).metadataRows or {}) do
    for _, p in ipairs(row.metadataParts or {}) do parts[#parts + 1] = txt(p.text) end
  end
  local channel, views, age = "", "", ""
  for _, p in ipairs(parts) do
    if p:find("view") then views = p elseif p:find("ago") or p:find("Streamed") then age = p
    elseif channel == "" then channel = p end
  end
  local labels = all_text(o.contentImage or {}, {})
  if o.contentType == "LOCKUP_CONTENT_TYPE_VIDEO" then
    local dur = ""
    local live = false
    for _, s in ipairs(labels) do
      if s:find("^%d+:%d%d") then dur = s elseif s:upper() == "LIVE" or s == "Live now" then live = true end
    end
    if dur == "" and (live or views:find("watching")) then
      return { id = o.contentId, title = title, channel = channel, views = views, age = "", duration = "LIVE",
               seconds = 0, live = true }
    end
    local secs = seconds(dur)
    if dur == "" or secs <= 60 then return false end
    return { id = o.contentId, title = title, channel = channel, views = views, age = age, duration = dur, seconds = secs }
  elseif lists and o.contentType == "LOCKUP_CONTENT_TYPE_PLAYLIST" then
    local pic = ""
    for _, s in ipairs({ find_key(o.contentImage or {}, "sources") or {} }) do
      local first = s[1] or {}
      pic = tostring(first.url or ""):match("/vi/([^/]+)/") or ""
    end
    local count = ""
    for _, s in ipairs(labels) do if s:find("video") then count = s end end
    if pic == "" then return false end
    return { id = pic, list = o.contentId, title = title, channel = channel, views = "Playlist", age = "",
             duration = count ~= "" and count or "Playlist", seconds = 0 }
  end
  return false
end

local function collect(o, found, depth, lists)   -- lists = also take playlists (search results, a channel's playlists)
  if depth > 40 or type(o) ~= "table" then return end
  if lists and (o.compactPlaylistRenderer or o.playlistRenderer or o.gridPlaylistRenderer) then
    local p = o.compactPlaylistRenderer or o.playlistRenderer or o.gridPlaylistRenderer
    if p.playlistId then
      local t = playlist_tile(p)
      if t.id ~= "" then found[#found + 1] = t end
    end
    return
  end
  if o.lockupViewModel and type(o.lockupViewModel.contentType) == "string" then
    local t = read_lockup(o.lockupViewModel, lists)
    if t then found[#found + 1] = t end
    return
  end
  local t = read_video(o)                            -- (false = a video block, but a Short / live stream: look inside
  if t then found[#found + 1] = t; return end       --  anyway, in case it's a block that holds other videos)
  for _, x in pairs(o) do collect(x, found, depth + 1, lists) end
end

function yt_search(query, filter)   -- -> list of videos { id, title, channel, views, age, duration, seconds }
  -- the Subscriptions page asks through here too: "@latest" = newest uploads of all subscriptions, "@channel" =
  -- one subscribed channel's uploads (query = its id). See sub_videos further down.
  if filter == "@latest" or filter == "@channel" then return sub_videos(query, filter) end
  if filter and filter:sub(1, 4) == "@ch_" then return channel_row(query, filter) end   -- (one channel's rows)
  if filter == "@related" then                       -- "Because you watched ...": the videos YouTube lists next to it
    local n, err = yt_next(query)
    if not n then return nil, err end
    log(string.format("related to %s: %d videos", query, #n.related))
    return n.related
  end
  if filter == "@home" then                          -- a row of the home feed (already fetched by yt_page)
    local row = HOME_ROWS[tonumber(query) or 0]
    if not row then return nil, "the home feed has to be loaded again" end
    return row.videos
  end
  local found, err = ask("search", ',"query":' .. quote(query) ..
                         (filter and filter ~= "" and (',"params":' .. quote(filter)) or ""),
    function(r)
      local f = {}
      collect(r, f, 0, true)                  -- (playlists too: their tiles open the playlist)
      return f, nil, nil, #f == 0             -- nothing readable: maybe another client's layout reads better
    end)
  if not found then return nil, "search: " .. tostring(err) end
  -- the same video can appear twice (e.g. in a "people also watched" shelf): keep the first
  local seen, out, lists = {}, {}, 0
  for _, v in ipairs(found) do
    local key = v.list or v.id
    if not seen[key] then
      seen[key] = true
      out[#out + 1] = v
      if v.list then lists = lists + 1 end
    end
  end
  log(string.format("search %q: %d videos, %d playlists", query, #out - lists, lists))
  return out
end

-- the thumbnail address for a video (320 x 180 JPEG)
function yt_thumb(id)
  return "https://i.ytimg.com/vi/" .. id .. "/mqdefault.jpg"
end

-- everything around a video: details for the info panel + the "More Videos" list
function yt_next(v)
  local id = video_id(v)
  local out, err = ask("next", ',"videoId":' .. quote(id) .. ',"contentCheckOk":true,"racyCheckOk":true',
    function(r)
      local o = read_next(r, id)
      return o, nil, nil, (#o.related == 0 and not o.channel)     -- nothing readable: try the other client
    end)
  if not out then return nil, "next API: " .. tostring(err) end
  log(string.format("next %s [%s]: %d related videos, %s likes, %s", id, tostring(working.next), #out.related,
      tostring(out.likes), tostring(out.subs)))
  return out
end

-- COMMENTS (the description panel, owner 2026-11-22). The next reply holds a "comment-item-section" with a
-- continuation token; asking next with that token gives 20 comments (ANDROID_VR / VISIONOS: the classic
-- commentThreadRenderer > commentRenderer layout) + a token for the next 20. Replies to comments: only their count.
local function token_in(o, depth)               -- the first continuation token inside o
  if depth > 25 or type(o) ~= "table" then return nil end
  if o.continuationCommand and o.continuationCommand.token then return o.continuationCommand.token end
  if o.nextContinuationData and o.nextContinuationData.continuation then return o.nextContinuationData.continuation end
  if o.reloadContinuationData and o.reloadContinuationData.continuation then return o.reloadContinuationData.continuation end
  for i = 1, #o do                              -- (lists in order: the first sort option = "Top")
    local t = token_in(o[i], depth + 1)
    if t then return t end
  end
  for _, x in pairs(o) do
    local t = token_in(x, depth + 1)
    if t then return t end
  end
end

function comments_token(r)      -- -> token, "2.4M" (how many comments)
  local function walk(o, depth)
    if depth > 30 or type(o) ~= "table" then return nil end
    local p = o.engagementPanelSectionListRenderer      -- (2026: the comments panel; its sort menu holds the tokens)
    if p and p.panelIdentifier == "comment-item-section" then
      local h = (p.header or {}).engagementPanelTitleHeaderRenderer or {}
      return token_in(h.menu or p, 0), txt(h.contextualInfo)
    end
    local s = o.itemSectionRenderer
    if s and s.sectionIdentifier == "comment-item-section" then return token_in(s, 0) end
    for _, x in pairs(o) do
      local t = walk(x, depth + 1)
      if t then return t end
    end
  end
  return walk(r, 0)
end

-- -> { comments = { {author, text, age, likes, replies, avatar}, ... }, next = token or nil } or nil, why
function yt_comments(token)
  local out, err = ask("next", ',"continuation":' .. quote(token), function(r)
    local list, nxt = {}, nil
    local function walk(o, depth)
      if depth > 40 or type(o) ~= "table" then return end
      if o.commentThreadRenderer then           -- one comment (its replies aren't opened)
        local c = ((o.commentThreadRenderer.comment or {}).commentRenderer)
        if c then
          local av
          for _, t in ipairs(((c.authorThumbnail or {}).thumbnails) or {}) do
            if not av or (t.width or 0) <= 48 then av = t.url end
          end
          if av and av:sub(1, 2) == "//" then av = "https:" .. av end
          list[#list + 1] = { author = txt(c.authorText), text = txt(c.contentText), age = txt(c.publishedTimeText),
                              likes = txt(c.voteCount), replies = tonumber(c.replyCount) or 0, avatar = av or "" }
        end
        return
      end
      if o.continuationItemRenderer and not nxt then nxt = token_in(o.continuationItemRenderer, 0) end
      if o.nextContinuationData and not nxt then nxt = o.nextContinuationData.continuation end
      for i = 1, #o do walk(o[i], depth + 1) end
      for k, x in pairs(o) do if type(k) ~= "number" then walk(x, depth + 1) end end
    end
    walk(r.continuationContents or r.onResponseReceivedEndpoints or r, 0)
    return { comments = list, next = nxt }, nil, nil, #list == 0
  end)
  if not out then return nil, "comments: " .. tostring(err) end
  log(string.format("comments: %d (more: %s)", #out.comments, out.next and "yes" or "no"))
  return out
end

function read_next(r, id)
  local out = { related = {} }
  local function meta(o, depth)              -- the videoMetadataRenderer block (details of THIS video)
    if depth > 30 or type(o) ~= "table" then return end
    if o.videoMetadataRenderer then
      local m = o.videoMetadataRenderer
      out.title = txt(m.title)
      out.views = txt(m.viewCountText)
      out.age = txt(m.dateText)
      out.published = txt(m.publishedTimeText)
      out.desc = txt(m.description)
      local like = ((m.likeButton or {}).likeButtonRenderer or {}).likeCount
      out.likes = like and commas(like) or ""
      local owner = (m.owner or {}).videoOwnerRenderer or {}
      out.channel = txt(owner.title)
      out.channel_id = (((owner.navigationEndpoint or {}).browseEndpoint) or {}).browseId
      out.subs = txt(owner.subscriberCountText)
      for _, t in ipairs(((owner.thumbnail or {}).thumbnails) or {}) do
        if not out.avatar or (t.width or 0) <= 88 then out.avatar = t.url end
      end
      return
    end
    for _, x in pairs(o) do meta(x, depth + 1) end
  end
  meta(r, 0)
  local found = {}
  collect(r, found, 0)
  local seen = { [id] = true }
  for _, f in ipairs(found) do
    if not seen[f.id] then seen[f.id] = true; out.related[#out.related + 1] = f end
  end
  if out.avatar and out.avatar:sub(1, 2) == "//" then out.avatar = "https:" .. out.avatar end
  if out.published == "" then out.published = out.age end     -- (ANDROID_VR only sends the date)
  out.comments_token, out.comment_count = comments_token(r)   -- (where the comments start: yt_comments)
  return out
end

-- a channel page: header (name, handle, subscribers, video count, avatar, banner), its total views (About tab) and
-- its home-tab video shelves -> { title, handle, subs, videos, views, avatar, banner, sections = { {title, videos} } }
-- -> reply table, reply text. check(r, text) may say a reply is unusable (then the next client is asked);
-- without one, any reply with at least one video / playlist in it counts
local function browse(id, params, continuation, check)   -- (continuation: the next page of a long list instead)
  local fields = continuation and (',"continuation":' .. quote(continuation))
                 or (',"browseId":' .. quote(id) .. (params and (',"params":' .. quote(params)) or ""))
  local res, err = ask("browse", fields, function(r, text)
    if check then
      local ok, why = check(r, text)
      if not ok then return nil, why end
      return { r = r, text = text }
    end
    local f = {}
    collect(r, f, 0, true)
    return { r = r, text = text }, nil, nil, #f == 0
  end)
  if not res then return nil, err end
  return res.r, res.text
end

-- the HOME FEED ("FEwhat_to_watch", what YouTube's apps show on their home screen) for REGION, without an account:
-- shelves of videos -> HOME_ROWS = { { title = "Recommended", videos = {...} }, ... } (empty shelves left out).
-- Checked 2026-11-21: ~700 KB, 5 shelves / ~58 videos, different per country (US / BR / IN)
HOME_ROWS = {}
function home_feed()
  local r, err = browse("FEwhat_to_watch")
  if not r then return nil, err end
  local rows, seen = {}, {}
  local function shelves(o, depth)
    if depth > 30 or type(o) ~= "table" then return end
    local sh = o.shelfRenderer or o.richShelfRenderer
    if sh then
      local f, vids = {}, {}
      collect(sh.content or sh.contents or sh, f, 0)
      for _, v in ipairs(f) do
        if not seen[v.id] then seen[v.id] = true; vids[#vids + 1] = v end    -- (each video once on the page)
      end
      local title = txt(sh.title)
      if #vids > 0 then rows[#rows + 1] = { title = title ~= "" and title or "Recommended", videos = vids } end
      return
    end
    for i = 1, #o do shelves(o[i], depth + 1) end   -- (lists in order: the shelves keep YouTube's order)
    for k, x in pairs(o) do if type(k) ~= "number" then shelves(x, depth + 1) end end
  end
  shelves(r, 0)
  HOME_ROWS = rows
  log(string.format("home feed (%s): %d rows", REGION ~= "" and REGION or "worldwide", #rows))
  return rows
end

local function pick_thumb(list, want)   -- the smallest picture at least `want` wide (else the biggest there is)
  local best, biggest
  for _, t in ipairs(list or {}) do
    local w = t.width or 0
    if w >= want and (not best or w < (best.width or 0)) then best = t end
    if not biggest or w > (biggest.width or 0) then biggest = t end
  end
  best = best or biggest
  local u = best and best.url
  if u and u:sub(1, 2) == "//" then u = "https:" .. u end
  return u
end

-- the channel header: the classic layout (c4TabbedHeaderRenderer) or the newer one (pageHeaderRenderer: a title,
-- rows of small text parts "@handle" / "18.3M subscribers" / "7.2K videos", avatar + banner picture sources)
local function read_channel_header(r)
  local h = (r.header or {}).c4TabbedHeaderRenderer
  if h then
    return { title = txt(h.title), handle = txt(h.channelHandleText), subs = txt(h.subscriberCountText),
             videos = txt(h.videosCountText), avatar = pick_thumb((h.avatar or {}).thumbnails, 176),
             banner = pick_thumb((h.tvBanner or h.banner or {}).thumbnails, 1280) }
  end
  local p = (r.header or {}).pageHeaderRenderer
  local md = (r.metadata or {}).channelMetadataRenderer or {}
  if not p and not md.title then return nil end
  p = p or {}
  local vm = (p.content or {}).pageHeaderViewModel or {}
  local out = { title = txt(p.pageTitle), handle = "", subs = "", videos = "" }
  if out.title == "" then out.title = txt(find_key(vm.title or {}, "text") or vm.title) end
  if out.title == "" then out.title = tostring(md.title or "") end
  for _, row in ipairs(((vm.metadata or {}).contentMetadataViewModel or {}).metadataRows or {}) do
    for _, part in ipairs(row.metadataParts or {}) do
      local s = txt(part.text)
      if s:sub(1, 1) == "@" then out.handle = s
      elseif s:find("subscriber") then out.subs = s
      elseif s:find("video") then out.videos = s end
    end
  end
  out.avatar = pick_thumb(find_key(vm.image or {}, "sources"), 176) or
               pick_thumb((md.avatar or {}).thumbnails, 176)
  out.banner = pick_thumb(find_key(vm.banner or {}, "sources"), 1280)
  return out
end

local function shelves(o, out, depth)     -- every shelf of videos anywhere in the reply (not inside another shelf)
  if depth > 30 or type(o) ~= "table" then return end
  local s = o.shelfRenderer or o.richShelfRenderer
  if s then out[#out + 1] = s; return end
  for _, x in pairs(o) do shelves(x, out, depth + 1) end
end

function yt_channel(id)
  local r, err = browse(id, nil, nil, function(r)
    if read_channel_header(r) then return true end
    return false, "no channel header NewTube can read"
  end)
  if not r then return nil, err end
  local out = read_channel_header(r)
  out.sections = {}
  -- the home tab's shelves of videos, in YouTube's order (shelves of channels / Shorts end up empty: skipped)
  local list = {}
  local contents = r.contents or {}
  local tabs = ((contents.singleColumnBrowseResultsRenderer or contents.twoColumnBrowseResultsRenderer or {}).tabs) or {}
  local home = tabs[1] and tabs[1].tabRenderer and tabs[1].tabRenderer.content
  shelves(home or contents, list, 0)
  for _, shelf in ipairs(list) do
    local found = {}
    collect(shelf.content or shelf.contents, found, 0)
    if #found > 0 then
      for _, v in ipairs(found) do if v.channel == "" then v.channel = out.title end end
      out.sections[#out.sections + 1] = { title = txt(shelf.title), videos = found }
    end
  end
  if #out.sections == 0 then                      -- no shelves we know: all the videos on the page as one row
    local found = {}
    collect(contents, found, 0)
    for _, v in ipairs(found) do if v.channel == "" then v.channel = out.title end end
    if #found > 0 then out.sections[1] = { title = "Videos", videos = found } end
  end
  local any = function() return true end           -- (a channel with no playlists is a fine answer too)
  -- the newest uploads (Videos tab) first, then past / current live streams (Live tab), if the home tab lacks them
  local function tab_row(params, title, front)
    local tr = browse(id, params, nil, any)
    if not tr then return end
    local found, seen, vids = {}, {}, {}
    collect(tr.contents, found, 0)
    for _, v in ipairs(found) do
      if not seen[v.id] then seen[v.id] = true; if v.channel == "" then v.channel = out.title end; vids[#vids + 1] = v end
    end
    if #vids == 0 then return end
    if front then table.insert(out.sections, 1, { title = title, videos = vids })
    else out.sections[#out.sections + 1] = { title = title, videos = vids } end
  end
  tab_row("EgZ2aWRlb3PyBgQKAjoA", "Latest videos", true)
  local live = channel_row(id, "@ch_live")       -- (the WEB client: the TV clients' Live tab = the Videos tab)
  if live and #live > 0 then
    for _, v in ipairs(live) do if v.channel == "" then v.channel = out.title end end
    out.sections[#out.sections + 1] = { title = "Live streams", videos = live }
  end
  -- the channel's playlists (Playlists tab) as one more shelf: their tiles open the playlist
  local pr = browse(id, "EglwbGF5bGlzdHPyBgQKAkIA", nil, any)
  if pr then
    local found, lists = {}, {}
    collect(pr.contents, found, 0, true)
    for _, v in ipairs(found) do
      if v.list then
        if v.channel == "" then v.channel = out.title end
        lists[#lists + 1] = v
      end
    end
    if #lists > 0 then out.sections[#out.sections + 1] = { title = "Playlists", videos = lists } end
  end
  -- total views: one short field in the About tab (found by text search: no need to read the whole reply)
  local _, about = browse(id, "EgVhYm91dPIGBAoCEgA=", nil, any)
  out.views = about and about:match('"viewCountText":{"runs":%[{"text":"([^"]-) views"}') or ""
  log(string.format("channel %s: %s, %s, %s videos, %s views, %d shelves", id, out.title, out.subs, out.videos,
      out.views, #out.sections))
  return out
end

-- a YouTube playlist: browse "VL" + its id gives the header + 20 videos, each continuation 20 more (up to PL_MAX)
-- -> { title, channel, count, videos }
local PL_MAX = 96

local function next_token(o, depth)
  if depth > 30 or type(o) ~= "table" then return nil end
  if o.nextContinuationData and o.nextContinuationData.continuation then return o.nextContinuationData.continuation end
  if o.continuationCommand and o.continuationCommand.token then return o.continuationCommand.token end
  for _, x in pairs(o) do
    local t = next_token(x, depth + 1)
    if t then return t end
  end
end

function yt_playlist(id)
  local r, err = browse("VL" .. id, nil)
  if not r then return nil, err end
  local h = (r.header or {}).playlistHeaderRenderer
  local out
  if h then
    out = { title = txt(h.title), channel = txt(h.ownerText), count = txt(h.numVideosText), videos = {} }
  else                                            -- (newer header: a page title + small text parts)
    local p = (r.header or {}).pageHeaderRenderer or {}
    out = { title = txt(p.pageTitle), channel = "", count = "", videos = {} }
    for _, s in ipairs(all_text(p.content or {}, {})) do
      if out.count == "" and s:find("^[%d,]+ video") then out.count = s end
    end
  end
  local seen, pages = {}, 1
  while true do
    local part = r.continuationContents or r.contents      -- (a continuation reply has both: the list is in the first)
    local found = {}
    collect(part, found, 0)
    for _, v in ipairs(found) do
      if not seen[v.id] and #out.videos < PL_MAX then seen[v.id] = true; out.videos[#out.videos + 1] = v end
    end
    local tok = next_token(part, 0)
    if not tok or #out.videos >= PL_MAX or pages >= 6 then break end
    r = browse(nil, nil, tok)
    if not r then break end
    pages = pages + 1
  end
  if out.title == "" then out.title = "Playlist" end
  log(string.format("playlist %s: %s by %s, %s, %d videos in %d pages", id, out.title, out.channel, out.count,
      #out.videos, pages))
  return out
end

-- search suggestions while typing: YouTube's suggest service (no account) answers
-- window.google.ac.h(["pengu",[["penguinz0",0,[...],{...}],["penguin trek",0,[...]], ...],{...}])
local function urlencode(s)
  return (s:gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end))
end

function yt_suggest(q)        -- -> list of strings (up to 8)
  local status, body = http("GET", "https://suggestqueries-clients6.youtube.com/complete/search?client=youtube&ds=yt" ..
                            "&hl=en" .. (REGION ~= "" and "&gl=" .. REGION or "") .. "&q=" .. urlencode(q), "User-Agent: Mozilla/5.0\r\n", nil)
  if status ~= 200 then return nil, "suggestions: HTTP " .. tostring(status) end
  local inner = body:match("^[^%(]*%((.*)%)%s*$")
  local r = inner and json.decode(inner)
  if type(r) ~= "table" or type(r[2]) ~= "table" then return nil, "suggestions unreadable" end
  local out = {}
  for _, s in ipairs(r[2]) do
    if type(s) == "table" and type(s[1]) == "string" and #out < 8 then out[#out + 1] = s[1] end
  end
  return out
end

-- LOCAL SUBSCRIPTIONS (kept on the console by NewTube itself: no YouTube account). subs() is given by the program:
-- { {id = "UC...", title = "penguinz0"}, ... }. A channel's uploads come from its Videos tab (~80 KB, newest 30).
local sub_cache = {}                   -- id -> videos, refilled each time the Subscriptions page loads ("@latest")
local LATEST_CHANNELS = 15             -- how many channels the mixed "latest" row asks (one request each)

local function channel_uploads(id, name)
  if sub_cache[id] then return sub_cache[id] end
  local r, err = browse(id, "EgZ2aWRlb3PyBgQKAjoA")
  if not r then return nil, err end
  local h = sub_avatar and read_channel_header(r)        -- (the channel's picture for the Subscriptions column)
  if h and h.avatar then sub_avatar(id, h.avatar) end
  local found, seen, out = {}, {}, {}
  collect(r, found, 0)
  for _, v in ipairs(found) do
    if not seen[v.id] then
      seen[v.id] = true
      if v.channel == "" then v.channel = name or "" end
      out[#out + 1] = v
    end
  end
  sub_cache[id] = out
  return out
end

local UNITS = { second = 1, minute = 60, hour = 3600, day = 86400, week = 604800, month = 2592000, year = 31536000 }
local function age_seconds(age)        -- "3 hours ago" -> 10800 (for sorting; unknown = very old)
  local n, unit = tostring(age):match("(%d+)%s+(%a+)")
  if not n then return 1e12 end
  unit = unit:lower():gsub("s$", "")
  return tonumber(n) * (UNITS[unit] or 1e9)
end

function sub_videos(query, filter)
  local list = subs and subs() or {}
  if filter == "@channel" then
    local name
    for _, s in ipairs(list) do if s.id == query then name = s.title end end
    local v, err = channel_uploads(query, name)
    if v then
      for _, x in ipairs(v) do x.age_s = age_seconds(x.age) end
      log(string.format("subscription %s: %d videos", tostring(name), #v))
    end
    return v, err
  end
  sub_cache = {}                       -- "@latest" comes first on the page: fresh data for every row
  local all = {}
  for i, s in ipairs(list) do
    if i > LATEST_CHANNELS then break end
    local v = channel_uploads(s.id, s.title) or {}
    for k = 1, math.min(8, #v) do all[#all + 1] = v[k] end
  end
  for _, v in ipairs(all) do v.sort_age = age_seconds(v.age) end
  table.sort(all, function(a, b) return a.sort_age < b.sort_age end)
  local out = {}
  for k = 1, math.min(24, #all) do out[k] = all[k]; out[k].age_s = all[k].sort_age end   -- (age_s: "New" badges)
  log(string.format("latest from %d subscriptions: %d videos", math.min(#list, LATEST_CHANNELS), #out))
  return out
end

-- ONE SUBSCRIBED CHANNEL's rows on the Subscriptions page (owner 2026-11-22: pick a channel in the column, its rows
-- load), each its own request (query = the channel id):
--   "@ch_latest" newest uploads (Videos tab)      "@ch_popular" most viewed (see channel_popular)
--   "@ch_live"   live / past streams (Live tab)   "@ch_lists"   its playlists (Playlists tab)
local function sub_name(id)
  for _, s in ipairs(subs and subs() or {}) do if s.id == id then return s.title end end
  return ""
end

local function tidy(found, name, lists_only)      -- each video once, the channel's name filled in
  local seen, out = {}, {}
  for _, v in ipairs(found) do
    local key = v.list or v.id
    if not seen[key] and (not lists_only or v.list) then
      seen[key] = true
      if v.channel == "" then v.channel = name end
      out[#out + 1] = v
    end
  end
  return out
end

-- POPULAR: the TV / phone clients ignore every sort setting; YouTube's WEB client shows Latest / Popular / Oldest
-- buttons on the Videos tab, each holding a "continuation" token. So: the web Videos tab (its text is only searched
-- for the Popular token: no JSON reading of ~450 KB), then that token = the list sorted by views (~400 KB).
-- Checked 2026-11-22 (penguinz0: 30M, 26M, 22M views...). Browsing only: no video links come from this client.
local WEB = client("WEB", "1", "2.20251101.00.00",
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
  '"platform":"DESKTOP"')
local function web_browse(fields)               -- -> status, reply text (the WEB client; browsing only)
  local vis = get_visitor()
  local h = "User-Agent: " .. WEB.ua .. "\r\nContent-Type: application/json\r\nX-Youtube-Client-Name: " .. WEB.id ..
            "\r\nX-Youtube-Client-Version: " .. WEB.version .. "\r\nOrigin: https://www.youtube.com\r\n" ..
            (vis and ("X-Goog-Visitor-Id: " .. vis .. "\r\n") or "")
  return http("POST", "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false", h,
              '{"context":{"client":' .. ctx_of(WEB) .. '}' .. fields .. '}')
end
local function channel_popular(id, name)
  local post = web_browse
  local status, text = post(',"browseId":' .. quote(id) .. ',"params":"EgZ2aWRlb3PyBgQKAjoA"')
  if status ~= 200 then return nil, "HTTP " .. tostring(status) .. " (popular videos)" end
  local at = text:find('"text":"Popular"', 1, true)
  local tok = at and text:match('"token":"([^"]+)"', at)
  text = nil
  if not tok then return nil, "This channel has no popular list." end
  local status2, reply = post(',"continuation":' .. quote(tok))
  if status2 ~= 200 then return nil, "HTTP " .. tostring(status2) .. " (popular videos)" end
  local r, jerr = json.decode(reply)
  reply = nil
  if not r then return nil, "reply unreadable: " .. tostring(jerr) end
  local found = {}
  collect(r, found, 0)
  return tidy(found, name)
end

function channel_row(id, filter)     -- (global: yt_search, above, calls it)
  local name = sub_name(id)
  local any = function() return true end         -- (an empty tab is a fine answer)
  local out, err
  if filter == "@ch_popular" then
    out, err = channel_popular(id, name)
  elseif filter == "@ch_live" then                -- (the TV clients ignore the Live tab and send the Videos tab:
    local status, reply = web_browse(',"browseId":' .. quote(id) .. ',"params":"EgdzdHJlYW1z8gYECgJ6AA=="')
    if status ~= 200 then return nil, "HTTP " .. tostring(status) .. " (live streams)" end   --  the WEB client is right)
    local r, jerr = json.decode(reply)
    reply = nil
    if not r then return nil, "reply unreadable: " .. tostring(jerr) end
    local found = {}
    collect(r.contents or r, found, 0)
    out = tidy(found, name)
  else
    local params = filter == "@ch_lists" and "EglwbGF5bGlzdHPyBgQKAkIA" or "EgZ2aWRlb3PyBgQKAjoA"
    local r
    r, err = browse(id, params, nil, any)
    if r then
      local h = sub_avatar and filter == "@ch_latest" and read_channel_header(r)
      if h and h.avatar then sub_avatar(id, h.avatar) end
      local found = {}
      collect(r.contents or r, found, 0, filter == "@ch_lists")
      out = tidy(found, name, filter == "@ch_lists")
    end
  end
  if out then log(string.format("channel %s %s: %d", name, filter, #out)) end
  return out, err
end

-- LIKES / DISLIKES for the playlist page's ratio bar. YouTube hides dislike counts since 2021; the community project
-- "Return YouTube Dislike" (returnyoutubedislikeapi.com, free, no key) estimates them. Only the video id is sent.
-- -> likes, dislikes (numbers) or nil, why
function yt_votes(id)
  local status, reply = http("GET", "https://returnyoutubedislikeapi.com/votes?videoId=" .. id,
                             "User-Agent: Mozilla/5.0\r\nAccept: application/json\r\n")
  if status ~= 200 then return nil, "votes: HTTP " .. tostring(status) end
  local likes = tonumber(reply:match('"likes":(%d+)'))
  local dislikes = tonumber(reply:match('"dislikes":(%d+)'))
  if not likes or not dislikes then return nil, "votes: unreadable reply" end
  return likes, dislikes
end

-- MIX: YouTube's endless auto playlist built around a video ("RD" + its id): the next API with that playlist gives
-- ~25 videos in its playlist panel. -> list of videos (the first = the video itself), or nil, why
function yt_mix(id)
  local out, err = ask("next", ',"videoId":' .. quote(id) .. ',"playlistId":' .. quote("RD" .. id) ..
                       ',"contentCheckOk":true,"racyCheckOk":true',
    function(r)
      -- the Mix's videos are playlistPanelVideoRenderer blocks (ANDROID_VR sends them; VISIONOS doesn't -> next client)
      local seen, vids = {}, {}
      local function walk(o, depth)
        if depth > 40 or type(o) ~= "table" then return end
        if o.playlistPanelVideoRenderer then
          local v = read_video(o.playlistPanelVideoRenderer)
          if v and not seen[v.id] then seen[v.id] = true; vids[#vids + 1] = v end
          return
        end
        for i = 1, #o do walk(o[i], depth + 1) end
        for k, x in pairs(o) do if type(k) ~= "number" then walk(x, depth + 1) end end
      end
      walk(r, 0)
      if #vids < 2 then return nil, "no Mix in the reply" end
      return vids
    end)
  if not out then
    -- YouTube only makes Mixes for some videos (mostly music; checked 2026-11-22: not even the website gets one for
    -- the others). Then: our own "mix" = the video + the videos YouTube lists next to it (the C side keeps topping it
    -- up from the related videos of the last one, so it still goes on)
    local n = yt_next(id)
    if not n or #n.related == 0 then return nil, "mix: " .. tostring(err) end
    out = { { id = id, title = n.title or "", channel = n.channel or "", views = n.views or "", age = n.published or "",
              duration = "", seconds = 0 } }
    for _, v in ipairs(n.related) do out[#out + 1] = v end
    log(string.format("mix for %s: YouTube has none, made one from %d related videos", id, #n.related))
    return out
  end
  log(string.format("mix for %s: %d videos", id, #out))
  return out
end

log("youtube.lua " .. SCRIPT_VERSION .. " loaded")
