# srt-restart-drill — ingest-restart SRT pull wedge reproduction (NMW-3124 / NMW-3126)

Local docker reproduction of the production topology in which a transcode box pulls a
stream from an ingest box over SRT (`MistInTSSRT`) and the ingest is restarted underneath it.

```
source (ffmpeg, RTMP push, restart: always)
   └─> ingest     stream live+drill   source push://      TSSRT listener :8889   (nedmedia/mistserver:nedmedia-v2)
          └─> transcode  stream drill   source srt://ingest:8889?streamid=live+drill
                         + one MistProcMKVExec rung (ffmpeg libx264 240p), production field shape
                         HLS served on :8080  (host 127.0.0.1:28080)
```

* `transcode` and `source` run `nedmedia/mistserver:nedmedia-v2-drill`, which is
  `nedmedia-v2` plus `apk add ffmpeg curl` (`Dockerfile.drill`). The Mist binaries are the
  production ones; ffmpeg is only there for the MKVExec rung and the synthetic encoder.
* Configs are in `config/` and are copied into `run/<svc>/config.json` at every stack start
  (Mist rewrites its config on disk; the repo copies stay pristine). `run/` is gitignored.
* Host ports: ingest HTTP 18080 / API 14242, transcode HTTP 28080 / API 24242 (loopback only).

## Run

```sh
docker build -f Dockerfile.drill -t nedmedia/mistserver:nedmedia-v2-drill .
./drill.sh all                       # every trigger, fresh stack each, then compose down -v
./drill.sh trigger kill20            # one trigger, stack left up for poking
./drill.sh up | status | down
./drill.sh list
```

Triggers (`drill.sh list`): `restart` (docker restart ingest), `kill5|kill20|kill60`
(docker kill, compose up -d after N s), `pause15|pause40` (docker pause/unpause),
`restart-srcpaused` (pause the ffmpeg source, restart ingest, unpause the source 20 s later),
`restart-x3` (three restarts 30 s apart, so the later ones land inside the previous recovery),
`none` (control). Run by name only: `kill120` (longer than the puller's 10 SRT connect attempts),
`srcpause40` (the NMW-2106 case: pause the ffmpeg source 40 s, ingest untouched). Env knobs: `SAMPLE_EVERY`
(2 s), `SAMPLE_FOR` (180 s), `STEADY_S` (30 s of verified-healthy before the trigger fires), `INGEST_IMAGE`,
`TRANSCODE_IMAGE`, `RESULTS_DIR` (default `run/results`; use one per image so baselines are kept, e.g.
`TRANSCODE_IMAGE=nedmedia/mistserver:nmw-3124-dev-drill RESULTS_DIR=$PWD/run/results-nmw-3124-dev ./drill.sh all`).

Each trigger starts from a freshly started stack verified healthy with the HLS demand loop
running (index + variant + newest segment, once a second), then samples into
`run/results/<trigger>/`:

* `samples.tsv` — one line per ~2.5 s: ingest `json_live+drill.js` live/tracks, transcode
  `json_drill.js` live/tracks/codecs, process counts in the transcode container
  (`MistInTSSRT`, `MistInBuffer`, `MistProcMKVExec`, `MistOutHLS`), HLS index HTTP status,
  and cumulative counts in the transcode log of `already running`, `Requested invalid track`,
  `Dropping invalid track`.
* `transcode.log`, `ingest.log`, `transcode.ps`, `trigger.log`, `verdict.txt`.

Wedge criterion (from the incident): puller alive, `json_drill.js` live with 0 tracks for
more than 60 s, and `A pull process for this stream is already running` repeating.

Results of the runs on this host: `RESULTS.md` (nedmedia-v2 baseline from NMW-3126, then the
`nmw-3124-dev` verification from NMW-3130).
