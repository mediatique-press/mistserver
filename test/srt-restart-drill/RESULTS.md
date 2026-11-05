# RESULTS -- ingest-restart SRT pull wedge drill (NMW-3126, parent NMW-3124)

Run on 2026-10-30 on the dev workstation (20 CPU / 61 GB), Docker 28.1, compose v2.35.1.
Images: ingest `nedmedia/mistserver:nedmedia-v2` (current prod, has the idle watchdog),
transcode and source `nedmedia/mistserver:nedmedia-v2-drill` (= nedmedia-v2 + `apk add ffmpeg curl`,
built locally from `Dockerfile.drill`; Mist binaries identical). Harness: `drill.sh` in this directory;
raw data for every trigger in `run/results/<trigger>/` (gitignored; samples.tsv, transcode.log,
ingest.log, transcode.ps, verdict.txt).

Topology: `source` (ffmpeg testsrc2 640x360 H.264 + AAC, RTMP push `rtmp://ingest/push/live+drill`,
`restart: always`) -> `ingest` stream `live+drill` (push://, DVR 15 s) with a TSSRT listener output on
8889 -> `transcode` stream `drill` with source `srt://ingest:8889?streamid=live+drill` (MistInTSSRT,
default `--idletimeout 10000`), one `MistProcMKVExec` rung (ffmpeg libx264 426x240, production field
shape: `source_mask 4`, `target_mask 1`, `restart_type fixed`, `exit_unmask false`). An HLS demand loop
(master -> variant -> newest segment, once a second) ran against `transcode` for the whole run.
Every trigger started from a freshly started stack verified healthy for 30 s.

Sampled every ~2.5 s (the 2 s cadence stretches to ~8 s while the HLS probe is timing out at 5 s)
for 180 s (240 s for `restart-x3`): `json_live+drill.js` on ingest (`in_live`, `in_tracks`),
`json_drill.js` on transcode (`tx_live`, `tx_tracks`, `tx_codecs`; `err` = the JSON output returned an
`error` field, text in `tx_codecs`), process counts inside the transcode container (`tssrt` =
MistInTSSRT incl. angel, `buffer` = MistInBuffer, `mkvexec`, `outhls` = MistOutHLS), HLS master index
HTTP code (`000` = curl timed out after 5 s), and cumulative counts in the transcode log of
`A pull process for this stream is already running`, `Requested invalid track`, `Dropping invalid track`.

**Wedge criterion** (incident 2026-10-30, ingest-5 / br1r5varhysm): puller alive, `json_<key>.js`
`meta.live=1` with 0 tracks for > 60 s, `already running` repeating, HLS hung, MistOutHLS piling up,
only `nuke_stream` recovers.

## Summary

**The wedge did not reproduce with any trigger.** Every trigger produced a transient outage of 65-120 s
after which the transcode stream recovered by itself, with no `live=1 / 0 tracks` samples at all and no
`Requested invalid track` lines.

| trigger | first unhealthy (s) | healthy again (s) | HLS outage (s) | peak MistInTSSRT | peak MistOutHLS | `already running` | `Dropping invalid track` | `Requested invalid track` | end state live/tracks/hls |
|---|---|---|---|---|---|---|---|---|---|
| `restart` | 10 | 78 | 68 | 3 | 14 | 0 | 2 | 0 | 1/3/200 |
| `kill5` | 7 | 75 | 68 | 3 | 14 | 0 | 2 | 0 | 1/3/200 |
| `kill20` | 8 | 91 | 83 | 10 | 19 | 7 | 2 | 0 | 1/3/200 |
| `kill60` | 8 | 128 | 120 | 20 | 30 | 17 | 2 | 0 | 1/3/200 |
| `pause15` | 2 | 83 | 81 | 6 | 16 | 3 | 2 | 0 | 1/3/200 |
| `pause40` | 3 | 101 | 98 | 11 | 23 | 9 | 2 | 0 | 1/3/200 |
| `restart-srcpaused` | 10 | 92 | 82 | 7 | 14 | 2 | 2 | 0 | 1/3/200 |
| `restart-x3` | 10 | 108 | 98 | 3 | 14 | 0 | 4 | 0 | 1/3/200 |

`already running` is counted from the trigger moment; the 2 lines every stack logs at boot (two
concurrent HLS requests racing to spawn the first puller) are not included.

## What every trigger did have in common (not the wedge, but worth knowing)

1. **The outage is 65-120 s, not the 5-15 s one would expect.** The sequence is the same every time:
   * t+5..10 s: the puller's SRT socket breaks (libsrt peer-idle, ~5 s), `MistProcMKVExec` logs
     `Dropping invalid track 2/1: disappeared from metadata` (the exact line from the incident), the
     buffer closes, `Stream drill became inactive`, `json_drill.js` -> `Stream is offline`.
   * while the ingest is down: every HLS request spawns a new `MistInTSSRT`, which logs `A pull process
     for this stream is already running` or `Can't connect SRT socket to any address for ingest` and
     lingers; MistInTSSRT count climbs (20 processes after 60 s down) together with MistOutHLS (30).
   * ingest back + source reconnected: the puller connects, `Stream drill became active`, MKVExec
     restarts (`Cannot assign address from size-less pointer` FAIL at spawn is normal for this build),
     `json_drill.js` shows the 3 tracks within ~10 s -- **but HLS keeps timing out.**
   * ~25 s later MKVExec exits with `Logging unclean exit reason: set inactive`, the stream goes
     inactive again for ~5-10 s, MKVExec is restarted a second time, and only after that second cycle
     does `/hls/drill/index.m3u8` answer 200 again. Total outage 65-120 s. In `kill60` the 360p source
     track was briefly visible to viewers (`tx_codecs=AAC,H264360,JSON` at t=68) before the mask applied.
   So every ingest restart costs the transcode box a minute-plus of HLS 5xx/timeouts and a pile of
   puller + MistOutHLS processes, which all drain once the stream is healthy. This is the transient
   version of what the incident showed permanently; in the incident the second cycle never completed.
2. The idle watchdog (`No data received over SRT for ... ms`) never fired: in every trigger libsrt
   itself reported the broken connection within ~5 s (peer idle timeout), even under `docker pause`
   (SIGSTOP of the whole ingest). The wedge therefore needs a situation in which the SRT socket stays
   "connected" without delivering data -- a restart where the ingest's TSSRT output keeps the session
   open (e.g. the ingest host comes back on the same address while the kernel still answers SRT
   keepalives), or a half-open path through the cloud network -- none of which a single-host docker
   network produces.
3. The transcode image is built with `-DDEBUG=3`, so `INFO` lines from MistInTSSRT / MistInBuffer
   (`Input booting`, `Caller SRT socket ... connected`, exit reasons) are compiled out; the logs only
   carry WARN/ERROR/FAIL and the controller's INFO. The per-stream `"debug": 4` in the configs is
   harmless but cannot add them. A `-DDEBUG=4` build of the same commit would be needed to see the
   puller's own state transitions.

## Differences from production that may matter for the next attempt

* One docker network, one host: the SRT peer-idle detection is instant and reliable. Production
  pullers cross a cloud network to an ingest on another host; a restart there may leave the UDP path
  half-open.
* `docker restart`/`kill` restarts only Mist; production "ingest-5 restarted" was most likely the whole
  box (compose with `restart: unless-stopped`), so Mist came up before the encoder reconnected, more
  like `restart-srcpaused` here (which also recovered).
* One MKVExec rung instead of three, no `SRT_ACCEPT`/`PUSH_REWRITE` triggers on the ingest, no
  CloudFront fan-out (the demand loop is a single client).
* Pull `DVR` 60 s here (prod transcoders use 60 s as well per live-stream-v2 compose; ingest 15 s).

## Suggested next triggers (not run here)

* Block the SRT path instead of stopping the process: `iptables -A INPUT -p udp --dport 8889 -j DROP`
  (or `tc netem loss 100%`) on the ingest side so the puller's socket stays connected but silent;
  then restart the ingest while the drop is in place and lift it afterwards.
* Restart the ingest while the transcode is in its second MKVExec cycle (`restart-x3` did this at 30 s
  spacing and still recovered; try 20 s and 45 s).
* Run with a `-DDEBUG=4` build of nedmedia-v2 so the puller logs its connect/exit reasons.

## Per-trigger timelines

### `restart` -- (a) `docker restart ingest`

Trigger log (host clock, UTC-3):

```
10:17:43 T: docker restart drill-ingest
10:17:45 T: restart done
```

Verdict: **no wedge** -- recovered on its own. First unhealthy sample t=10 s, first healthy again t=78 s (HLS outage ~68 s). Longest run of `live=1, tracks=0, puller alive`: 0 samples. Peak MistInTSSRT processes 3, peak MistOutHLS 14, `already running` lines in the window: 0.

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 10:17:43 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 3 | 10:17:45 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 10:17:48 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 8 | 10:17:50 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 10 | 10:17:58 | 1 | 3 | err | 0 | Stream_is_offline | 0 | 0 | 0 | 1 | 000 | 0 | 0 | 2 |
| 18 | 10:18:05 | 1 | 3 | 1 | 1 | JSON | 2 | 2 | 1 | 3 | 000 | 0 | 0 | 2 |
| 25 | 10:18:13 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 5 | 000 | 0 | 0 | 2 |
| 33 | 10:18:20 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 8 | 000 | 0 | 0 | 2 |
| 40 | 10:18:28 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 10 | 000 | 0 | 0 | 2 |
| 48 | 10:18:35 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 3 | 2 | 0 | 12 | 000 | 0 | 0 | 2 |
| 56 | 10:18:43 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 14 | 000 | 0 | 0 | 2 |
| 63 | 10:18:51 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 2 |
| 71 | 10:18:58 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 2 |
| 78 | 10:19:01 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 81 | 10:19:03 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 83 | 10:19:06 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 86 | 10:19:08 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 109 | 10:19:32 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 140 | 10:20:02 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 171 | 10:20:33 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 181 | 10:20:44 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
13:17:50 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
13:17:50 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
13:17:50 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:17:58 MistProcMKVExec:drill (430) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:17:58 MistController (8) INFO: Discovered ongoing push 430, adding to push list (../src/src/controller/controller_push.cpp:106)
13:17:58 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:18:23 MistProcMKVExec:drill (430) FAIL: Logging unclean exit reason: set inactive (../src/lib/config.cpp:65)
13:18:24 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:18:32 MistProcMKVExec:drill (546) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:18:32 MistController (8) INFO: Discovered ongoing push 546, adding to push list (../src/src/controller/controller_push.cpp:106)
13:18:32 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `kill5` -- (b) `docker kill ingest`, `compose up -d ingest` after 5 s

Trigger log (host clock, UTC-3):

```
10:22:31 T: docker kill drill-ingest
10:22:37 T: compose up -d ingest after 5s
10:22:37 T: ingest up
```

Verdict: **no wedge** -- recovered on its own. First unhealthy sample t=7 s, first healthy again t=75 s (HLS outage ~68 s). Longest run of `live=1, tracks=0, puller alive`: 0 samples. Peak MistInTSSRT processes 3, peak MistOutHLS 14, `already running` lines in the window: 0.

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 10:22:31 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 10:22:34 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 10:22:36 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 7 | 10:22:44 | err | 0 | err | 0 | Stream_is_offline | 0 | 0 | 0 | 1 | 000 | 0 | 0 | 2 |
| 15 | 10:22:51 | 1 | 3 | 1 | 1 | JSON | 2 | 2 | 1 | 3 | 000 | 0 | 0 | 2 |
| 22 | 10:22:59 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 5 | 000 | 0 | 0 | 2 |
| 30 | 10:23:06 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 7 | 000 | 0 | 0 | 2 |
| 37 | 10:23:14 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 10 | 000 | 0 | 0 | 2 |
| 45 | 10:23:21 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 3 | 2 | 0 | 12 | 000 | 0 | 0 | 2 |
| 52 | 10:23:29 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 14 | 000 | 0 | 0 | 2 |
| 60 | 10:23:36 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 13 | 000 | 0 | 0 | 2 |
| 67 | 10:23:44 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 2 |
| 75 | 10:23:47 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 200 | 0 | 0 | 2 |
| 78 | 10:23:50 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 81 | 10:23:52 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 83 | 10:23:55 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 107 | 10:24:18 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 138 | 10:24:49 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 168 | 10:25:20 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 181 | 10:25:33 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
13:22:36 MistProcMKVExec:drill (161) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
13:22:36 MistProcMKVExec:drill (161) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
13:22:37 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:22:44 MistProcMKVExec:drill (413) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:22:44 MistController (8) INFO: Discovered ongoing push 413, adding to push list (../src/src/controller/controller_push.cpp:106)
13:22:44 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:23:09 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:23:09 MistProcMKVExec:drill (413) FAIL: Logging unclean exit reason: set inactive (../src/lib/config.cpp:65)
13:23:19 MistProcMKVExec:drill (524) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:23:19 MistController (8) INFO: Discovered ongoing push 524, adding to push list (../src/src/controller/controller_push.cpp:106)
13:23:19 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `kill20` -- (b) `docker kill ingest`, `compose up -d ingest` after 20 s

Trigger log (host clock, UTC-3):

```
10:27:23 T: docker kill drill-ingest
10:27:44 T: compose up -d ingest after 20s
10:27:44 T: ingest up
```

Verdict: **no wedge** -- recovered on its own. First unhealthy sample t=8 s, first healthy again t=91 s (HLS outage ~83 s). Longest run of `live=1, tracks=0, puller alive`: 0 samples. Peak MistInTSSRT processes 10, peak MistOutHLS 19, `already running` lines in the window: 7.

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 10:27:23 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 10:27:26 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 10:27:29 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 8 | 10:27:36 | - | - | err | 0 | Stream_is_offline | 2 | 0 | 0 | 1 | 000 | 1 | 0 | 2 |
| 15 | 10:27:44 | - | - | err | 0 | Stream_is_offline | 4 | 0 | 0 | 3 | 000 | 4 | 0 | 2 |
| 23 | 10:27:51 | - | - | err | 0 | Stream_is_offline | 6 | 0 | 0 | 5 | 000 | 6 | 0 | 2 |
| 30 | 10:27:59 | 1 | 3 | 1 | 2 | AAC,JSON | 8 | 2 | 1 | 7 | 000 | 6 | 0 | 2 |
| 38 | 10:28:06 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 10 | 000 | 6 | 0 | 2 |
| 45 | 10:28:14 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 12 | 000 | 6 | 0 | 2 |
| 53 | 10:28:22 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 14 | 000 | 6 | 0 | 2 |
| 61 | 10:28:29 | 1 | 3 | err | 0 | Stream_is_offline | 7 | 0 | 0 | 16 | 000 | 7 | 0 | 2 |
| 68 | 10:28:37 | 1 | 3 | 1 | 1 | JSON | 10 | 2 | 1 | 19 | 000 | 7 | 0 | 2 |
| 76 | 10:28:44 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 14 | 000 | 7 | 0 | 2 |
| 83 | 10:28:52 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 13 | 000 | 7 | 0 | 2 |
| 91 | 10:28:57 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 13 | 200 | 7 | 0 | 2 |
| 96 | 10:29:00 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 7 | 0 | 2 |
| 99 | 10:29:02 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 7 | 0 | 2 |
| 101 | 10:29:05 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 7 | 0 | 2 |
| 124 | 10:29:28 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 7 | 0 | 2 |
| 155 | 10:29:58 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 7 | 0 | 2 |
| 181 | 10:30:24 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 7 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
13:27:29 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
13:27:29 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
13:27:30 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:27:31 MistInTSSRT:drill (410) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 2 more 'A pull process for this stream is already running' lines ...
13:27:41 MistInTSSRT:drill (400) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:27:43 MistInTSSRT:drill (430) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 2 more 'A pull process for this stream is already running' lines ...
13:27:53 MistProcMKVExec:drill (456) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:27:53 MistController (8) INFO: Discovered ongoing push 456, adding to push list (../src/src/controller/controller_push.cpp:106)
13:27:54 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:28:20 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:28:25 MistInTSSRT:drill (563) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
13:28:29 MistProcMKVExec:drill (569) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:28:29 MistController (8) INFO: Discovered ongoing push 569, adding to push list (../src/src/controller/controller_push.cpp:106)
13:28:29 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `kill60` -- (b) `docker kill ingest`, `compose up -d ingest` after 60 s

Trigger log (host clock, UTC-3):

```
10:32:17 T: docker kill drill-ingest
10:33:17 T: compose up -d ingest after 60s
10:33:17 T: ingest up
```

Verdict: **no wedge** -- recovered on its own. First unhealthy sample t=8 s, first healthy again t=128 s (HLS outage ~120 s). Longest run of `live=1, tracks=0, puller alive`: 0 samples. Peak MistInTSSRT processes 20, peak MistOutHLS 30, `already running` lines in the window: 17.

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 10:32:17 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 3 | 10:32:19 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 10:32:22 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 8 | 10:32:29 | - | - | err | 0 | Stream_is_offline | 0 | 0 | 0 | 1 | 000 | 1 | 0 | 2 |
| 15 | 10:32:37 | - | - | err | 0 | Stream_is_offline | 3 | 0 | 0 | 3 | 000 | 4 | 0 | 2 |
| 23 | 10:32:44 | - | - | err | 0 | Stream_is_offline | 6 | 0 | 0 | 5 | 000 | 6 | 0 | 2 |
| 30 | 10:32:52 | - | - | err | 0 | Stream_is_offline | 8 | 0 | 0 | 7 | 000 | 8 | 0 | 2 |
| 38 | 10:33:00 | - | - | err | 0 | Stream_is_offline | 11 | 0 | 0 | 10 | 000 | 11 | 0 | 2 |
| 46 | 10:33:07 | - | - | err | 0 | Stream_is_offline | 13 | 0 | 0 | 12 | 000 | 13 | 0 | 2 |
| 53 | 10:33:15 | - | - | err | 0 | Stream_is_offline | 15 | 0 | 0 | 14 | 000 | 15 | 0 | 2 |
| 61 | 10:33:22 | - | - | err | 0 | Stream_is_offline | 17 | 0 | 0 | 16 | 000 | 17 | 0 | 2 |
| 68 | 10:33:30 | 1 | 3 | 1 | 3 | AAC,H264360,JSON | 19 | 2 | 1 | 19 | 000 | 17 | 0 | 2 |
| 76 | 10:33:37 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 19 | 2 | 1 | 21 | 000 | 17 | 0 | 2 |
| 83 | 10:33:45 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 19 | 2 | 1 | 23 | 000 | 17 | 0 | 2 |
| 91 | 10:33:52 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 19 | 2 | 1 | 25 | 000 | 17 | 0 | 2 |
| 98 | 10:34:00 | 1 | 3 | err | 0 | Stream_is_offline | 20 | 2 | 0 | 28 | 000 | 17 | 0 | 2 |
| 106 | 10:34:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 20 | 2 | 1 | 30 | 000 | 17 | 0 | 2 |
| 113 | 10:34:15 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 17 | 0 | 2 |
| 121 | 10:34:22 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 13 | 000 | 17 | 0 | 2 |
| 128 | 10:34:27 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 200 | 17 | 0 | 2 |
| 133 | 10:34:30 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 17 | 0 | 2 |
| 136 | 10:34:32 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 17 | 0 | 2 |
| 138 | 10:34:35 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 17 | 0 | 2 |
| 161 | 10:34:57 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 17 | 0 | 2 |
| 181 | 10:35:18 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 17 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
13:32:22 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
13:32:22 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
13:32:23 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:32:29 MistInTSSRT:drill (416) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 2 more 'A pull process for this stream is already running' lines ...
13:32:34 MistInTSSRT:drill (413) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:32:35 MistInTSSRT:drill (434) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 2 more 'A pull process for this stream is already running' lines ...
13:32:45 MistInTSSRT:drill (413) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:32:47 MistInTSSRT:drill (454) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 3 more 'A pull process for this stream is already running' lines ...
13:32:56 MistInTSSRT:drill (413) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:32:59 MistInTSSRT:drill (469) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 2 more 'A pull process for this stream is already running' lines ...
13:33:07 MistInTSSRT:drill (413) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:33:10 MistInTSSRT:drill (495) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 3 more 'A pull process for this stream is already running' lines ...
13:33:18 MistInTSSRT:drill (413) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:33:24 MistProcMKVExec:drill (533) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:33:24 MistController (8) INFO: Discovered ongoing push 533, adding to push list (../src/src/controller/controller_push.cpp:106)
13:33:25 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:33:50 MistProcMKVExec:drill (533) FAIL: Logging unclean exit reason: set inactive (../src/lib/config.cpp:65)
13:33:51 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:33:58 MistProcMKVExec:drill (643) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:33:58 MistController (8) INFO: Discovered ongoing push 643, adding to push list (../src/src/controller/controller_push.cpp:106)
13:33:59 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `pause15` -- (c) `docker pause ingest` 15 s, unpause

Trigger log (host clock, UTC-3):

```
10:37:08 T: docker pause drill-ingest
10:37:23 T: unpaused after 15s
```

Verdict: **no wedge** -- recovered on its own. First unhealthy sample t=2 s, first healthy again t=83 s (HLS outage ~81 s). Longest run of `live=1, tracks=0, puller alive`: 0 samples. Peak MistInTSSRT processes 6, peak MistOutHLS 16, `already running` lines in the window: 3.

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 10:37:08 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 10:37:20 | - | - | err | 0 | Stream_is_offline | 2 | 2 | 1 | 0 | 000 | 0 | 0 | 2 |
| 14 | 10:37:29 | 1 | 3 | err | 0 | Stream_is_offline | 2 | 0 | 0 | 3 | 000 | 3 | 0 | 2 |
| 23 | 10:37:36 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 5 | 000 | 3 | 0 | 2 |
| 30 | 10:37:44 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 7 | 000 | 3 | 0 | 2 |
| 38 | 10:37:51 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 10 | 000 | 3 | 0 | 2 |
| 45 | 10:37:59 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 12 | 000 | 3 | 0 | 2 |
| 53 | 10:38:06 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 6 | 2 | 1 | 14 | 000 | 3 | 0 | 2 |
| 60 | 10:38:14 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 6 | 2 | 1 | 16 | 000 | 3 | 0 | 2 |
| 68 | 10:38:21 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 3 | 0 | 2 |
| 75 | 10:38:29 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 3 | 0 | 2 |
| 83 | 10:38:32 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 3 | 0 | 2 |
| 86 | 10:38:34 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 3 | 0 | 2 |
| 88 | 10:38:37 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 3 | 0 | 2 |
| 91 | 10:38:39 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 3 | 0 | 2 |
| 113 | 10:39:02 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 3 | 0 | 2 |
| 144 | 10:39:33 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 3 | 0 | 2 |
| 175 | 10:40:03 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 3 | 0 | 2 |
| 180 | 10:40:09 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 3 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
13:37:14 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
13:37:14 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
13:37:15 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:37:24 MistInTSSRT:drill (406) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 2 more 'A pull process for this stream is already running' lines ...
13:37:29 MistProcMKVExec:drill (419) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:37:29 MistController (8) INFO: Discovered ongoing push 419, adding to push list (../src/src/controller/controller_push.cpp:106)
13:37:29 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:37:54 MistProcMKVExec:drill (419) FAIL: Logging unclean exit reason: set inactive (../src/lib/config.cpp:65)
13:37:55 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:38:02 MistProcMKVExec:drill (522) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:38:02 MistController (8) INFO: Discovered ongoing push 522, adding to push list (../src/src/controller/controller_push.cpp:106)
13:38:02 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `pause40` -- (c) `docker pause ingest` 40 s, unpause

Trigger log (host clock, UTC-3):

```
10:42:01 T: docker pause drill-ingest
10:42:41 T: unpaused after 40s
```

Verdict: **no wedge** -- recovered on its own. First unhealthy sample t=3 s, first healthy again t=101 s (HLS outage ~98 s). Longest run of `live=1, tracks=0, puller alive`: 0 samples. Peak MistInTSSRT processes 11, peak MistOutHLS 23, `already running` lines in the window: 9.

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 10:42:01 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 3 | 10:42:12 | - | - | err | 0 | Stream_is_offline | 2 | 2 | 1 | 0 | 000 | 0 | 0 | 2 |
| 14 | 10:42:24 | - | - | err | 0 | Stream_is_offline | 4 | 0 | 0 | 3 | 000 | 4 | 0 | 2 |
| 26 | 10:42:35 | - | - | err | 0 | Stream_is_offline | 7 | 0 | 0 | 6 | 000 | 7 | 0 | 2 |
| 37 | 10:42:46 | err | 0 | err | 0 | Stream_is_offline | 10 | 0 | 0 | 9 | 000 | 9 | 0 | 2 |
| 48 | 10:42:54 | 1 | 3 | 1 | 1 | JSON | 11 | 2 | 1 | 11 | 000 | 9 | 0 | 2 |
| 56 | 10:43:01 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 11 | 2 | 1 | 14 | 000 | 9 | 0 | 2 |
| 63 | 10:43:09 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 11 | 2 | 1 | 16 | 000 | 9 | 0 | 2 |
| 71 | 10:43:16 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 11 | 2 | 1 | 18 | 000 | 9 | 0 | 2 |
| 78 | 10:43:24 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 11 | 2 | 0 | 20 | 000 | 9 | 0 | 2 |
| 86 | 10:43:31 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 11 | 2 | 1 | 23 | 000 | 9 | 0 | 2 |
| 93 | 10:43:39 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 9 | 0 | 2 |
| 101 | 10:43:46 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 13 | 200 | 9 | 0 | 2 |
| 108 | 10:43:49 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 111 | 10:43:51 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 113 | 10:43:54 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 136 | 10:44:17 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 167 | 10:44:47 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 179 | 10:45:00 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
13:42:06 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
13:42:06 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
13:42:07 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:42:17 MistInTSSRT:drill (407) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 2 more 'A pull process for this stream is already running' lines ...
13:42:20 MistInTSSRT:drill (395) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:42:20 MistInTSSRT:drill (419) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 1 more 'A pull process for this stream is already running' lines ...
13:42:27 MistInTSSRT:drill (395) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:42:30 MistInTSSRT:drill (436) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 1 more 'A pull process for this stream is already running' lines ...
13:42:34 MistInTSSRT:drill (395) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:42:38 MistInTSSRT:drill (446) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
13:42:41 MistInTSSRT:drill (395) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
13:42:41 MistInTSSRT:drill (454) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
13:42:46 MistProcMKVExec:drill (477) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:42:46 MistController (8) INFO: Discovered ongoing push 477, adding to push list (../src/src/controller/controller_push.cpp:106)
13:42:47 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:43:13 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:43:14 MistInTSSRT:drill (394) WARN: Input for stream drill uncleanly shut down! Cleaning and restarting... (../src/src/input/input.cpp:664)
13:43:19 MistProcMKVExec:drill (587) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:43:19 MistController (8) INFO: Discovered ongoing push 587, adding to push list (../src/src/controller/controller_push.cpp:106)
13:43:20 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `restart-srcpaused` -- (d) pause ffmpeg source, `docker restart ingest`, unpause source 20 s later

Trigger log (host clock, UTC-3):

```
10:46:47 T: docker pause drill-source
10:46:49 T: docker restart drill-ingest
10:46:52 T: restart done, source still paused
10:47:12 T: source unpaused after 20s
```

Verdict: **no wedge** -- recovered on its own. First unhealthy sample t=10 s, first healthy again t=92 s (HLS outage ~82 s). Longest run of `live=1, tracks=0, puller alive`: 0 samples. Peak MistInTSSRT processes 7, peak MistOutHLS 14, `already running` lines in the window: 2.

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 10:46:47 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 10:46:50 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 10:46:52 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 7 | 10:46:55 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 10 | 10:47:02 | err | 0 | err | 0 | Stream_is_offline | 3 | 0 | 0 | 1 | 000 | 1 | 0 | 2 |
| 17 | 10:47:10 | err | 0 | err | 0 | Stream_is_waiting_for_data | 4 | 2 | 0 | 3 | 000 | 1 | 0 | 2 |
| 25 | 10:47:17 | err | 0 | err | 0 | Stream_is_waiting_for_data | 5 | 2 | 0 | 5 | 000 | 2 | 0 | 2 |
| 32 | 10:47:25 | 1 | 3 | 1 | 1 | JSON | 7 | 2 | 1 | 7 | 000 | 2 | 0 | 2 |
| 40 | 10:47:32 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 7 | 2 | 1 | 10 | 000 | 2 | 0 | 2 |
| 47 | 10:47:40 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 7 | 2 | 1 | 12 | 000 | 2 | 0 | 2 |
| 55 | 10:47:47 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 7 | 2 | 1 | 14 | 000 | 2 | 0 | 2 |
| 62 | 10:47:55 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 6 | 2 | 0 | 14 | 000 | 2 | 0 | 2 |
| 70 | 10:48:02 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 4 | 2 | 1 | 14 | 000 | 2 | 0 | 2 |
| 77 | 10:48:10 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 2 | 0 | 2 |
| 85 | 10:48:17 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 2 | 0 | 2 |
| 92 | 10:48:20 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 95 | 10:48:22 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 97 | 10:48:25 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 100 | 10:48:27 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 123 | 10:48:50 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 153 | 10:49:21 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 181 | 10:49:49 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
13:46:56 MistProcMKVExec:drill (161) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
13:46:56 MistProcMKVExec:drill (161) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
13:46:56 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:46:57 MistInTSSRT:drill (424) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
        ... 1 more 'A pull process for this stream is already running' lines ...
13:47:18 MistProcMKVExec:drill (481) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:47:18 MistController (8) INFO: Discovered ongoing push 481, adding to push list (../src/src/controller/controller_push.cpp:106)
13:47:18 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:47:43 MistProcMKVExec:drill (481) FAIL: Logging unclean exit reason: set inactive (../src/lib/config.cpp:65)
13:47:44 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:47:50 MistProcMKVExec:drill (591) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:47:50 MistController (8) INFO: Discovered ongoing push 591, adding to push list (../src/src/controller/controller_push.cpp:106)
13:47:50 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `restart-x3` -- (extra) three `docker restart ingest` 30 s apart (2nd/3rd land inside the previous recovery)

Trigger log (host clock, UTC-3):

```
10:51:59 T: docker restart drill-ingest (1/3)
10:52:32 T: docker restart drill-ingest (2/3)
10:53:04 T: docker restart drill-ingest (3/3)
10:53:07 T: restarts done
```

Verdict: **no wedge** -- recovered on its own. First unhealthy sample t=10 s, first healthy again t=108 s (HLS outage ~98 s). Longest run of `live=1, tracks=0, puller alive`: 0 samples. Peak MistInTSSRT processes 3, peak MistOutHLS 14, `already running` lines in the window: 0.

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 10:51:59 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 10:52:02 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 10:52:04 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 7 | 10:52:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 2 |
| 10 | 10:52:15 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 2 | 2 | 0 | 1 | 000 | 0 | 0 | 2 |
| 18 | 10:52:22 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 3 | 000 | 0 | 0 | 2 |
| 25 | 10:52:30 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 5 | 000 | 0 | 0 | 2 |
| 33 | 10:52:37 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 8 | 000 | 0 | 0 | 2 |
| 40 | 10:52:45 | 1 | 3 | err | 0 | Stream_is_offline | 2 | 2 | 1 | 10 | 000 | 0 | 0 | 2 |
| 48 | 10:52:52 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 12 | 000 | 0 | 0 | 2 |
| 55 | 10:53:00 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 14 | 000 | 0 | 0 | 2 |
| 63 | 10:53:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 2 |
| 70 | 10:53:15 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 4 |
| 78 | 10:53:22 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 3 | 2 | 0 | 14 | 000 | 0 | 0 | 4 |
| 85 | 10:53:30 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 13 | 000 | 0 | 0 | 4 |
| 93 | 10:53:37 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 13 | 000 | 0 | 0 | 4 |
| 100 | 10:53:45 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 4 |
| 108 | 10:53:47 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 2 | 200 | 0 | 0 | 4 |
| 110 | 10:53:50 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 113 | 10:53:53 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 116 | 10:53:55 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 139 | 10:54:19 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 170 | 10:54:50 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 201 | 10:55:20 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 231 | 10:55:51 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 241 | 10:56:01 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
13:52:07 MistInTSSRT:drill (151) INFO: Logging clean exit reason: SRT connection close (../src/lib/config.cpp:63)
13:52:07 MistInTSSRT:drill (151) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
13:52:07 MistInBuffer:drill (156) INFO: Disconnected track 1 (../src/src/input/input_buffer.cpp:536)
13:52:07 MistInBuffer:drill (156) INFO: Should remove track 1 (../src/src/input/input_buffer.cpp:340)
13:52:07 MistInBuffer:drill (156) INFO: Disconnected track 2 (../src/src/input/input_buffer.cpp:536)
13:52:07 MistInBuffer:drill (156) INFO: Should remove track 2 (../src/src/input/input_buffer.cpp:340)
13:52:07 MistProcMKVExec:drill (161) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
13:52:07 MistProcMKVExec:drill (161) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
13:52:07 MistProcMKVExec:drill (161) INFO: Buffer completely played out (../src/src/output/output.cpp:2391)
13:52:07 MistProcMKVExec:drill (161) INFO: Logging clean exit reason: end of stream (../src/lib/config.cpp:63)
13:52:07 MistProcMKVExec:drill (161) INFO: Client handler shutting down, exit reason: end of stream (../src/src/output/output.cpp:2156)
13:52:07 MistProcMKVExec (161) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:07 MistProcMKVExec (161) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:07 MistInBuffer:drill (156) INFO: Logging clean exit reason: source disconnected for non-resumable stream (../src/lib/config.cpp:63)
13:52:07 MistInBuffer:drill (156) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
13:52:07 MistProcMKVExec (161) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:07 MistInTSSRT:drill (151) INFO: Waiting up to 10 seconds for 1 processes... (../src/lib/procs.cpp:123)
13:52:07 MistInTSSRT:drill (151) INFO: SIGINT 155 (../src/lib/procs.cpp:134)
13:52:07 MistInBuffer:drill (155) INFO: Logging clean exit reason: signal Interrupt (2) from process 151 (../src/lib/config.cpp:63)
13:52:07 MistInBuffer:drill (155) INFO: Received signal Interrupt (2) from process 151 (../src/lib/config.cpp:683)
13:52:07 MistInBuffer:drill (155) INFO: Shutting down input for stream drill because of signal interrupt... (../src/src/input/input.cpp:602)
13:52:07 MistInBuffer:drill (156) INFO: Received signal Terminated (15) from process 155 (../src/lib/config.cpp:683)
13:52:07 MistProcMKVExec (161) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:07 MistProcMKVExec (161) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:07 MistProcMKVExec:drill→drill (161) INFO: Logging clean exit reason: no more data (../src/lib/config.cpp:63)
13:52:07 MistProcMKVExec:drill→drill (161) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
13:52:07 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:52:08 MistProcMKVExec (161) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:08 MistProcMKVExec (161) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:08 MistProcMKVExec (161) INFO: Closing process clean (../src/src/process/process_exec.cpp:239)
13:52:08 MistInTSSRT:drill (410) INFO: Initialized libsrt version 1.5.6 (../src/lib/socket_srt.cpp:46)
13:52:08 MistInTSSRT:drill (410) INFO: Parsed url: srt://ingest:8889?streamid=live+drill (../src/src/input/input_tssrt.cpp:210)
13:52:09 MistInTSSRT:drill (410) INFO: Caller SRT socket 685512310 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
13:52:09 MistInTSSRT:drill (410) INFO: Starting stream (../src/src/input/input.cpp:741)
13:52:09 MistInBuffer:drill (415) INFO: Setting bufferTime from 50000 to new value of 60000 (../src/src/input/input_buffer.cpp:604)
13:52:09 MistInBuffer:drill (415) INFO: Starting serve (../src/src/input/input.cpp:745)
13:52:09 MistInBuffer:drill (415) INFO: Input started (../src/src/input/input.cpp:898)
13:52:09 MistInBuffer:drill (415) INFO: Setting segmentSize from 1900 to new value of 6000 (../src/src/input/input_buffer.cpp:647)
13:52:09 MistInBuffer:drill (415) INFO: Setting maxKeepAway from 0 to new value of 45000 (../src/src/input/input_buffer.cpp:655)
13:52:09 MistInTSSRT:drill (410) INFO: Input started (../src/src/input/input.cpp:1029)
13:52:12 MistInTSSRT:drill (410) INFO: Creating new (delayed) track 0: JSON meta (../src/lib/dtsc.cpp:2110)
13:52:12 MistInTSSRT:drill (410) INFO: Creating new (delayed) track 1: AAC audio (../src/lib/dtsc.cpp:2110)
13:52:13 MistInBuffer:drill (415) INFO: Started process 426: /usr/local/bin/MistProcMKVExec {"codec":"H264","exec":"ffmpeg -loglevel warning -hide_banner -fflags nobuffer -i - -vf scale=426:240 -c:v libx264 -preset veryfast -tune zerolatency -profile:v main -pix_fmt yuv420p -b:v 300k -maxrate 330k -bufsize 600k -force_key_frames source -bf 0 -threads 2 -c:a aac -b:a 64k -ac 2 -ar 48000 -f matroska -cluster_time_limit 0 -","exit_unmask":0,"inconsequential":0,"process":"MKVExec","resolution":"426x240","restart_type":"fixed","source":"drill","source_mask":4,"sources":null,"tags_inhibit":null,"target_mask":1,"target_profiles":[{"bitrate":300,"height":240,"name":"240p","width":426}],"track_select":"audio=all&video=all","x-LSP-kind":"video"} (../src/src/input/input_buffer.cpp:843)
13:52:13 MistProcMKVExec (426) INFO: No sink explicitly set, using source as sink (../src/src/process/process_exec.cpp:180)
13:52:13 MistProcMKVExec (426) INFO: Executing command: ffmpeg -loglevel warning -hide_banner -fflags nobuffer -i - -vf scale=426:240 -c:v libx264 -preset veryfast -tune zerolatency -profile:v main -pix_fmt yuv420p -b:v 300k -maxrate 330k -bufsize 600k -force_key_frames source -bf 0 -threads 2 -c:a aac -b:a 64k -ac 2 -ar 48000 -f matroska -cluster_time_limit 0 - (../src/src/process/process_exec.cpp:203)
13:52:13 MistProcMKVExec:drill→drill (426) INFO: Starting stream (../src/src/input/input.cpp:741)
13:52:13 MistProcMKVExec:drill→drill (426) INFO: Input started (../src/src/input/input.cpp:1029)
13:52:13 MistProcMKVExec:drill (426) INFO: Stripping target options: audio=all&video=all (../src/src/output/output.cpp:156)
13:52:13 MistProcMKVExec:drill (426) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:52:13 MistProcMKVExec:drill (426) INFO: Booting new session Oa1a43e0d5bdda150d65879ecd7ca1cd02d9fa22b941d1a96b3f2c38e715f7234 (../src/lib/comms.cpp:392)
13:52:13 MistController (8) INFO: Discovered ongoing push 426, adding to push list (../src/src/controller/controller_push.cpp:106)
13:52:13 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:52:14 MistInTSSRT:drill (410) INFO: Creating new (delayed) track 2: H264 video (../src/lib/dtsc.cpp:2110)
13:52:14 MistInTSSRT:drill (410) INFO: Track 2 page 0 not starting with a keyframe! (../src/src/io.cpp:503)
13:52:15 MistProcMKVExec:drill (426) INFO: Track 1 seekable (../src/src/output/output.cpp:943)
13:52:15 MistProcMKVExec:drill (426) INFO: Track 2 seekable (../src/src/output/output.cpp:943)
13:52:15 MistProcMKVExec:drill (426) INFO: Recording will start at timestamp 0d00h00m08s.021 (../src/src/output/output.cpp:1299)
13:52:15 MistProcMKVExec:drill (426) INFO: Track 1 seekable (../src/src/output/output.cpp:943)
13:52:15 MistProcMKVExec:drill (426) INFO: Track 2 seekable (../src/src/output/output.cpp:943)
13:52:15 MistProcMKVExec:drill (426) INFO: Outputting drill to stdout with MKVExec format (../src/src/output/output.cpp:1851)
13:52:15 MistProcMKVExec:drill (426) INFO: Masking source track 1 with 4 (../src/src/process/process_exec.cpp:125)
13:52:15 MistProcMKVExec:drill (426) INFO: Masking source track 2 with 4 (../src/src/process/process_exec.cpp:125)
13:52:16 MistProcMKVExec:drill→drill (426) INFO: Detected track 1 => 3: video_H264_426x240_0fps (../src/lib/ebml_socketglue.cpp:443)
13:52:16 MistProcMKVExec:drill→drill (426) INFO: Resizing metadata track list from 4 to 8 (../src/lib/dtsc.cpp:1350)
13:52:16 MistProcMKVExec:drill→drill (426) INFO: Detected track 2 => 4: audio_AAC_2ch_48000hz (../src/lib/ebml_socketglue.cpp:443)
13:52:16 MistInTSSRT:drill (410) INFO: Reloading entire metadata (../src/lib/dtsc.cpp:1513)
13:52:16 MistProcMKVExec:drill (426) INFO: Reloading entire metadata (../src/lib/dtsc.cpp:1513)
13:52:16 MistInBuffer:drill (415) INFO: Reloading entire metadata (../src/lib/dtsc.cpp:1513)
13:52:38 MistInBuffer:drill (415) INFO: Logging clean exit reason: no activity for 30s (> 30s) (../src/lib/config.cpp:63)
13:52:38 MistInBuffer:drill (415) INFO: Input closing cleanly with reason: no activity for 30s (> 30s) (../src/src/input/input.cpp:825)
13:52:38 MistProcMKVExec:drill (426) INFO: Logging clean exit reason: Live stream source shut down (../src/lib/config.cpp:63)
13:52:38 MistProcMKVExec:drill (426) INFO: Client handler shutting down, exit reason: Live stream source shut down (../src/src/output/output.cpp:2156)
13:52:38 MistProcMKVExec (426) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:38 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:52:39 MistProcMKVExec (426) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:39 MistProcMKVExec (426) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:39 MistProcMKVExec (426) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:39 MistProcMKVExec (426) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:39 MistProcMKVExec:drill→drill (426) INFO: Logging clean exit reason: no more data (../src/lib/config.cpp:63)
13:52:39 MistProcMKVExec:drill→drill (426) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
13:52:39 MistInTSSRT:drill (410) INFO: Logging clean exit reason: SRT connection close (../src/lib/config.cpp:63)
13:52:39 MistInTSSRT:drill (410) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
13:52:39 MistProcMKVExec (426) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:39 MistProcMKVExec (426) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:52:40 MistProcMKVExec (426) INFO: Closing process clean (../src/src/process/process_exec.cpp:239)
13:52:40 MistInTSSRT:drill (519) INFO: Initialized libsrt version 1.5.6 (../src/lib/socket_srt.cpp:46)
13:52:40 MistInTSSRT:drill (519) INFO: Parsed url: srt://ingest:8889?streamid=live+drill (../src/src/input/input_tssrt.cpp:210)
13:52:41 MistInTSSRT:drill (519) INFO: Caller SRT socket 546716768 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
13:52:41 MistInTSSRT:drill (519) INFO: Starting stream (../src/src/input/input.cpp:741)
13:52:41 MistInBuffer:drill (524) INFO: Setting bufferTime from 50000 to new value of 60000 (../src/src/input/input_buffer.cpp:604)
13:52:41 MistInBuffer:drill (524) INFO: Starting serve (../src/src/input/input.cpp:745)
13:52:41 MistInBuffer:drill (524) INFO: Input started (../src/src/input/input.cpp:898)
13:52:41 MistInBuffer:drill (524) INFO: Setting segmentSize from 1900 to new value of 6000 (../src/src/input/input_buffer.cpp:647)
13:52:41 MistInBuffer:drill (524) INFO: Setting maxKeepAway from 0 to new value of 45000 (../src/src/input/input_buffer.cpp:655)
13:52:41 MistInTSSRT:drill (519) INFO: Input started (../src/src/input/input.cpp:1029)
13:52:44 MistInTSSRT:drill (519) INFO: Creating new (delayed) track 0: JSON meta (../src/lib/dtsc.cpp:2110)
13:52:44 MistInTSSRT:drill (519) INFO: Creating new (delayed) track 1: AAC audio (../src/lib/dtsc.cpp:2110)
13:52:44 MistInBuffer:drill (524) INFO: Started process 528: /usr/local/bin/MistProcMKVExec {"codec":"H264","exec":"ffmpeg -loglevel warning -hide_banner -fflags nobuffer -i - -vf scale=426:240 -c:v libx264 -preset veryfast -tune zerolatency -profile:v main -pix_fmt yuv420p -b:v 300k -maxrate 330k -bufsize 600k -force_key_frames source -bf 0 -threads 2 -c:a aac -b:a 64k -ac 2 -ar 48000 -f matroska -cluster_time_limit 0 -","exit_unmask":0,"inconsequential":0,"process":"MKVExec","resolution":"426x240","restart_type":"fixed","source":"drill","source_mask":4,"sources":null,"tags_inhibit":null,"target_mask":1,"target_profiles":[{"bitrate":300,"height":240,"name":"240p","width":426}],"track_select":"audio=all&video=all","x-LSP-kind":"video"} (../src/src/input/input_buffer.cpp:843)
13:52:44 MistProcMKVExec (528) INFO: No sink explicitly set, using source as sink (../src/src/process/process_exec.cpp:180)
13:52:44 MistProcMKVExec (528) INFO: Executing command: ffmpeg -loglevel warning -hide_banner -fflags nobuffer -i - -vf scale=426:240 -c:v libx264 -preset veryfast -tune zerolatency -profile:v main -pix_fmt yuv420p -b:v 300k -maxrate 330k -bufsize 600k -force_key_frames source -bf 0 -threads 2 -c:a aac -b:a 64k -ac 2 -ar 48000 -f matroska -cluster_time_limit 0 - (../src/src/process/process_exec.cpp:203)
13:52:44 MistProcMKVExec:drill→drill (528) INFO: Starting stream (../src/src/input/input.cpp:741)
13:52:44 MistProcMKVExec:drill→drill (528) INFO: Input started (../src/src/input/input.cpp:1029)
13:52:44 MistProcMKVExec:drill (528) INFO: Stripping target options: audio=all&video=all (../src/src/output/output.cpp:156)
13:52:44 MistProcMKVExec:drill (528) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:52:44 MistProcMKVExec:drill (528) INFO: Booting new session Oca46ec1d0775f745319bf6e9d4298dc2970f8fd1f459175265e40d708d2e85ae (../src/lib/comms.cpp:392)
13:52:44 MistController (8) INFO: Discovered ongoing push 528, adding to push list (../src/src/controller/controller_push.cpp:106)
13:52:44 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:52:45 MistInTSSRT:drill (519) INFO: Creating new (delayed) track 2: H264 video (../src/lib/dtsc.cpp:2110)
13:52:45 MistInTSSRT:drill (519) INFO: Track 2 page 0 not starting with a keyframe! (../src/src/io.cpp:503)
13:52:45 MistProcMKVExec:drill (528) INFO: Track 1 seekable (../src/src/output/output.cpp:943)
13:52:45 MistProcMKVExec:drill (528) INFO: Track 2 seekable (../src/src/output/output.cpp:943)
13:52:45 MistProcMKVExec:drill (528) INFO: Recording will start at timestamp 0d00h00m06s.021 (../src/src/output/output.cpp:1299)
13:52:45 MistProcMKVExec:drill (528) INFO: Track 1 seekable (../src/src/output/output.cpp:943)
13:52:45 MistProcMKVExec:drill (528) INFO: Track 2 seekable (../src/src/output/output.cpp:943)
13:52:45 MistProcMKVExec:drill (528) INFO: Outputting drill to stdout with MKVExec format (../src/src/output/output.cpp:1851)
13:52:45 MistProcMKVExec:drill (528) INFO: Masking source track 1 with 4 (../src/src/process/process_exec.cpp:125)
13:52:45 MistProcMKVExec:drill (528) INFO: Masking source track 2 with 4 (../src/src/process/process_exec.cpp:125)
13:52:47 MistProcMKVExec:drill→drill (528) INFO: Detected track 1 => 3: video_H264_426x240_0fps (../src/lib/ebml_socketglue.cpp:443)
13:52:47 MistProcMKVExec:drill→drill (528) INFO: Resizing metadata track list from 4 to 8 (../src/lib/dtsc.cpp:1350)
13:52:47 MistProcMKVExec:drill→drill (528) INFO: Detected track 2 => 4: audio_AAC_2ch_48000hz (../src/lib/ebml_socketglue.cpp:443)
13:52:47 MistInTSSRT:drill (519) INFO: Reloading entire metadata (../src/lib/dtsc.cpp:1513)
13:52:47 MistProcMKVExec:drill (528) INFO: Reloading entire metadata (../src/lib/dtsc.cpp:1513)
13:52:47 MistInBuffer:drill (524) INFO: Reloading entire metadata (../src/lib/dtsc.cpp:1513)
13:53:12 MistInTSSRT:drill (519) INFO: Logging clean exit reason: SRT connection close (../src/lib/config.cpp:63)
13:53:12 MistInTSSRT:drill (519) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
13:53:12 MistInBuffer:drill (524) INFO: Disconnected track 1 (../src/src/input/input_buffer.cpp:536)
13:53:12 MistInBuffer:drill (524) INFO: Should remove track 1 (../src/src/input/input_buffer.cpp:340)
13:53:12 MistInBuffer:drill (524) INFO: Disconnected track 2 (../src/src/input/input_buffer.cpp:536)
13:53:12 MistInBuffer:drill (524) INFO: Should remove track 2 (../src/src/input/input_buffer.cpp:340)
13:53:12 MistProcMKVExec:drill (528) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
13:53:12 MistProcMKVExec:drill (528) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
13:53:12 MistProcMKVExec:drill (528) INFO: Buffer completely played out (../src/src/output/output.cpp:2391)
13:53:12 MistProcMKVExec:drill (528) INFO: Logging clean exit reason: end of stream (../src/lib/config.cpp:63)
13:53:12 MistProcMKVExec:drill (528) INFO: Client handler shutting down, exit reason: end of stream (../src/src/output/output.cpp:2156)
13:53:12 MistProcMKVExec (528) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:53:12 MistInBuffer:drill (524) INFO: Logging clean exit reason: source disconnected for non-resumable stream (../src/lib/config.cpp:63)
13:53:12 MistInBuffer:drill (524) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
13:53:12 MistProcMKVExec (528) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:53:12 MistInTSSRT:drill (519) INFO: Waiting up to 10 seconds for 1 processes... (../src/lib/procs.cpp:123)
13:53:12 MistInTSSRT:drill (519) INFO: SIGINT 523 (../src/lib/procs.cpp:134)
13:53:12 MistInBuffer:drill (523) INFO: Logging clean exit reason: signal Interrupt (2) from process 519 (../src/lib/config.cpp:63)
13:53:12 MistInBuffer:drill (523) INFO: Received signal Interrupt (2) from process 519 (../src/lib/config.cpp:683)
13:53:12 MistInBuffer:drill (523) INFO: Shutting down input for stream drill because of signal interrupt... (../src/src/input/input.cpp:602)
13:53:12 MistInBuffer:drill (524) INFO: Received signal Terminated (15) from process 523 (../src/lib/config.cpp:683)
13:53:12 MistProcMKVExec (528) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:53:12 MistProcMKVExec (528) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:53:12 MistProcMKVExec (528) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:53:12 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
13:53:12 MistProcMKVExec:drill→drill (528) INFO: Logging clean exit reason: no more data (../src/lib/config.cpp:63)
13:53:12 MistProcMKVExec:drill→drill (528) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
13:53:13 MistProcMKVExec (528) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:53:13 MistProcMKVExec (528) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:53:13 MistProcMKVExec (528) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:53:13 MistInTSSRT:drill (519) INFO: SIGINT 523 (../src/lib/procs.cpp:134)
13:53:13 MistInBuffer:drill (523) INFO: Received signal Interrupt (2) from process 519 (../src/lib/config.cpp:683)
13:53:13 MistInBuffer:drill (523) INFO: Shutting down input for stream drill because of signal interrupt... (../src/src/input/input.cpp:602)
13:53:13 MistInBuffer:drill (524) INFO: Received signal Terminated (15) from process 523 (../src/lib/config.cpp:683)
13:53:13 MistInBuffer:drill (524) INFO: Waiting up to 5 seconds for 1 processes... (../src/lib/procs.cpp:123)
13:53:13 MistInBuffer:drill (524) INFO: SIGINT 528 (../src/lib/procs.cpp:134)
13:53:13 MistProcMKVExec (528) INFO: Logging clean exit reason: signal Interrupt (2) from process 524 (../src/lib/config.cpp:63)
13:53:13 MistProcMKVExec (528) INFO: Received signal Interrupt (2) from process 524 (../src/lib/config.cpp:683)
13:53:13 MistProcMKVExec (528) INFO: Stopping process... (../src/src/process/process_exec.cpp:234)
13:53:13 MistProcMKVExec (528) INFO: Closing process clean (../src/src/process/process_exec.cpp:239)
13:53:14 MistInTSSRT:drill (622) INFO: Initialized libsrt version 1.5.6 (../src/lib/socket_srt.cpp:46)
13:53:14 MistInTSSRT:drill (622) INFO: Parsed url: srt://ingest:8889?streamid=live+drill (../src/src/input/input_tssrt.cpp:210)
13:53:15 MistInTSSRT:drill (622) INFO: Caller SRT socket 284039997 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
13:53:15 MistInTSSRT:drill (622) INFO: Starting stream (../src/src/input/input.cpp:741)
13:53:15 MistInBuffer:drill (627) INFO: Setting bufferTime from 50000 to new value of 60000 (../src/src/input/input_buffer.cpp:604)
13:53:15 MistInBuffer:drill (627) INFO: Starting serve (../src/src/input/input.cpp:745)
13:53:15 MistInBuffer:drill (627) INFO: Input started (../src/src/input/input.cpp:898)
13:53:15 MistInBuffer:drill (627) INFO: Setting segmentSize from 1900 to new value of 6000 (../src/src/input/input_buffer.cpp:647)
13:53:15 MistInBuffer:drill (627) INFO: Setting maxKeepAway from 0 to new value of 45000 (../src/src/input/input_buffer.cpp:655)
13:53:15 MistInTSSRT:drill (622) INFO: Input started (../src/src/input/input.cpp:1029)
13:53:19 MistInTSSRT:drill (622) INFO: Creating new (delayed) track 0: JSON meta (../src/lib/dtsc.cpp:2110)
13:53:19 MistInTSSRT:drill (622) INFO: Creating new (delayed) track 1: AAC audio (../src/lib/dtsc.cpp:2110)
13:53:19 MistInBuffer:drill (627) INFO: Started process 644: /usr/local/bin/MistProcMKVExec {"codec":"H264","exec":"ffmpeg -loglevel warning -hide_banner -fflags nobuffer -i - -vf scale=426:240 -c:v libx264 -preset veryfast -tune zerolatency -profile:v main -pix_fmt yuv420p -b:v 300k -maxrate 330k -bufsize 600k -force_key_frames source -bf 0 -threads 2 -c:a aac -b:a 64k -ac 2 -ar 48000 -f matroska -cluster_time_limit 0 -","exit_unmask":0,"inconsequential":0,"process":"MKVExec","resolution":"426x240","restart_type":"fixed","source":"drill","source_mask":4,"sources":null,"tags_inhibit":null,"target_mask":1,"target_profiles":[{"bitrate":300,"height":240,"name":"240p","width":426}],"track_select":"audio=all&video=all","x-LSP-kind":"video"} (../src/src/input/input_buffer.cpp:843)
13:53:19 MistProcMKVExec (644) INFO: No sink explicitly set, using source as sink (../src/src/process/process_exec.cpp:180)
13:53:19 MistProcMKVExec (644) INFO: Executing command: ffmpeg -loglevel warning -hide_banner -fflags nobuffer -i - -vf scale=426:240 -c:v libx264 -preset veryfast -tune zerolatency -profile:v main -pix_fmt yuv420p -b:v 300k -maxrate 330k -bufsize 600k -force_key_frames source -bf 0 -threads 2 -c:a aac -b:a 64k -ac 2 -ar 48000 -f matroska -cluster_time_limit 0 - (../src/src/process/process_exec.cpp:203)
13:53:19 MistProcMKVExec:drill→drill (644) INFO: Starting stream (../src/src/input/input.cpp:741)
13:53:19 MistProcMKVExec:drill→drill (644) INFO: Input started (../src/src/input/input.cpp:1029)
13:53:19 MistProcMKVExec:drill (644) INFO: Stripping target options: audio=all&video=all (../src/src/output/output.cpp:156)
13:53:19 MistProcMKVExec:drill (644) FAIL: Cannot assign address from size-less pointer: address family not recognized (../src/lib/socket.cpp:224)
13:53:19 MistProcMKVExec:drill (644) INFO: Booting new session O6efd1a047264662b7f0a32390243f919715717c198d8c33c15dbf4387f74714a (../src/lib/comms.cpp:392)
13:53:19 MistController (8) INFO: Discovered ongoing push 644, adding to push list (../src/src/controller/controller_push.cpp:106)
13:53:19 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
13:53:20 MistInTSSRT:drill (622) INFO: Creating new (delayed) track 2: H264 video (../src/lib/dtsc.cpp:2110)
13:53:20 MistInTSSRT:drill (622) INFO: Track 2 page 0 not starting with a keyframe! (../src/src/io.cpp:503)
13:53:21 MistProcMKVExec:drill (644) INFO: Track 1 seekable (../src/src/output/output.cpp:943)
13:53:21 MistProcMKVExec:drill (644) INFO: Track 2 seekable (../src/src/output/output.cpp:943)
13:53:21 MistProcMKVExec:drill (644) INFO: Recording will start at timestamp 0d00h00m12s.021 (../src/src/output/output.cpp:1299)
13:53:21 MistProcMKVExec:drill (644) INFO: Track 1 seekable (../src/src/output/output.cpp:943)
13:53:21 MistProcMKVExec:drill (644) INFO: Track 2 seekable (../src/src/output/output.cpp:943)
13:53:21 MistProcMKVExec:drill (644) INFO: Outputting drill to stdout with MKVExec format (../src/src/output/output.cpp:1851)
13:53:21 MistProcMKVExec:drill (644) INFO: Masking source track 1 with 4 (../src/src/process/process_exec.cpp:125)
13:53:21 MistProcMKVExec:drill (644) INFO: Masking source track 2 with 4 (../src/src/process/process_exec.cpp:125)
13:53:23 MistProcMKVExec:drill→drill (644) INFO: Detected track 1 => 3: video_H264_426x240_0fps (../src/lib/ebml_socketglue.cpp:443)
13:53:23 MistProcMKVExec:drill→drill (644) INFO: Resizing metadata track list from 4 to 8 (../src/lib/dtsc.cpp:1350)
13:53:23 MistProcMKVExec:drill→drill (644) INFO: Detected track 2 => 4: audio_AAC_2ch_48000hz (../src/lib/ebml_socketglue.cpp:443)
13:53:23 MistProcMKVExec:drill (644) INFO: Reloading entire metadata (../src/lib/dtsc.cpp:1513)
13:53:23 MistInTSSRT:drill (622) INFO: Reloading entire metadata (../src/lib/dtsc.cpp:1513)
13:53:23 MistInBuffer:drill (627) INFO: Reloading entire metadata (../src/lib/dtsc.cpp:1513)
```

---

# nmw-3124-dev -- verification of the NMW-3124 fix (NMW-3130; units 03 + 04 applied)

Run on 2026-10-30 (11:29-12:35 host time, UTC-3) on the same workstation, same harness, same configs.
Image under test: `nedmedia/mistserver:nmw-3124-dev`, built from this worktree with
`docker build -f Dockerfile.mistserver --build-arg BUILDKIT_SYNTAX=docker/dockerfile:1 -t nedmedia/mistserver:nmw-3124-dev .`
(`MistInTSSRT --help` in the image shows the new `--idletimeout` text "no packet was delivered to a track, or the
stream had no live track"; `nedmedia-v2` still shows "no bytes were received"). The transcode and source
containers ran `nedmedia/mistserver:nmw-3124-dev-drill` (= `nmw-3124-dev` + `apk add ffmpeg curl`, Mist
binaries untouched, built with `docker build -f Dockerfile.drill --build-arg BASE=nedmedia/mistserver:nmw-3124-dev`);
the ingest stayed on `nedmedia/mistserver:nedmedia-v2` as in production. Raw data in
`run/results-nmw-3124-dev/<trigger>/` and `run/results-nedmedia-v2-control/<trigger>/` (gitignored).

Harness changes for this run (`drill.sh`): `RESULTS_DIR` env knob so the nedmedia-v2 baselines above are not
overwritten; new triggers `srcpause40` (the NMW-2106 case: `docker pause` the ffmpeg **source** 40 s, ingest
untouched) and `kill120` (ingest down longer than the puller's 10 SRT connect attempts, so the new
"connect failed -> fail preRun" path is exercised). Steady state is `none` with `SAMPLE_FOR=300`.

What the fix changes, as seen in the logs (new lines, with the file:line of the image under test):

* `MistInTSSRT:drill WARN: No usable data received over SRT for 10002 ms (limit 10000 ms) - closing connection (input_tssrt.cpp:276)`
  -- the idle watchdog, now driven by "packet mapped to a track" instead of "bytes received" (was
  `No data received over SRT for ... ms` at input_tssrt.cpp:289 in nedmedia-v2).
* `MistInTSSRT:drill WARN: Stream has had no live track for 10010 ms (limit 10000 ms) - closing SRT connection (input_tssrt.cpp:290)`
  -- the new no-tracks watchdog (NMW-3129).
* `MistInTSSRT:drill WARN: Connecting to srt://ingest:8889?streamid=live+drill timed out (input_tssrt.cpp:261)` followed by
  `FAIL: Logging unclean exit reason: Could not connect to SRT source srt://ingest:8889?streamid=live+drill`
  -- preRun now fails instead of returning true and spawning a MistInBuffer for a source that was never reached.
  In nedmedia-v2 the same moment reads `Connecting to ... timed out (input_tssrt.cpp:249)` +
  `Input closing without a set exit reason` and a MistInBuffer is started and killed one second later.

## Pass / fail against the NMW-3130 "done when" list

| check | result | evidence |
|---|---|---|
| after each restart trigger the transcode serves `drill` with video + audio and HLS 200, no `nuke_stream` | **pass** (all 8 unit-01 triggers + `kill120`) | end state `1/3/200` (`AAC,H264240,JSON`) for every trigger; `nuke_stream` never called; zero samples with `live=1, tracks=0, puller alive` |
| ... **within 2 x idletimeout (20 s) of the ingest coming back** | **fail, on both images** | HLS 200 came 61-74 s after the ingest was back (`restart` 74 s, `kill5` 68, `kill20` 69, `kill60` 64, `pause15` 68, `pause40` 65, `restart-srcpaused` 63, `kill120` 61; `restart-x3` 40 s after the 3rd restart). nedmedia-v2 baseline: 65-75 s. `json_drill.js` shows all 3 tracks within 5-10 s of the ingest being back; it is HLS that stays at timeout. See "Why recovery still takes a minute" below -- not a MistInTSSRT defect, unchanged by this fix |
| no `A pull process for this stream is already running` after the stream is serving again | **pass** | every such line sits inside a window where the controller has logged `Stream drill became inactive` and not yet `became active` (per-trigger timing below); none after the final `became active`; none in the 5-min steady state |
| NMW-2106 silence case (`srcpause40`) | **pass** | puller 153: `Stream has had no live track for 10010 ms ... closing SRT connection` at +10 s; the re-pull (444) `No usable data received over SRT for 10002 ms ... closing connection` at +24 s; stream active 7 s after the source resumed, HLS 200 34 s after. Identical timeline on the nedmedia-v2 control (`No data received over SRT for 10007 ms` there), so no regression and no change in behaviour for this case |
| steady state: 5 min unbroken pull, no spurious watchdog exit | **pass** | 118/118 samples `1/3/200`, MistInTSSRT pid 151 alive for the whole window (ps at the end: angel 70 etime 6:39, child 151 etime 6:06), zero `No usable data` / `no live track` / `Input closing` lines in the window (15:10:29-15:15:30 UTC) |
| `kill120` (extra): connect-failure path | **pass** | puller 415: 10 x `Can't connect SRT socket to any address for ingest` 11 s apart -> `Connecting to ... timed out` -> `Could not connect to SRT source` -> exits; **no** MistInBuffer spawned for it, **no** angel restart (`uncleanly shut down! Cleaning and restarting` absent); the controller re-pulled 3 s later (pid 627) once the ingest was up, and recovery was the usual 61 s |

Fixed image vs baseline, same columns as the unit-01 table plus `HLS 200 after ingest back` (s), watchdog
lines `idle/no-tracks/connect-fail` and `no activity for 30s` buffer exits in the window:

| trigger | first unhealthy (s) | healthy again (s) | HLS 200 after ingest back (s) | peak MistInTSSRT | peak MistOutHLS | `already running` | `Dropping invalid track` | `Requested invalid track` | `live=1,tracks=0` samples | watchdog idle/notracks/connfail | buffer `no activity` exits | end |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `restart` | 9 | 75 | 74 | 4 | 14 | 1 | 2 | 0 | 0 | 0/0/0 | 1 | 1/3/200 |
| `kill5` | 7 | 73 | 68 | 5 | 14 | 2 | 2 | 0 | 0 | 0/0/0 | 1 | 1/3/200 |
| `kill20` | 7 | 89 | 69 | 8 | 19 | 6 | 2 | 0 | 0 | 0/0/0 | 1 | 1/3/200 |
| `kill60` | 8 | 121 | 64 | 28 | 30 | 21 | 2 | 0 | 0 | 0/0/0 | 0 | 1/3/200 |
| `pause15` | 2 | 83 | 68 | 6 | 16 | 4 | 2 | 0 | 0 | 0/0/0 | 1 | 1/3/200 |
| `pause40` | 3 | 101 | 65 | 11 | 23 | 9 | 2 | 0 | 0 | 0/0/0 | 1 | 1/3/200 |
| `restart-srcpaused` | 10 | 85 | 63 | 5 | 14 | 1 | 2 | 0 | 0 | 0/0/0 | 1 | 1/3/200 |
| `restart-x3` | 10 | 108 | 40 (after 3rd) | 3 | 14 | 0 | 4 | 0 | 0 | 0/0/0 | 1 | 1/3/200 |
| `kill120` (extra) | 7 | 181 | 61 | 60 | 47 | 53 | 2 | 0 | 0 | 0/0/1 | 0 | 1/3/200 |
| `srcpause40` (NMW-2106) | 13 | 73 | 34 (after source resumed) | 10 | 14 | 4 | 2 | 0 | 0 | 1/1/0 | 0 | 1/3/200 |
| `none` (steady, 300 s) | - | - | - | 2 | 1 | 0 | 0 | 0 | 0 | 0/0/0 | 0 | 1/3/200 |
| control nedmedia-v2 `srcpause40` | 13 | 73 | 34 | 10 | 15 | 4 | 2 | 0 | 0 | 1 (old text)/-/0 | 0 | 1/3/200 |
| control nedmedia-v2 `kill120` | 7 | 181 | 62 | 59 | 44 | 53 | 2 | 0 | 0 | 0/-/0 | 0 | 1/3/200 |

(`buffer no activity exits` counts only the one inside the recovery; every stack also logs one at cold boot,
see below. In the unit-01 baseline table these INFO lines were partly compiled out, which is why its counts
differ.) The nedmedia-v2 baselines for the 8 original triggers are the ones recorded by unit 01 above
(same host, same harness, 1-2 h earlier); they were not re-run.

## Verdict

* **The wedge did not reproduce on either image** (as in unit 01), so this run cannot show the fix curing
  it; what it shows is that the fix does not regress any trigger, that the three new code paths fire when
  they should and are handled cleanly by the angel/controller, and that a healthy pull is never cut.
* **The fix does not change the recovery time after an ingest restart** (61-74 s on both images). The
  "within 2 x idletimeout" criterion fails on the fixed image exactly as it does on nedmedia-v2.

## Why recovery still takes a minute (both images, also at every cold start)

Every stream start in this harness -- cold boot and every post-restart reconnect alike -- goes through the
same two cycles, and HLS only answers in the second one:

```
15:08:54 MistInTSSRT:drill (71)  INFO: Starting stream                                     <- cold boot of the steady-state run
15:08:58 MistController (8)      INFO: Stream drill became active
15:09:23 MistInBuffer:drill (78) INFO: Input closing cleanly with reason: no activity for 30s (> 30s)
15:09:23 MistInTSSRT:drill (71)  INFO: Input closing cleanly with reason: buffer requested shutdown
15:09:25 MistInTSSRT:drill (70)  WARN: Input for stream drill uncleanly shut down! Cleaning and restarting...
15:09:30 MistController (8)      INFO: Stream drill became active                            <- HLS 200 ~25 s after this
```

and after `restart` on the fixed image:

```
14:31:24 MistInTSSRT:drill (428) INFO: Caller SRT socket 197734269 connected targetting ingest:8889      <- ingest back at 14:31:16
14:31:28 MistController (8)      INFO: Stream drill became active                                        <- json_drill.js: AAC,H264240,JSON from t=24
14:31:54 MistInBuffer:drill (433) INFO: Input closing cleanly with reason: no activity for 30s (> 30s)   <- HLS still 000 throughout
14:31:54 MistInTSSRT:drill (428) INFO: Input closing cleanly with reason: buffer requested shutdown
14:31:55 MistController (8)      INFO: Stream drill became inactive
14:32:00 MistInTSSRT:drill (550) WARN: A pull process for this stream is already running                <- during the inactive gap, counted in the table
14:32:04 MistController (8)      INFO: Stream drill became active
14:32:30                          HLS 200 (t=75), the 14 hung MistOutHLS drain in the same sample
```

The first MistInBuffer instance exits with `no activity for 30s` 25 s after the stream became active even
though `json_drill.js` already lists all three tracks and the HLS demand loop is requesting once a second;
the MistOutHLS processes it spawns hang silently (nothing from them in the log at this debug level) and pile
up (14-47) until the second cycle, when they all drain at once and the index answers 200. The MistInTSSRT
watchdogs are not involved (no watchdog line in any restart trigger), and the puller delivers data the whole
time, so this is outside the scope of units 03/04. It is the same "double MKVExec cycle" unit 01 noted;
it should be looked at separately (why the first buffer instance sees no activity, and why the HLS outputs
block instead of answering) if the production outage per ingest restart is to drop below a minute.

Other observations, unchanged from nedmedia-v2:

* While the ingest is down, each HLS request still spawns a MistInTSSRT that logs `already running` or
  `Can't connect SRT socket` and lingers: 28 pullers / 30 MistOutHLS after 60 s down, 60 / 47 after 120 s.
  They drain within ~10 s of the stream serving again. The fix does not address this (not in its scope).
* The angel treats the `buffer requested shutdown` exit as unclean (`Input for stream drill uncleanly shut
  down! Cleaning and restarting...`) and restarts its child, while a preRun failure (`Could not connect to
  SRT source`) is *not* restarted by the angel and is re-pulled by the controller -- which is what unit 03
  intended.
* `srcpause40`: which of the two 10 s watchdogs fires first depends on a 250 ms refresh lag in the no-tracks
  clock (`lastTrackMs` is refreshed at most every 250 ms, so it can expire up to 250 ms before `lastRecvMs`);
  both log a clean `ER_CLEAN_INACTIVE` exit and the result is the same. Cosmetic.

## Commands run

```
docker build -f Dockerfile.mistserver --build-arg BUILDKIT_SYNTAX=docker/dockerfile:1 -t nedmedia/mistserver:nmw-3124-dev .     # exit 0 (cached: identical source already built)
cd test/srt-restart-drill
docker build -f Dockerfile.drill --build-arg BASE=nedmedia/mistserver:nmw-3124-dev -t nedmedia/mistserver:nmw-3124-dev-drill .
TRANSCODE_IMAGE=nedmedia/mistserver:nmw-3124-dev-drill RESULTS_DIR=$PWD/run/results-nmw-3124-dev ./drill.sh all                  # 8 triggers, run/all-nmw-3124-dev.log
SAMPLE_FOR=300 TRANSCODE_IMAGE=... RESULTS_DIR=... ./drill.sh all none                                                           # steady state
TRANSCODE_IMAGE=... RESULTS_DIR=... ./drill.sh all "srcpause40 kill120"                                                          # NMW-2106 case + long outage
TRANSCODE_IMAGE=nedmedia/mistserver:nedmedia-v2-drill RESULTS_DIR=$PWD/run/results-nedmedia-v2-control ./drill.sh all "srcpause40 kill120"   # controls; run/phase2.log
docker compose down -v   (done by drill.sh all after every batch; no drill containers, network or volumes left)
```

## Per-trigger timelines (nmw-3124-dev unless marked CONTROL)

### `restart` -- (a) `docker restart ingest`

Trigger log (host clock, UTC-3):

```
11:31:14 T: docker restart drill-ingest
11:31:16 T: restart done
```

Verdict: samples=59 first_unhealthy_t=9 recovered_t=75 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=1 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 11:31:14 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 11:31:16 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 4 | 11:31:19 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 7 | 11:31:21 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 9 | 11:31:28 | 1 | 3 | err | 0 | Stream_is_offline | 1 | 0 | 0 | 1 | 000 | 0 | 0 | 2 |
| 16 | 11:31:36 | 1 | 3 | 1 | 1 | JSON | 2 | 2 | 1 | 3 | 000 | 0 | 0 | 2 |
| 24 | 11:31:43 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 5 | 000 | 0 | 0 | 2 |
| 31 | 11:31:50 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 7 | 000 | 0 | 0 | 2 |
| 38 | 11:31:57 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 9 | 000 | 0 | 0 | 2 |
| 45 | 11:32:05 | 1 | 3 | err | 0 | Stream_is_offline | 3 | 0 | 0 | 12 | 000 | 1 | 0 | 2 |
| 53 | 11:32:12 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 4 | 2 | 1 | 14 | 000 | 1 | 0 | 2 |
| 60 | 11:32:19 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 13 | 000 | 1 | 0 | 2 |
| 67 | 11:32:27 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 13 | 000 | 1 | 0 | 2 |
| 75 | 11:32:30 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 14 | 200 | 1 | 0 | 2 |
| 78 | 11:32:32 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 80 | 11:32:35 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 83 | 11:32:37 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 113 | 11:33:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 143 | 11:33:37 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 173 | 11:34:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 180 | 11:34:14 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
14:31:21 MistInTSSRT:drill (154) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:31:21 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
14:31:21 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
14:31:22 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
14:31:22 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:31:22 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:31:24 MistInTSSRT:drill (428) INFO: Caller SRT socket 197734269 connected targetting ingest:8889 -> [::ffff:172.22.0.4]:8889 (../src/lib/socket_srt.cpp:469)
14:31:28 MistInBuffer:drill (433) INFO: Started process 437: /usr/local/bin/MistProcMKVExec {...}
14:31:28 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
14:31:54 MistInBuffer:drill (433) INFO: Input closing cleanly with reason: no activity for 30s (> 30s) (../src/src/input/input.cpp:825)
14:31:54 MistInTSSRT:drill (428) INFO: Input closing cleanly with reason: buffer requested shutdown (../src/src/input/input.cpp:825)
14:31:55 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:31:56 MistProcMKVExec:drill→drill (437) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:32:00 MistInTSSRT:drill (550) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:32:00 MistInTSSRT:drill (532) INFO: Caller SRT socket 755246417 connected targetting ingest:8889 -> [::ffff:172.22.0.4]:8889 (../src/lib/socket_srt.cpp:469)
14:32:03 MistInBuffer:drill (552) INFO: Started process 556: /usr/local/bin/MistProcMKVExec {...}
14:32:04 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `kill5` -- (b) `docker kill ingest`, `compose up -d ingest` after 5 s

Trigger log (host clock, UTC-3):

```
11:36:04 T: docker kill drill-ingest
11:36:09 T: compose up -d ingest after 5s
11:36:10 T: ingest up
```

Verdict: samples=59 first_unhealthy_t=7 recovered_t=73 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=2 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 11:36:04 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 11:36:07 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 11:36:09 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 7 | 11:36:16 | - | - | err | 0 | Stream_is_offline | 2 | 0 | 0 | 1 | 000 | 1 | 0 | 2 |
| 14 | 11:36:23 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 3 | 2 | 0 | 3 | 000 | 1 | 0 | 2 |
| 21 | 11:36:31 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 5 | 000 | 1 | 0 | 2 |
| 29 | 11:36:38 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 7 | 000 | 1 | 0 | 2 |
| 36 | 11:36:45 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 9 | 000 | 1 | 0 | 2 |
| 43 | 11:36:53 | 1 | 3 | err | 0 | Stream_is_offline | 4 | 0 | 0 | 12 | 000 | 2 | 0 | 2 |
| 51 | 11:37:00 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 14 | 000 | 2 | 0 | 2 |
| 58 | 11:37:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 14 | 000 | 2 | 0 | 2 |
| 65 | 11:37:15 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 13 | 000 | 2 | 0 | 2 |
| 73 | 11:37:18 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 13 | 200 | 2 | 0 | 2 |
| 76 | 11:37:21 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 1 | 200 | 2 | 0 | 2 |
| 79 | 11:37:23 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 81 | 11:37:25 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 111 | 11:37:56 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 141 | 11:38:26 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 172 | 11:38:56 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |
| 181 | 11:39:05 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 2 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
14:36:09 MistInTSSRT:drill (154) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:36:09 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
14:36:09 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
14:36:10 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
14:36:10 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:36:10 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:36:11 MistInTSSRT:drill (418) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:36:15 MistInTSSRT:drill (407) INFO: Caller SRT socket 706857096 connected targetting ingest:8889 -> 172.22.0.3:8889 (../src/lib/socket_srt.cpp:469)
14:36:18 MistInBuffer:drill (422) INFO: Started process 435: /usr/local/bin/MistProcMKVExec {...}
14:36:19 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
14:36:44 MistInBuffer:drill (422) INFO: Input closing cleanly with reason: no activity for 30s (> 30s) (../src/src/input/input.cpp:825)
14:36:44 MistProcMKVExec:drill→drill (435) INFO: Input closing cleanly with reason: buffer requested shutdown (../src/src/input/input.cpp:825)
14:36:44 MistInTSSRT:drill (407) INFO: Input closing cleanly with reason: buffer requested shutdown (../src/src/input/input.cpp:825)
14:36:44 MistProcMKVExec:drill (435) FAIL: Logging unclean exit reason: set inactive (../src/lib/config.cpp:65)
14:36:44 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:36:48 MistInTSSRT:drill (532) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:36:48 MistInTSSRT:drill (520) INFO: Caller SRT socket 738656268 connected targetting ingest:8889 -> [::ffff:172.22.0.3]:8889 (../src/lib/socket_srt.cpp:469)
14:36:52 MistInBuffer:drill (534) INFO: Started process 544: /usr/local/bin/MistProcMKVExec {...}
14:36:52 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `kill20` -- (b) `docker kill ingest`, `compose up -d ingest` after 20 s

Trigger log (host clock, UTC-3):

```
11:40:55 T: docker kill drill-ingest
11:41:15 T: compose up -d ingest after 20s
11:41:16 T: ingest up
```

Verdict: samples=50 first_unhealthy_t=7 recovered_t=89 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=6 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 11:40:55 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 11:40:57 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 4 | 11:41:00 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 7 | 11:41:07 | - | - | err | 0 | Stream_is_offline | 2 | 2 | 1 | 1 | 000 | 0 | 0 | 2 |
| 14 | 11:41:14 | - | - | err | 0 | Stream_is_offline | 2 | 0 | 0 | 3 | 000 | 4 | 0 | 2 |
| 21 | 11:41:22 | - | - | err | 0 | Stream_is_offline | 6 | 0 | 0 | 5 | 000 | 6 | 0 | 2 |
| 29 | 11:41:29 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 8 | 2 | 0 | 7 | 000 | 6 | 0 | 2 |
| 36 | 11:41:37 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 9 | 000 | 6 | 0 | 2 |
| 44 | 11:41:45 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 12 | 000 | 6 | 0 | 2 |
| 52 | 11:41:52 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 14 | 000 | 6 | 0 | 2 |
| 59 | 11:42:00 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 8 | 2 | 0 | 16 | 000 | 6 | 0 | 2 |
| 67 | 11:42:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 19 | 000 | 6 | 0 | 2 |
| 74 | 11:42:15 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 6 | 0 | 2 |
| 82 | 11:42:22 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 12 | 000 | 6 | 0 | 2 |
| 89 | 11:42:25 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 6 | 0 | 2 |
| 92 | 11:42:28 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 6 | 0 | 2 |
| 95 | 11:42:30 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 6 | 0 | 2 |
| 97 | 11:42:33 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 6 | 0 | 2 |
| 128 | 11:43:04 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 6 | 0 | 2 |
| 159 | 11:43:35 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 6 | 0 | 2 |
| 180 | 11:43:55 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 6 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
14:41:00 MistInTSSRT:drill (154) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:41:00 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
14:41:00 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
14:41:01 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
14:41:01 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:41:01 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:41:09 MistInTSSRT:drill (424) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:41:12 MistOutHLS:drill (403) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
14:41:12 MistOutHLS:drill (403) FAIL: Logging unclean exit reason: Could not connect to stream (../src/lib/config.cpp:65)
14:41:12 MistInTSSRT:drill (431) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:41:12 MistOutHLS:drill (411) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
14:41:12 MistOutHLS:drill (411) FAIL: Logging unclean exit reason: Could not connect to stream (../src/lib/config.cpp:65)
14:41:12 MistInTSSRT:drill (432) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
14:41:17 MistInTSSRT:drill (443) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:41:18 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:41:20 MistInTSSRT:drill (447) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:41:21 MistInTSSRT:drill (414) INFO: Caller SRT socket 827805566 connected targetting ingest:8889 -> [::ffff:172.22.0.3]:8889 (../src/lib/socket_srt.cpp:469)
14:41:24 MistInBuffer:drill (449) INFO: Started process 466: /usr/local/bin/MistProcMKVExec {...}
14:41:25 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
14:41:50 MistInBuffer:drill (449) INFO: Input closing cleanly with reason: no activity for 30s (> 30s) (../src/src/input/input.cpp:825)
14:41:50 MistInTSSRT:drill (414) INFO: Input closing cleanly with reason: buffer requested shutdown (../src/src/input/input.cpp:825)
14:41:50 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:41:52 MistProcMKVExec:drill→drill (466) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:41:52 MistInTSSRT:drill (413) WARN: Input for stream drill uncleanly shut down! Cleaning and restarting... (../src/src/input/input.cpp:664)
14:41:53 MistInTSSRT:drill (550) INFO: Caller SRT socket 635706550 connected targetting ingest:8889 -> [::ffff:172.22.0.3]:8889 (../src/lib/socket_srt.cpp:469)
14:41:57 MistInBuffer:drill (555) INFO: Started process 566: /usr/local/bin/MistProcMKVExec {...}
14:41:57 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `kill60` -- (b) `docker kill ingest`, `compose up -d ingest` after 60 s

Trigger log (host clock, UTC-3):

```
11:45:45 T: docker kill drill-ingest
11:46:46 T: compose up -d ingest after 60s
11:46:46 T: ingest up
```

Verdict: samples=41 first_unhealthy_t=8 recovered_t=121 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=21 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 11:45:45 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 1 | 200 | 0 | 0 | 0 |
| 2 | 11:45:48 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 11:45:51 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 8 | 11:45:58 | - | - | err | 0 | Stream_is_offline | 2 | 0 | 0 | 1 | 000 | 2 | 0 | 2 |
| 15 | 11:46:06 | - | - | err | 0 | Stream_is_offline | 4 | 0 | 0 | 3 | 000 | 4 | 0 | 2 |
| 23 | 11:46:13 | - | - | err | 0 | Stream_is_offline | 6 | 0 | 0 | 5 | 000 | 6 | 0 | 2 |
| 30 | 11:46:21 | - | - | err | 0 | Stream_is_offline | 8 | 0 | 0 | 7 | 000 | 8 | 0 | 2 |
| 38 | 11:46:29 | - | - | err | 0 | Stream_is_offline | 11 | 0 | 0 | 10 | 000 | 11 | 0 | 2 |
| 46 | 11:46:36 | - | - | err | 0 | Stream_is_offline | 13 | 0 | 0 | 12 | 000 | 13 | 0 | 2 |
| 53 | 11:46:44 | - | - | err | 0 | Stream_is_offline | 15 | 0 | 0 | 14 | 000 | 15 | 0 | 2 |
| 61 | 11:46:51 | - | - | err | 0 | Stream_is_offline | 18 | 0 | 0 | 17 | 000 | 17 | 0 | 2 |
| 68 | 11:46:59 | err | 0 | err | 0 | Stream_is_waiting_for_data | 19 | 2 | 0 | 19 | 000 | 18 | 0 | 2 |
| 76 | 11:47:06 | err | 0 | err | 0 | Stream_is_waiting_for_data | 22 | 2 | 0 | 21 | 000 | 19 | 0 | 2 |
| 83 | 11:47:14 | err | 0 | err | 0 | Stream_is_waiting_for_data | 24 | 2 | 0 | 23 | 000 | 20 | 0 | 2 |
| 91 | 11:47:21 | 1 | 3 | err | 0 | Stream_is_offline | 27 | 0 | 0 | 26 | 000 | 21 | 0 | 2 |
| 98 | 11:47:29 | 1 | 3 | 1 | 1 | JSON | 28 | 2 | 1 | 28 | 000 | 21 | 0 | 2 |
| 106 | 11:47:36 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 28 | 2 | 1 | 30 | 000 | 21 | 0 | 2 |
| 113 | 11:47:44 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 10 | 2 | 1 | 14 | 000 | 21 | 0 | 2 |
| 121 | 11:47:50 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 14 | 200 | 21 | 0 | 2 |
| 127 | 11:47:52 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 21 | 0 | 2 |
| 129 | 11:47:55 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 1 | 200 | 21 | 0 | 2 |
| 132 | 11:47:57 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 21 | 0 | 2 |
| 163 | 11:48:28 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 21 | 0 | 2 |
| 181 | 11:48:47 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 21 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
14:45:51 MistInTSSRT:drill (153) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:45:51 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
14:45:51 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
14:45:51 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
14:45:51 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:45:52 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:45:53 MistInTSSRT:drill (408) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
14:46:01 MistInTSSRT:drill (419) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:46:02 MistInTSSRT:drill (398) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:46:04 MistInTSSRT:drill (427) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
14:46:10 MistInTSSRT:drill (438) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:46:13 MistInTSSRT:drill (398) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:46:16 MistInTSSRT:drill (447) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 2 more 'A pull process for this stream is already running' lines ...
14:46:24 MistInTSSRT:drill (460) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:46:24 MistInTSSRT:drill (398) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:46:28 MistInTSSRT:drill (462) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
14:46:34 MistInTSSRT:drill (479) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:46:35 MistInTSSRT:drill (398) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:46:39 MistInTSSRT:drill (488) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:46:40 MistInTSSRT:drill (490) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:46:46 MistInTSSRT:drill (398) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:46:46 MistInTSSRT:drill (493) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:46:46 MistInTSSRT:drill (501) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:46:51 MistInTSSRT:drill (398) INFO: Caller SRT socket 47203410 connected targetting ingest:8889 -> 172.22.0.2:8889 (../src/lib/socket_srt.cpp:469)
14:46:54 MistInTSSRT:drill (398) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:46:55 MistInBuffer:drill (507) INFO: Input closing cleanly with reason: signal Terminated (15) from process 506 (../src/src/input/input.cpp:825)
14:46:55 MistInTSSRT:drill (540) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:46:56 MistInTSSRT:drill (536) INFO: Caller SRT socket 183083116 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
14:46:59 MistInTSSRT:drill (536) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:46:59 MistInBuffer:drill (542) INFO: Input closing cleanly with reason: signal Terminated (15) from process 541 (../src/src/input/input.cpp:825)
14:47:01 MistInTSSRT:drill (547) INFO: Caller SRT socket 237604519 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
14:47:04 MistInTSSRT:drill (547) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:47:04 MistInBuffer:drill (552) INFO: Input closing cleanly with reason: signal Terminated (15) from process 551 (../src/src/input/input.cpp:825)
14:47:05 MistInTSSRT:drill (574) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:47:06 MistInTSSRT:drill (570) INFO: Caller SRT socket 224036521 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
14:47:09 MistInTSSRT:drill (570) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:47:09 MistInBuffer:drill (576) INFO: Input closing cleanly with reason: signal Terminated (15) from process 575 (../src/src/input/input.cpp:825)
14:47:10 MistInTSSRT:drill (593) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:47:10 MistInTSSRT:drill (588) INFO: Caller SRT socket 211123057 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
14:47:13 MistInTSSRT:drill (588) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:47:14 MistInBuffer:drill (595) INFO: Input closing cleanly with reason: signal Terminated (15) from process 594 (../src/src/input/input.cpp:825)
14:47:16 MistInTSSRT:drill (612) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:47:17 MistInTSSRT:drill (601) INFO: Caller SRT socket 317800656 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
14:47:21 MistInBuffer:drill (614) INFO: Started process 618: /usr/local/bin/MistProcMKVExec {...}
14:47:21 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `pause15` -- (c) `docker pause ingest` 15 s, unpause

Trigger log (host clock, UTC-3):

```
11:50:39 T: docker pause drill-ingest
11:50:54 T: unpaused after 15s
```

Verdict: samples=50 first_unhealthy_t=2 recovered_t=83 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=4 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 11:50:39 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 11:50:50 | - | - | err | 0 | Stream_is_offline | 2 | 2 | 1 | 0 | 000 | 0 | 0 | 2 |
| 13 | 11:51:00 | 1 | 3 | err | 0 | Stream_is_offline | 2 | 0 | 0 | 3 | 000 | 3 | 0 | 2 |
| 23 | 11:51:07 | 1 | 3 | 1 | 1 | JSON | 5 | 2 | 1 | 5 | 000 | 3 | 0 | 2 |
| 30 | 11:51:15 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 7 | 000 | 3 | 0 | 2 |
| 38 | 11:51:22 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 10 | 000 | 3 | 0 | 2 |
| 45 | 11:51:30 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 12 | 000 | 4 | 0 | 2 |
| 53 | 11:51:37 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 6 | 2 | 1 | 14 | 000 | 4 | 0 | 2 |
| 60 | 11:51:45 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 6 | 2 | 1 | 16 | 000 | 4 | 0 | 2 |
| 68 | 11:51:52 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 14 | 000 | 4 | 0 | 2 |
| 75 | 11:52:00 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 15 | 000 | 4 | 0 | 2 |
| 83 | 11:52:02 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 85 | 11:52:05 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 88 | 11:52:08 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 91 | 11:52:10 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 121 | 11:52:41 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 152 | 11:53:12 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 181 | 11:53:40 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
14:50:44 MistInTSSRT:drill (153) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:50:44 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
14:50:44 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
14:50:45 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
14:50:45 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:50:45 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:50:55 MistInTSSRT:drill (415) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:50:55 MistOutHLS:drill (393) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
14:50:55 MistOutHLS:drill (393) FAIL: Logging unclean exit reason: Could not connect to stream (../src/lib/config.cpp:65)
14:50:55 MistInTSSRT:drill (418) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:50:56 MistOutHLS:drill (394) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
14:50:56 MistOutHLS:drill (394) FAIL: Logging unclean exit reason: Could not connect to stream (../src/lib/config.cpp:65)
14:50:56 MistInTSSRT:drill (419) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:50:56 MistInTSSRT:drill (397) INFO: Caller SRT socket 543190004 connected targetting ingest:8889 -> 172.22.0.2:8889 (../src/lib/socket_srt.cpp:469)
14:51:00 MistInBuffer:drill (421) INFO: Started process 428: /usr/local/bin/MistProcMKVExec {...}
14:51:01 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
14:51:25 MistInBuffer:drill (421) INFO: Input closing cleanly with reason: no activity for 30s (> 30s) (../src/src/input/input.cpp:825)
14:51:25 MistInTSSRT:drill (397) INFO: Input closing cleanly with reason: buffer requested shutdown (../src/src/input/input.cpp:825)
14:51:26 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:51:27 MistProcMKVExec:drill→drill (428) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:51:27 MistInTSSRT:drill (396) WARN: Input for stream drill uncleanly shut down! Cleaning and restarting... (../src/src/input/input.cpp:664)
14:51:28 MistInTSSRT:drill (525) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:51:28 MistInTSSRT:drill (520) INFO: Caller SRT socket 1011529686 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
14:51:32 MistInBuffer:drill (527) INFO: Started process 530: /usr/local/bin/MistProcMKVExec {...}
14:51:33 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
14:51:47 MistOutHLS:drill (443) FAIL: Could not open session semaphore; aborting! (../src/lib/comms.cpp:336)
14:51:47 MistOutHLS:drill (443) FAIL: Logging unclean exit reason: could not connect to session 8d20c1d335f1db98d3a3685922eec32feb35e32bd71cae868ab0b411f8632212 (../src/lib/config.cpp:65)
```

### `pause40` -- (c) `docker pause ingest` 40 s, unpause

Trigger log (host clock, UTC-3):

```
11:55:31 T: docker pause drill-ingest
11:56:11 T: unpaused after 40s
```

Verdict: samples=42 first_unhealthy_t=3 recovered_t=101 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=9 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 11:55:31 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 3 | 11:55:42 | - | - | err | 0 | Stream_is_offline | 2 | 2 | 1 | 0 | 000 | 0 | 0 | 2 |
| 14 | 11:55:53 | - | - | err | 0 | Stream_is_offline | 4 | 0 | 0 | 3 | 000 | 4 | 0 | 2 |
| 25 | 11:56:05 | - | - | err | 0 | Stream_is_offline | 7 | 0 | 0 | 6 | 000 | 7 | 0 | 2 |
| 37 | 11:56:16 | - | - | err | 0 | Stream_is_offline | 10 | 0 | 0 | 9 | 000 | 9 | 0 | 2 |
| 48 | 11:56:24 | 1 | 3 | 1 | 1 | JSON | 11 | 2 | 1 | 11 | 000 | 9 | 0 | 2 |
| 56 | 11:56:31 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 11 | 2 | 1 | 14 | 000 | 9 | 0 | 2 |
| 63 | 11:56:39 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 11 | 2 | 1 | 16 | 000 | 9 | 0 | 2 |
| 71 | 11:56:46 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 11 | 2 | 1 | 18 | 000 | 9 | 0 | 2 |
| 78 | 11:56:54 | 1 | 3 | 1 | 2 | AAC,JSON | 11 | 2 | 1 | 20 | 000 | 9 | 0 | 2 |
| 86 | 11:57:02 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 11 | 2 | 1 | 23 | 000 | 9 | 0 | 2 |
| 94 | 11:57:09 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 9 | 0 | 2 |
| 101 | 11:57:16 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 13 | 200 | 9 | 0 | 2 |
| 108 | 11:57:18 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 110 | 11:57:21 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 113 | 11:57:23 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 144 | 11:57:54 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 175 | 11:58:25 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |
| 180 | 11:58:31 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 9 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
14:55:36 MistInTSSRT:drill (154) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
14:55:36 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
14:55:36 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
14:55:36 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
14:55:36 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:55:37 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:55:47 MistOutHLS:drill (383) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
14:55:47 MistOutHLS:drill (383) FAIL: Logging unclean exit reason: Could not connect to stream (../src/lib/config.cpp:65)
14:55:47 MistInTSSRT:drill (399) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:55:47 MistOutHLS:drill (384) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
14:55:47 MistOutHLS:drill (384) FAIL: Logging unclean exit reason: Could not connect to stream (../src/lib/config.cpp:65)
14:55:47 MistInTSSRT:drill (400) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:55:48 MistInTSSRT:drill (409) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:55:49 MistInTSSRT:drill (387) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:55:49 MistInTSSRT:drill (411) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:55:55 MistInTSSRT:drill (417) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:55:56 MistInTSSRT:drill (387) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:56:00 MistInTSSRT:drill (428) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:56:01 MistInTSSRT:drill (432) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:56:03 MistInTSSRT:drill (387) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:56:07 MistInTSSRT:drill (438) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:56:10 MistInTSSRT:drill (387) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
14:56:11 MistInTSSRT:drill (449) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
14:56:12 MistInTSSRT:drill (387) INFO: Caller SRT socket 760781390 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
14:56:16 MistInBuffer:drill (451) INFO: Started process 464: /usr/local/bin/MistProcMKVExec {...}
14:56:16 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
14:56:41 MistInBuffer:drill (451) INFO: Input closing cleanly with reason: no activity for 30s (> 30s) (../src/src/input/input.cpp:825)
14:56:41 MistInTSSRT:drill (387) INFO: Input closing cleanly with reason: buffer requested shutdown (../src/src/input/input.cpp:825)
14:56:41 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
14:56:43 MistProcMKVExec:drill→drill (464) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
14:56:43 MistInTSSRT:drill (386) WARN: Input for stream drill uncleanly shut down! Cleaning and restarting... (../src/src/input/input.cpp:664)
14:56:44 MistInTSSRT:drill (556) INFO: Caller SRT socket 849566440 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
14:56:47 MistInBuffer:drill (562) INFO: Started process 572: /usr/local/bin/MistProcMKVExec {...}
14:56:48 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `restart-srcpaused` -- (d) pause ffmpeg source, `docker restart ingest`, unpause source 20 s later

Trigger log (host clock, UTC-3):

```
12:00:21 T: docker pause drill-source
12:00:23 T: docker restart drill-ingest
12:00:26 T: restart done, source still paused
12:00:46 T: source unpaused after 20s
```

Verdict: samples=51 first_unhealthy_t=10 recovered_t=85 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=1 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 12:00:21 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 12:00:24 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 12:00:26 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 1 | 200 | 0 | 0 | 0 |
| 7 | 12:00:29 | err | 0 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 10 | 12:00:36 | err | 0 | err | 0 | Stream_is_offline | 0 | 0 | 0 | 0 | 000 | 1 | 0 | 2 |
| 17 | 12:00:44 | err | 0 | err | 0 | Stream_is_waiting_for_data | 4 | 2 | 0 | 3 | 000 | 1 | 0 | 2 |
| 25 | 12:00:52 | err | 0 | err | 0 | Stream_is_waiting_for_data | 5 | 2 | 0 | 5 | 000 | 1 | 0 | 2 |
| 33 | 12:00:59 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 7 | 000 | 1 | 0 | 2 |
| 40 | 12:01:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 9 | 000 | 1 | 0 | 2 |
| 48 | 12:01:14 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 5 | 2 | 1 | 12 | 000 | 1 | 0 | 2 |
| 55 | 12:01:22 | 1 | 3 | err | 0 | Stream_is_offline | 4 | 0 | 0 | 14 | 000 | 1 | 0 | 2 |
| 63 | 12:01:29 | 1 | 3 | 1 | 1 | JSON | 4 | 2 | 1 | 14 | 000 | 1 | 0 | 2 |
| 70 | 12:01:37 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 1 | 0 | 2 |
| 78 | 12:01:44 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 1 | 0 | 2 |
| 85 | 12:01:49 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 200 | 1 | 0 | 2 |
| 90 | 12:01:52 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 93 | 12:01:55 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 96 | 12:01:57 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 126 | 12:02:28 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 157 | 12:02:59 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |
| 180 | 12:03:22 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 1 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
15:00:29 MistInTSSRT:drill (153) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:00:30 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
15:00:30 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
15:00:30 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
15:00:30 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
15:00:31 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
15:00:32 MistInTSSRT:drill (428) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:00:32 MistInTSSRT:drill (423) INFO: Caller SRT socket 618597742 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:00:35 MistInTSSRT:drill (423) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:00:36 MistInBuffer:drill (430) INFO: Input closing cleanly with reason: signal Terminated (15) from process 429 (../src/src/input/input.cpp:825)
15:00:39 MistInTSSRT:drill (441) INFO: Caller SRT socket 1015215430 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:00:42 MistInTSSRT:drill (441) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:00:42 MistInBuffer:drill (446) INFO: Input closing cleanly with reason: signal Terminated (15) from process 445 (../src/src/input/input.cpp:825)
15:00:43 MistInTSSRT:drill (458) INFO: Caller SRT socket 667650613 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:00:48 MistInBuffer:drill (463) INFO: Started process 475: /usr/local/bin/MistProcMKVExec {...}
15:00:49 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
15:01:13 MistInBuffer:drill (463) INFO: Input closing cleanly with reason: no activity for 30s (> 30s) (../src/src/input/input.cpp:825)
15:01:13 MistProcMKVExec:drill→drill (475) INFO: Input closing cleanly with reason: buffer requested shutdown (../src/src/input/input.cpp:825)
15:01:13 MistInTSSRT:drill (458) INFO: Input closing cleanly with reason: buffer requested shutdown (../src/src/input/input.cpp:825)
15:01:13 MistProcMKVExec:drill (475) FAIL: Logging unclean exit reason: set inactive (../src/lib/config.cpp:65)
15:01:14 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
15:01:18 MistInTSSRT:drill (568) INFO: Caller SRT socket 111693125 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:01:21 MistInBuffer:drill (573) INFO: Started process 578: /usr/local/bin/MistProcMKVExec {...}
15:01:22 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `restart-x3` -- (extra) three `docker restart ingest` 30 s apart

Trigger log (host clock, UTC-3):

```
12:05:11 T: docker restart drill-ingest (1/3)
12:05:44 T: docker restart drill-ingest (2/3)
12:06:16 T: docker restart drill-ingest (3/3)
12:06:19 T: restarts done
```

Verdict: samples=46 first_unhealthy_t=10 recovered_t=108 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=0 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 12:05:11 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 1 | 200 | 0 | 0 | 0 |
| 2 | 12:05:14 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 12:05:16 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 7 | 12:05:19 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 1 | 200 | 0 | 0 | 2 |
| 10 | 12:05:26 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 2 | 2 | 0 | 1 | 000 | 0 | 0 | 2 |
| 17 | 12:05:34 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 3 | 000 | 0 | 0 | 2 |
| 25 | 12:05:42 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 5 | 000 | 0 | 0 | 2 |
| 33 | 12:05:49 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 8 | 000 | 0 | 0 | 2 |
| 40 | 12:05:57 | 1 | 3 | err | 0 | Stream_is_offline | 1 | 0 | 0 | 10 | 000 | 0 | 0 | 2 |
| 48 | 12:06:04 | 1 | 3 | 1 | 1 | JSON | 3 | 2 | 1 | 12 | 000 | 0 | 0 | 2 |
| 55 | 12:06:12 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 13 | 000 | 0 | 0 | 2 |
| 63 | 12:06:19 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 2 |
| 70 | 12:06:27 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 4 |
| 78 | 12:06:34 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 3 | 2 | 0 | 13 | 000 | 0 | 0 | 4 |
| 85 | 12:06:42 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 3 | 2 | 1 | 13 | 000 | 0 | 0 | 4 |
| 93 | 12:06:49 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 4 |
| 100 | 12:06:57 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 14 | 000 | 0 | 0 | 4 |
| 108 | 12:06:59 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 4 | 200 | 0 | 0 | 4 |
| 110 | 12:07:02 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 113 | 12:07:05 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 1 | 200 | 0 | 0 | 4 |
| 116 | 12:07:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 147 | 12:07:38 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 178 | 12:08:09 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |
| 180 | 12:08:12 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 4 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
15:05:19 MistInTSSRT:drill (151) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:05:19 MistProcMKVExec:drill (161) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
15:05:19 MistProcMKVExec:drill (161) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
15:05:19 MistInBuffer:drill (156) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
15:05:19 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
15:05:20 MistProcMKVExec:drill→drill (161) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
15:05:21 MistInTSSRT:drill (419) INFO: Caller SRT socket 583877566 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:05:24 MistInBuffer:drill (424) INFO: Started process 435: /usr/local/bin/MistProcMKVExec {...}
15:05:25 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
15:05:50 MistInBuffer:drill (424) INFO: Input closing cleanly with reason: no activity for 30s (> 30s) (../src/src/input/input.cpp:825)
15:05:51 MistProcMKVExec:drill→drill (435) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
15:05:51 MistInTSSRT:drill (419) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:05:51 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
15:05:53 MistInTSSRT:drill (528) INFO: Caller SRT socket 1039716763 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:05:56 MistInBuffer:drill (533) INFO: Started process 537: /usr/local/bin/MistProcMKVExec {...}
15:05:57 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
15:06:24 MistInTSSRT:drill (528) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:06:24 MistProcMKVExec:drill (537) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
15:06:24 MistProcMKVExec:drill (537) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
15:06:24 MistInBuffer:drill (533) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
15:06:24 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
15:06:24 MistProcMKVExec:drill→drill (537) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
15:06:27 MistInTSSRT:drill (631) INFO: Caller SRT socket 469409393 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:06:30 MistInBuffer:drill (636) INFO: Started process 654: /usr/local/bin/MistProcMKVExec {...}
15:06:31 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `kill120` -- (extra) `docker kill ingest`, `compose up -d ingest` after 120 s -- longer than the 10 SRT connect attempts

Trigger log (host clock, UTC-3):

```
12:22:15 T: docker kill drill-ingest
12:24:16 T: compose up -d ingest after 120s
12:24:16 T: ingest up
```

Verdict: samples=27 first_unhealthy_t=7 recovered_t=181 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=53 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 12:22:15 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 12:22:18 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 12:22:20 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 7 | 12:22:28 | - | - | err | 0 | Stream_is_offline | 0 | 0 | 0 | 1 | 000 | 0 | 0 | 2 |
| 15 | 12:22:36 | - | - | err | 0 | Stream_is_offline | 3 | 0 | 0 | 3 | 000 | 4 | 0 | 2 |
| 23 | 12:22:43 | - | - | err | 0 | Stream_is_offline | 6 | 0 | 0 | 5 | 000 | 6 | 0 | 2 |
| 30 | 12:22:51 | - | - | err | 0 | Stream_is_offline | 8 | 0 | 0 | 7 | 000 | 8 | 0 | 2 |
| 38 | 12:22:58 | - | - | err | 0 | Stream_is_offline | 11 | 0 | 0 | 10 | 000 | 11 | 0 | 2 |
| 45 | 12:23:06 | - | - | err | 0 | Stream_is_offline | 13 | 0 | 0 | 12 | 000 | 13 | 0 | 2 |
| 53 | 12:23:13 | - | - | err | 0 | Stream_is_offline | 15 | 0 | 0 | 14 | 000 | 15 | 0 | 2 |
| 60 | 12:23:21 | - | - | err | 0 | Stream_is_offline | 17 | 0 | 0 | 16 | 000 | 17 | 0 | 2 |
| 68 | 12:23:28 | - | - | err | 0 | Stream_is_offline | 20 | 0 | 0 | 19 | 000 | 21 | 0 | 2 |
| 75 | 12:23:36 | - | - | err | 0 | Stream_is_offline | 24 | 0 | 0 | 21 | 000 | 26 | 0 | 2 |
| 83 | 12:23:44 | - | - | err | 0 | Stream_is_offline | 27 | 0 | 0 | 22 | 000 | 30 | 0 | 2 |
| 91 | 12:23:51 | - | - | err | 0 | Stream_is_offline | 31 | 0 | 0 | 24 | 000 | 34 | 0 | 2 |
| 98 | 12:23:59 | - | - | err | 0 | Stream_is_offline | 37 | 0 | 0 | 27 | 000 | 39 | 0 | 2 |
| 106 | 12:24:06 | - | - | err | 0 | Stream_is_offline | 41 | 0 | 0 | 29 | 000 | 44 | 0 | 2 |
| 113 | 12:24:14 | - | - | err | 0 | Stream_is_offline | 45 | 0 | 0 | 31 | 000 | 48 | 0 | 2 |
| 121 | 12:24:21 | - | - | err | 0 | Stream_is_offline | 48 | 0 | 0 | 33 | 000 | 51 | 0 | 2 |
| 128 | 12:24:29 | err | 0 | err | 0 | Stream_is_waiting_for_data | 54 | 2 | 0 | 36 | 000 | 51 | 0 | 2 |
| 136 | 12:24:36 | err | 0 | err | 0 | Stream_is_waiting_for_data | 55 | 2 | 0 | 38 | 000 | 52 | 0 | 2 |
| 143 | 12:24:44 | err | 0 | err | 0 | Stream_is_waiting_for_data | 58 | 2 | 0 | 40 | 000 | 53 | 0 | 2 |
| 151 | 12:24:52 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 60 | 2 | 0 | 42 | 000 | 53 | 0 | 2 |
| 159 | 12:24:59 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 60 | 2 | 1 | 45 | 000 | 53 | 0 | 2 |
| 166 | 12:25:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 60 | 2 | 1 | 47 | 000 | 53 | 0 | 2 |
| 174 | 12:25:14 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 14 | 000 | 53 | 0 | 2 |
| 181 | 12:25:17 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 53 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
15:22:20 MistInTSSRT:drill (153) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:22:21 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
15:22:21 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
15:22:21 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
15:22:21 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
15:22:21 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
15:22:28 MistInTSSRT:drill (418) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:22:31 MistInTSSRT:drill (433) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:22:32 MistOutHLS:drill (405) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:22:32 MistOutHLS:drill (405) FAIL: Logging unclean exit reason: Could not connect to stream (../src/lib/config.cpp:65)
15:22:32 MistInTSSRT:drill (434) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:22:33 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:22:34 MistInTSSRT:drill (436) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
15:22:40 MistInTSSRT:drill (447) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:22:44 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:22:46 MistInTSSRT:drill (455) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 2 more 'A pull process for this stream is already running' lines ...
15:22:53 MistInTSSRT:drill (468) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:22:55 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:22:58 MistInTSSRT:drill (470) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
15:23:04 MistInTSSRT:drill (487) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:06 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:23:08 MistInTSSRT:drill (496) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 2 more 'A pull process for this stream is already running' lines ...
15:23:16 MistInTSSRT:drill (509) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:17 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:23:22 MistInTSSRT:drill (511) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:23 MistInTSSRT:drill (520) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:24 MistOutHLS:drill (413) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:24 MistOutHLS:drill (413) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:24 MistOutHLS:drill (413) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:24 MistInTSSRT:drill (521) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:28 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:23:28 MistInTSSRT:drill (523) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:29 MistOutHLS:drill (417) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:29 MistOutHLS:drill (417) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:29 MistOutHLS:drill (417) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:29 MistInTSSRT:drill (530) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:31 MistInTSSRT:drill (539) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:31 MistOutHLS:drill (432) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:31 MistOutHLS:drill (432) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:31 MistOutHLS:drill (432) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:31 MistInTSSRT:drill (540) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:33 MistOutHLS:drill (405) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:33 MistOutHLS:drill (405) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:34 MistInTSSRT:drill (542) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:35 MistOutHLS:drill (435) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:35 MistOutHLS:drill (435) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:35 MistOutHLS:drill (435) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:35 MistInTSSRT:drill (543) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:39 MistInTSSRT:drill (552) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:39 MistOutHLS:drill (444) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:39 MistOutHLS:drill (444) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:39 MistOutHLS:drill (444) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:39 MistInTSSRT:drill (553) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:39 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:23:40 MistInTSSRT:drill (555) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:41 MistOutHLS:drill (446) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:41 MistOutHLS:drill (446) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:41 MistOutHLS:drill (446) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:41 MistInTSSRT:drill (556) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
15:23:46 MistInTSSRT:drill (567) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:46 MistOutHLS:drill (454) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:46 MistOutHLS:drill (454) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:46 MistOutHLS:drill (454) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:46 MistInTSSRT:drill (568) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:47 MistOutHLS:drill (456) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:47 MistOutHLS:drill (456) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:47 MistOutHLS:drill (456) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:47 MistInTSSRT:drill (569) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:50 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:23:52 MistInTSSRT:drill (571) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:53 MistOutHLS:drill (458) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:53 MistOutHLS:drill (458) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:53 MistOutHLS:drill (458) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:53 MistInTSSRT:drill (572) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:54 MistInTSSRT:drill (581) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:54 MistOutHLS:drill (467) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:54 MistOutHLS:drill (467) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:54 MistOutHLS:drill (467) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:54 MistInTSSRT:drill (582) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:58 MistInTSSRT:drill (584) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:23:59 MistOutHLS:drill (469) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:23:59 MistOutHLS:drill (469) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:23:59 MistOutHLS:drill (469) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:23:59 MistInTSSRT:drill (591) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:01 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:24:01 MistInTSSRT:drill (600) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:02 MistOutHLS:drill (484) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:24:02 MistOutHLS:drill (484) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:24:02 MistOutHLS:drill (484) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:24:02 MistInTSSRT:drill (601) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:04 MistInTSSRT:drill (603) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:05 MistOutHLS:drill (486) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:24:05 MistOutHLS:drill (486) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:24:05 MistOutHLS:drill (486) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:24:05 MistInTSSRT:drill (604) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:09 MistInTSSRT:drill (613) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:09 MistOutHLS:drill (495) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:24:09 MistOutHLS:drill (495) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:24:09 MistOutHLS:drill (495) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:24:09 MistInTSSRT:drill (614) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:10 MistInTSSRT:drill (616) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:11 MistOutHLS:drill (497) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:24:11 MistOutHLS:drill (497) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:24:11 MistOutHLS:drill (497) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:24:11 MistInTSSRT:drill (617) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:12 MistInTSSRT:drill (415) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:24:13 MistInTSSRT:drill (415) WARN: Connecting to srt://ingest:8889?streamid=live+drill timed out (../src/src/input/input_tssrt.cpp:261)
15:24:13 MistInTSSRT:drill (415) FAIL: Logging unclean exit reason: Could not connect to SRT source srt://ingest:8889?streamid=live+drill (../src/lib/config.cpp:65)
15:24:13 MistInTSSRT:drill (415) WARN: Input closing unclean, reason: Could not connect to SRT source srt://ingest:8889?streamid=live+drill (../src/src/input/input.cpp:828)
15:24:16 MistInTSSRT:drill (632) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:17 MistOutHLS:drill (506) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:24:17 MistOutHLS:drill (506) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:24:17 MistOutHLS:drill (506) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:24:17 MistInTSSRT:drill (633) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:17 MistOutHLS:drill (508) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:24:17 MistOutHLS:drill (508) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:24:17 MistOutHLS:drill (508) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:24:17 MistInTSSRT:drill (634) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:18 MistInTSSRT:drill (627) INFO: Caller SRT socket 694954612 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:24:21 MistInTSSRT:drill (627) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:24:22 MistInBuffer:drill (636) INFO: Input closing cleanly with reason: signal Terminated (15) from process 635 (../src/src/input/input.cpp:825)
15:24:23 MistInTSSRT:drill (674) INFO: Caller SRT socket 330999269 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:24:27 MistInTSSRT:drill (674) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:24:27 MistInBuffer:drill (679) INFO: Input closing cleanly with reason: signal Terminated (15) from process 678 (../src/src/input/input.cpp:825)
15:24:28 MistInTSSRT:drill (690) INFO: Caller SRT socket 171690021 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:24:31 MistInTSSRT:drill (690) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:24:32 MistInBuffer:drill (695) INFO: Input closing cleanly with reason: signal Terminated (15) from process 694 (../src/src/input/input.cpp:825)
15:24:32 MistInTSSRT:drill (717) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:33 MistInTSSRT:drill (713) INFO: Caller SRT socket 912827213 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:24:36 MistInTSSRT:drill (713) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:24:37 MistInBuffer:drill (719) INFO: Input closing cleanly with reason: signal Terminated (15) from process 718 (../src/src/input/input.cpp:825)
15:24:38 MistInTSSRT:drill (724) INFO: Caller SRT socket 1017401737 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:24:41 MistInTSSRT:drill (724) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:24:42 MistInBuffer:drill (729) INFO: Input closing cleanly with reason: signal Terminated (15) from process 728 (../src/src/input/input.cpp:825)
15:24:42 MistInTSSRT:drill (745) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:24:43 MistInTSSRT:drill (741) INFO: Caller SRT socket 679519042 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:24:47 MistInBuffer:drill (747) INFO: Started process 760: /usr/local/bin/MistProcMKVExec {...}
15:24:48 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `srcpause40` -- NMW-2106 silence case: `docker pause` the ffmpeg source (ingest untouched) 40 s, unpause

Trigger log (host clock, UTC-3):

```
12:17:23 T: docker pause drill-source
12:18:03 T: source unpaused after 40s
```

Verdict: samples=55 first_unhealthy_t=13 recovered_t=73 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=4 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 12:17:23 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 3 | 12:17:26 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 6 | 12:17:28 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 8 | 12:17:31 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 11 | 12:17:33 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 13 | 12:17:41 | 1 | 3 | err | 0 | Stream_is_offline | 0 | 0 | 0 | 1 | 000 | 0 | 0 | 2 |
| 21 | 12:17:48 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 2 | 2 | 0 | 3 | 000 | 2 | 0 | 2 |
| 28 | 12:17:56 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 5 | 2 | 0 | 5 | 000 | 3 | 0 | 2 |
| 36 | 12:18:03 | err | 0 | err | 0 | Stream_is_waiting_for_data | 7 | 2 | 0 | 7 | 000 | 4 | 0 | 2 |
| 43 | 12:18:11 | 1 | 3 | err | 0 | Stream_is_offline | 10 | 2 | 0 | 10 | 000 | 4 | 0 | 2 |
| 51 | 12:18:18 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 10 | 2 | 1 | 12 | 000 | 4 | 0 | 2 |
| 58 | 12:18:26 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 10 | 2 | 1 | 14 | 000 | 4 | 0 | 2 |
| 66 | 12:18:33 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 9 | 2 | 1 | 14 | 000 | 4 | 0 | 2 |
| 73 | 12:18:37 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 6 | 2 | 1 | 14 | 200 | 4 | 0 | 2 |
| 77 | 12:18:39 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 79 | 12:18:42 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 82 | 12:18:45 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 113 | 12:19:15 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 144 | 12:19:47 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 175 | 12:20:18 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 180 | 12:20:23 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
15:17:33 MistInTSSRT:drill (153) WARN: Stream has had no live track for 10010 ms (limit 10000 ms) - closing SRT connection (../src/src/input/input_tssrt.cpp:290)
15:17:33 MistInTSSRT:drill (153) INFO: Logging clean exit reason: SRT stream had no live track for 10010 ms (../src/lib/config.cpp:63)
15:17:33 MistInTSSRT:drill (153) INFO: Input closing cleanly with reason: SRT stream had no live track for 10010 ms (../src/src/input/input.cpp:825)
15:17:33 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
15:17:33 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
15:17:34 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
15:17:34 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
15:17:34 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
15:17:37 MistInTSSRT:drill (444) INFO: Caller SRT socket 209254711 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:17:47 MistInTSSRT:drill (444) WARN: No usable data received over SRT for 10002 ms (limit 10000 ms) - closing connection (../src/src/input/input_tssrt.cpp:276)
15:17:47 MistInTSSRT:drill (444) INFO: Logging clean exit reason: SRT connection idle for 10002 ms (../src/lib/config.cpp:63)
15:17:47 MistInTSSRT:drill (444) INFO: Input closing cleanly with reason: SRT connection idle for 10002 ms (../src/src/input/input.cpp:825)
15:17:48 MistInBuffer:drill (455) INFO: Input closing cleanly with reason: signal Terminated (15) from process 454 (../src/src/input/input.cpp:825)
15:17:48 MistInTSSRT:drill (470) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:17:48 MistInTSSRT:drill (474) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:17:49 MistInTSSRT:drill (469) INFO: Caller SRT socket 104273022 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:17:53 MistInTSSRT:drill (469) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:17:54 MistInBuffer:drill (476) INFO: Input closing cleanly with reason: signal Terminated (15) from process 475 (../src/src/input/input.cpp:825)
15:17:54 MistInTSSRT:drill (494) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:17:55 MistInTSSRT:drill (490) INFO: Caller SRT socket 465802117 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:17:58 MistInTSSRT:drill (490) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:17:59 MistInBuffer:drill (496) INFO: Input closing cleanly with reason: signal Terminated (15) from process 495 (../src/src/input/input.cpp:825)
15:17:59 MistInTSSRT:drill (513) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:18:00 MistInTSSRT:drill (509) INFO: Caller SRT socket 908912209 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:18:03 MistInTSSRT:drill (509) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:18:04 MistInBuffer:drill (515) INFO: Input closing cleanly with reason: signal Terminated (15) from process 514 (../src/src/input/input.cpp:825)
15:18:06 MistInTSSRT:drill (520) INFO: Caller SRT socket 538580958 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:18:09 MistInBuffer:drill (526) INFO: Started process 542: /usr/local/bin/MistProcMKVExec {...}
15:18:10 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `none` -- steady state, 300 s, no trigger

Trigger log (host clock, UTC-3):

```
12:10:29 T: no trigger (control run)
```

Verdict: samples=118 first_unhealthy_t=None recovered_t=None longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=0 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 12:10:29 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 31 | 12:11:01 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 62 | 12:11:31 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 92 | 12:12:02 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 123 | 12:12:33 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 154 | 12:13:04 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 185 | 12:13:35 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 216 | 12:14:05 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 247 | 12:14:36 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 277 | 12:15:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 1 | 200 | 0 | 0 | 0 |
| 301 | 12:15:30 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
```

### `srcpause40` -- CONTROL on nedmedia-v2: NMW-2106 silence case

Trigger log (host clock, UTC-3):

```
12:27:05 T: docker pause drill-source
12:27:45 T: source unpaused after 40s
```

Verdict: samples=55 first_unhealthy_t=13 recovered_t=73 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=4 invalid_track=0 outhls=0 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 12:27:05 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 3 | 12:27:07 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 12:27:10 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 8 | 12:27:13 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 11 | 12:27:15 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 13 | 12:27:23 | 1 | 3 | err | 0 | Stream_is_offline | 1 | 0 | 0 | 1 | 000 | 0 | 0 | 2 |
| 21 | 12:27:30 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 2 | 2 | 0 | 3 | 000 | 2 | 0 | 2 |
| 28 | 12:27:38 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 5 | 2 | 0 | 5 | 000 | 3 | 0 | 2 |
| 36 | 12:27:45 | err | 0 | err | 0 | Stream_is_waiting_for_data | 7 | 2 | 0 | 8 | 000 | 4 | 0 | 2 |
| 43 | 12:27:53 | 1 | 3 | err | 0 | Stream_is_waiting_for_data | 10 | 2 | 0 | 10 | 000 | 4 | 0 | 2 |
| 51 | 12:28:00 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 10 | 2 | 1 | 12 | 000 | 4 | 0 | 2 |
| 58 | 12:28:08 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 10 | 2 | 1 | 14 | 000 | 4 | 0 | 2 |
| 66 | 12:28:15 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 9 | 2 | 1 | 15 | 000 | 4 | 0 | 2 |
| 73 | 12:28:19 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 6 | 2 | 1 | 14 | 200 | 4 | 0 | 2 |
| 77 | 12:28:21 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 79 | 12:28:24 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 82 | 12:28:27 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 114 | 12:28:58 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 145 | 12:29:30 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 176 | 12:30:00 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |
| 181 | 12:30:05 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 4 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
15:27:15 MistInTSSRT:drill (151) WARN: No data received over SRT for 10007 ms (limit 10000 ms) - closing connection (../src/src/input/input_tssrt.cpp:289)
15:27:15 MistInTSSRT:drill (151) INFO: Logging clean exit reason: SRT connection idle for 10007 ms (../src/lib/config.cpp:63)
15:27:15 MistInTSSRT:drill (151) INFO: Input closing cleanly with reason: SRT connection idle for 10007 ms (../src/src/input/input.cpp:825)
15:27:15 MistProcMKVExec:drill (161) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
15:27:15 MistProcMKVExec:drill (161) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
15:27:15 MistInBuffer:drill (156) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
15:27:16 MistProcMKVExec:drill→drill (161) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
15:27:16 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
15:27:19 MistInTSSRT:drill (434) INFO: Caller SRT socket 445405788 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:27:29 MistInTSSRT:drill (434) WARN: No data received over SRT for 10010 ms (limit 10000 ms) - closing connection (../src/src/input/input_tssrt.cpp:289)
15:27:29 MistInTSSRT:drill (434) INFO: Logging clean exit reason: SRT connection idle for 10010 ms (../src/lib/config.cpp:63)
15:27:29 MistInTSSRT:drill (434) INFO: Input closing cleanly with reason: SRT connection idle for 10010 ms (../src/src/input/input.cpp:825)
15:27:29 MistInBuffer:drill (439) INFO: Input closing cleanly with reason: signal Terminated (15) from process 438 (../src/src/input/input.cpp:825)
15:27:29 MistInTSSRT:drill (463) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:27:29 MistInTSSRT:drill (464) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:27:30 MistInTSSRT:drill (459) INFO: Caller SRT socket 458272039 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:27:35 MistInTSSRT:drill (459) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:27:36 MistInBuffer:drill (466) INFO: Input closing cleanly with reason: signal Terminated (15) from process 465 (../src/src/input/input.cpp:825)
15:27:36 MistInTSSRT:drill (480) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:27:37 MistInTSSRT:drill (481) INFO: Caller SRT socket 315193343 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:27:40 MistInTSSRT:drill (481) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:27:41 MistInBuffer:drill (486) INFO: Input closing cleanly with reason: signal Terminated (15) from process 485 (../src/src/input/input.cpp:825)
15:27:41 MistInTSSRT:drill (503) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:27:42 MistInTSSRT:drill (499) INFO: Caller SRT socket 602476564 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:27:45 MistInTSSRT:drill (499) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:27:46 MistInBuffer:drill (505) INFO: Input closing cleanly with reason: signal Terminated (15) from process 504 (../src/src/input/input.cpp:825)
15:27:47 MistInTSSRT:drill (510) INFO: Caller SRT socket 478168523 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:27:51 MistInBuffer:drill (515) INFO: Started process 526: /usr/local/bin/MistProcMKVExec {...}
15:27:51 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

### `kill120` -- CONTROL on nedmedia-v2: kill120

Trigger log (host clock, UTC-3):

```
12:31:58 T: docker kill drill-ingest
12:33:59 T: compose up -d ingest after 120s
12:33:59 T: ingest up
```

Verdict: samples=27 first_unhealthy_t=7 recovered_t=181 longest_live0tracks_with_puller=0s(from t=None) end: tx_live=1 tx_tracks=3 hls=200 already_running=53 invalid_track=0 outhls=15 / WEDGE: no

Samples (every ~2.5 s; rows after recovery thinned):

| t | clock | in_live | in_tracks | tx_live | tx_tracks | tx_codecs | tssrt | buffer | mkvexec | outhls | hls | already_running | invalid_track | dropping |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 12:31:58 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 2 | 12:32:01 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 5 | 12:32:03 | - | - | 1 | 3 | AAC,H264240,JSON | 2 | 2 | 1 | 0 | 200 | 0 | 0 | 0 |
| 7 | 12:32:11 | - | - | err | 0 | Stream_is_offline | 0 | 0 | 0 | 1 | 000 | 1 | 0 | 2 |
| 15 | 12:32:18 | - | - | err | 0 | Stream_is_offline | 3 | 0 | 0 | 3 | 000 | 4 | 0 | 2 |
| 22 | 12:32:26 | - | - | err | 0 | Stream_is_offline | 6 | 0 | 0 | 5 | 000 | 6 | 0 | 2 |
| 30 | 12:32:34 | - | - | err | 0 | Stream_is_offline | 8 | 0 | 0 | 7 | 000 | 8 | 0 | 2 |
| 38 | 12:32:41 | - | - | err | 0 | Stream_is_offline | 11 | 0 | 0 | 10 | 000 | 11 | 0 | 2 |
| 45 | 12:32:49 | - | - | err | 0 | Stream_is_offline | 13 | 0 | 0 | 12 | 000 | 13 | 0 | 2 |
| 53 | 12:32:56 | - | - | err | 0 | Stream_is_offline | 15 | 0 | 0 | 14 | 000 | 15 | 0 | 2 |
| 60 | 12:33:04 | - | - | err | 0 | Stream_is_offline | 17 | 0 | 0 | 16 | 000 | 17 | 0 | 2 |
| 68 | 12:33:11 | - | - | err | 0 | Stream_is_offline | 20 | 0 | 0 | 19 | 000 | 21 | 0 | 2 |
| 75 | 12:33:19 | - | - | err | 0 | Stream_is_offline | 24 | 0 | 0 | 21 | 000 | 26 | 0 | 2 |
| 83 | 12:33:26 | - | - | err | 0 | Stream_is_offline | 27 | 0 | 0 | 22 | 000 | 30 | 0 | 2 |
| 90 | 12:33:34 | - | - | err | 0 | Stream_is_offline | 31 | 0 | 0 | 24 | 000 | 34 | 0 | 2 |
| 98 | 12:33:41 | - | - | err | 0 | Stream_is_offline | 37 | 0 | 0 | 27 | 000 | 39 | 0 | 2 |
| 105 | 12:33:49 | - | - | err | 0 | Stream_is_offline | 41 | 0 | 0 | 29 | 000 | 44 | 0 | 2 |
| 113 | 12:33:56 | - | - | err | 0 | Stream_is_offline | 45 | 0 | 0 | 31 | 000 | 48 | 0 | 2 |
| 120 | 12:34:04 | - | - | err | 0 | Stream_is_offline | 48 | 0 | 0 | 33 | 000 | 49 | 0 | 2 |
| 128 | 12:34:12 | err | 0 | err | 0 | Stream_is_waiting_for_data | 51 | 2 | 0 | 36 | 000 | 50 | 0 | 2 |
| 136 | 12:34:19 | err | 0 | err | 0 | Stream_is_offline | 54 | 2 | 0 | 38 | 000 | 51 | 0 | 2 |
| 143 | 12:34:27 | err | 0 | err | 0 | Stream_is_waiting_for_data | 55 | 2 | 0 | 39 | 000 | 52 | 0 | 2 |
| 151 | 12:34:34 | err | 0 | err | 0 | Stream_is_offline | 56 | 0 | 0 | 41 | 000 | 53 | 0 | 2 |
| 158 | 12:34:42 | 1 | 3 | 1 | 1 | JSON | 59 | 2 | 1 | 44 | 000 | 53 | 0 | 2 |
| 166 | 12:34:49 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 12 | 2 | 1 | 14 | 000 | 53 | 0 | 2 |
| 173 | 12:34:57 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 10 | 2 | 1 | 14 | 000 | 53 | 0 | 2 |
| 181 | 12:35:01 | 1 | 3 | 1 | 3 | AAC,H264240,JSON | 8 | 2 | 1 | 15 | 200 | 53 | 0 | 2 |

Transcode container log in the window (container clock is UTC; noise lines dropped):

```
15:32:03 MistInTSSRT:drill (153) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:32:04 MistProcMKVExec:drill (165) WARN: Dropping invalid track 2: disappeared from metadata (../src/src/output/output.cpp:2295)
15:32:04 MistProcMKVExec:drill (165) WARN: Dropping invalid track 1: disappeared from metadata (../src/src/output/output.cpp:2295)
15:32:04 MistInBuffer:drill (161) INFO: Input closing cleanly with reason: source disconnected for non-resumable stream (../src/src/input/input.cpp:825)
15:32:04 MistController (8) INFO: Stream drill became inactive (../src/src/controller/controller_statistics.cpp:202)
15:32:04 MistProcMKVExec:drill→drill (165) INFO: Input closing cleanly with reason: no more data (../src/src/input/input.cpp:825)
15:32:11 MistInTSSRT:drill (417) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:32:13 MistInTSSRT:drill (433) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:32:15 MistOutHLS:drill (404) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:32:15 MistOutHLS:drill (404) FAIL: Logging unclean exit reason: Could not connect to stream (../src/lib/config.cpp:65)
15:32:15 MistInTSSRT:drill (434) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:32:16 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:32:17 MistInTSSRT:drill (436) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
15:32:23 MistInTSSRT:drill (447) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:32:27 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:32:29 MistInTSSRT:drill (456) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 2 more 'A pull process for this stream is already running' lines ...
15:32:36 MistInTSSRT:drill (469) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:32:38 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:32:41 MistInTSSRT:drill (471) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
15:32:47 MistInTSSRT:drill (488) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:32:49 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:32:51 MistInTSSRT:drill (497) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 2 more 'A pull process for this stream is already running' lines ...
15:32:59 MistInTSSRT:drill (510) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:00 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:33:05 MistInTSSRT:drill (512) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:06 MistInTSSRT:drill (521) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:07 MistOutHLS:drill (412) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:07 MistOutHLS:drill (412) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:07 MistOutHLS:drill (412) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:07 MistInTSSRT:drill (522) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:11 MistInTSSRT:drill (524) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:11 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:33:11 MistOutHLS:drill (416) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:11 MistOutHLS:drill (416) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:11 MistOutHLS:drill (416) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:11 MistInTSSRT:drill (525) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:14 MistInTSSRT:drill (540) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:14 MistOutHLS:drill (432) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:14 MistOutHLS:drill (432) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:14 MistOutHLS:drill (432) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:14 MistInTSSRT:drill (541) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:15 MistOutHLS:drill (404) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:15 MistOutHLS:drill (404) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:17 MistInTSSRT:drill (543) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:17 MistOutHLS:drill (435) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:17 MistOutHLS:drill (435) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:17 MistOutHLS:drill (435) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:17 MistInTSSRT:drill (544) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:21 MistInTSSRT:drill (553) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:22 MistOutHLS:drill (444) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:22 MistOutHLS:drill (444) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:22 MistOutHLS:drill (444) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:22 MistInTSSRT:drill (554) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:22 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:33:23 MistInTSSRT:drill (556) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:23 MistOutHLS:drill (446) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:23 MistOutHLS:drill (446) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:23 MistOutHLS:drill (446) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:23 MistInTSSRT:drill (557) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
         ... 1 more 'A pull process for this stream is already running' lines ...
15:33:29 MistInTSSRT:drill (568) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:29 MistOutHLS:drill (455) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:29 MistOutHLS:drill (455) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:29 MistOutHLS:drill (455) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:29 MistInTSSRT:drill (569) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:30 MistOutHLS:drill (457) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:30 MistOutHLS:drill (457) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:30 MistOutHLS:drill (457) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:30 MistInTSSRT:drill (570) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:33 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:33:35 MistInTSSRT:drill (572) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:36 MistOutHLS:drill (459) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:36 MistOutHLS:drill (459) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:36 MistOutHLS:drill (459) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:36 MistInTSSRT:drill (573) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:36 MistInTSSRT:drill (582) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:37 MistOutHLS:drill (468) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:37 MistOutHLS:drill (468) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:37 MistOutHLS:drill (468) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:37 MistInTSSRT:drill (583) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:41 MistInTSSRT:drill (585) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:42 MistOutHLS:drill (470) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:42 MistOutHLS:drill (470) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:42 MistOutHLS:drill (470) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:42 MistInTSSRT:drill (586) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:44 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:33:44 MistInTSSRT:drill (601) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:45 MistOutHLS:drill (485) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:45 MistOutHLS:drill (485) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:45 MistOutHLS:drill (485) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:45 MistInTSSRT:drill (602) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:47 MistInTSSRT:drill (604) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:48 MistOutHLS:drill (487) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:48 MistOutHLS:drill (487) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:48 MistOutHLS:drill (487) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:48 MistInTSSRT:drill (605) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:51 MistInTSSRT:drill (614) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:52 MistOutHLS:drill (496) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:52 MistOutHLS:drill (496) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:52 MistOutHLS:drill (496) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:52 MistInTSSRT:drill (615) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:53 MistInTSSRT:drill (617) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:54 MistOutHLS:drill (498) FAIL: Stream open failed (../src/src/output/output.cpp:199)
15:33:54 MistOutHLS:drill (498) FAIL: Logging unclean exit reason: Stream open failed (../src/lib/config.cpp:65)
15:33:54 MistOutHLS:drill (498) FAIL: Could not connect to stream (../src/src/output/output.cpp:199)
15:33:54 MistInTSSRT:drill (618) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:33:55 MistInTSSRT:drill (414) ERROR: Can't connect SRT socket to any address for ingest (../src/lib/socket_srt.cpp:476)
15:33:56 MistInTSSRT:drill (414) WARN: Connecting to srt://ingest:8889?streamid=live+drill timed out (../src/src/input/input_tssrt.cpp:249)
15:33:56 MistInTSSRT:drill (414) INFO: Input closing without a set exit reason (../src/src/input/input.cpp:823)
15:33:57 MistInBuffer:drill (620) INFO: Input closing cleanly with reason: signal Terminated (15) from process 619 (../src/src/input/input.cpp:825)
15:33:59 MistInTSSRT:drill (668) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:34:04 MistInTSSRT:drill (663) INFO: Caller SRT socket 724932408 connected targetting ingest:8889 -> 172.22.0.2:8889 (../src/lib/socket_srt.cpp:469)
15:34:07 MistInTSSRT:drill (663) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:34:08 MistInBuffer:drill (672) INFO: Input closing cleanly with reason: signal Terminated (15) from process 671 (../src/src/input/input.cpp:825)
15:34:08 MistInTSSRT:drill (689) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:34:09 MistInTSSRT:drill (685) INFO: Caller SRT socket 288205650 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:34:12 MistInTSSRT:drill (685) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:34:13 MistInBuffer:drill (691) INFO: Input closing cleanly with reason: signal Terminated (15) from process 690 (../src/src/input/input.cpp:825)
15:34:14 MistInTSSRT:drill (702) INFO: Caller SRT socket 677072249 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:34:17 MistInTSSRT:drill (702) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:34:18 MistInBuffer:drill (708) INFO: Input closing cleanly with reason: signal Terminated (15) from process 707 (../src/src/input/input.cpp:825)
15:34:18 MistInTSSRT:drill (721) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:34:19 MistInTSSRT:drill (719) INFO: Caller SRT socket 264941088 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:34:22 MistInTSSRT:drill (719) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:34:23 MistInBuffer:drill (725) INFO: Input closing cleanly with reason: signal Terminated (15) from process 724 (../src/src/input/input.cpp:825)
15:34:23 MistInTSSRT:drill (742) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:34:24 MistInTSSRT:drill (737) INFO: Caller SRT socket 513001101 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:34:27 MistInTSSRT:drill (737) INFO: Input closing cleanly with reason: SRT connection close (../src/src/input/input.cpp:825)
15:34:28 MistInBuffer:drill (744) INFO: Input closing cleanly with reason: signal Terminated (15) from process 743 (../src/src/input/input.cpp:825)
15:34:29 MistInTSSRT:drill (757) WARN: A pull process for this stream is already running (../src/src/input/input.cpp:504)
15:34:30 MistInTSSRT:drill (758) INFO: Caller SRT socket 454247511 connected targetting ingest:8889 -> [::ffff:172.22.0.2]:8889 (../src/lib/socket_srt.cpp:469)
15:34:34 MistInBuffer:drill (763) INFO: Started process 767: /usr/local/bin/MistProcMKVExec {...}
15:34:34 MistController (8) INFO: Stream drill became active (../src/src/controller/controller_statistics.cpp:178)
```

