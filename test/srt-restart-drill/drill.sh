#!/usr/bin/env bash
# NMW-3126 -- ingest-restart SRT pull wedge drill.
#
#   ./drill.sh all                 fresh stack per trigger, every trigger, then compose down -v
#   ./drill.sh trigger <name>      fresh stack, one trigger, stack left up
#   ./drill.sh up | down | status  manage the stack by hand
#   ./drill.sh list                trigger names
#
# Every trigger starts from a freshly started stack that has been verified healthy (transcode
# json live with tracks, HLS 200) for STEADY_S seconds with the HLS request loop running,
# then samples every SAMPLE_EVERY s for SAMPLE_FOR s into run/results/<trigger>/samples.tsv
# and keeps the transcode + ingest container logs for the window.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
RUN="$HERE/run"
RES="${RESULTS_DIR:-$RUN/results}"   # RESULTS_DIR=run/results-<image> keeps baselines apart
TX_HTTP=http://127.0.0.1:28080
IN_HTTP=http://127.0.0.1:18080
SAMPLE_EVERY=${SAMPLE_EVERY:-2}
SAMPLE_FOR=${SAMPLE_FOR:-180}
STEADY_S=${STEADY_S:-30}
HEALTHY_TIMEOUT=${HEALTHY_TIMEOUT:-150}
TRIGGERS="restart kill5 kill20 kill60 pause15 pause40 restart-srcpaused restart-x3"
# Not in TRIGGERS (run by name): srcpause40 = NMW-2106 silence case, none = steady-state control,
# kill120 = outage longer than the 10 SRT connect attempts (exercises the preRun connect-failure path)

log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*"; }

# ---------- stack ----------
stage_configs() {
  mkdir -p "$RUN/ingest" "$RUN/transcode" "$RES"
  cp config/ingest.json "$RUN/ingest/config.json"
  cp config/transcode.json "$RUN/transcode/config.json"
}
stack_up() {
  stage_configs
  docker compose down -v -t 2 >/dev/null 2>&1 || true
  docker compose up -d
}
stack_down() {
  hls_stop
  docker compose down -v -t 2
}

# ---------- probes ----------
# json_<stream>.js -> "live tracks codecs" ("- - -" when unreachable / error)
json_probe() { # $1 base url, $2 stream
  curl -s -m 4 "$1/json_$2.js" | python3 -I "$HERE/jsonprobe.py" 2>/dev/null || echo "- - -"
}
tx_procs() { # counts: MistInTSSRT MistInBuffer MistProcMKVExec MistOutHLS
  local ps
  ps=$(docker exec drill-transcode ps -o args 2>/dev/null) || { echo "- - - -"; return; }
  printf '%s %s %s %s\n' \
    "$(printf '%s\n' "$ps" | grep -c 'MistInTSSRT')" \
    "$(printf '%s\n' "$ps" | grep -c 'MistInBuffer')" \
    "$(printf '%s\n' "$ps" | grep -c 'MistProcMKVExec')" \
    "$(printf '%s\n' "$ps" | grep -c 'MistOutHLS')"
}
hls_probe() { # "code segments"
  local body code
  body=$(curl -s -m 5 -w '\n%{http_code}' "$TX_HTTP/hls/drill/index.m3u8" 2>/dev/null)
  code=$(printf '%s' "$body" | tail -n1)
  # count media playlist / segment lines (master lists variants; count both for a size signal)
  printf '%s %s\n' "${code:-000}" "$(printf '%s' "$body" | grep -c -E '\.(m3u8|ts)|EXTINF')"
}
log_counts() { # $1 logfile -> "already_running invalid_track dropping_invalid"
  [ -f "$1" ] || { echo "0 0 0"; return; }
  printf '%s %s %s\n' \
    "$(grep -c 'already running' "$1")" \
    "$(grep -c 'Requested invalid track' "$1")" \
    "$(grep -c 'Dropping invalid track' "$1")"
}

# ---------- HLS demand loop (stands in for CloudFront) ----------
hls_start() {
  mkdir -p "$RUN"
  (
    while true; do
      v=$(curl -s -m 5 "$TX_HTTP/hls/drill/index.m3u8" | grep -m1 -E '^[^#].*m3u8')
      if [ -n "$v" ]; then
        seg=$(curl -s -m 5 "$TX_HTTP/hls/drill/$v" | grep -m1 -E '^[^#].*\.ts')
        [ -n "$seg" ] && curl -s -m 8 -o /dev/null "$TX_HTTP/hls/drill/${v%/*}/$seg"
      fi
      sleep 1
    done
  ) >/dev/null 2>&1 &
  echo $! > "$RUN/hls.pid"
}
hls_stop() {
  [ -f "$RUN/hls.pid" ] && { pkill -P "$(cat "$RUN/hls.pid")" 2>/dev/null; kill "$(cat "$RUN/hls.pid")" 2>/dev/null; rm -f "$RUN/hls.pid"; }
  true
}

# ---------- sample line ----------
header() { echo -e "t\tclock\tin_live\tin_tracks\ttx_live\ttx_tracks\ttx_codecs\ttssrt\tbuffer\tmkvexec\touthls\thls\thls_lines\talready_running\tinvalid_track\tdropping"; }
sample() { # $1 t, $2 logfile
  local ij tj pj hj lj
  ij=$(json_probe "$IN_HTTP" "live+drill"); tj=$(json_probe "$TX_HTTP" drill)
  pj=$(tx_procs); hj=$(hls_probe); lj=$(log_counts "$2")
  set -- "$1" $ij $tj $pj $hj $lj
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$1" "$(date +%H:%M:%S)" "$2" "$3" "$5" "$6" "$7" "$8" "$9" "${10}" "${11}" "${12}" "${13}" "${14}" "${15}" "${16}"
}

wait_healthy() { # healthy = tx live=1, tracks>=1, hls 200 for STEADY_S consecutive seconds
  local t0=$SECONDS ok_since=-1 tj hj
  while :; do
    tj=$(json_probe "$TX_HTTP" drill); hj=$(hls_probe)
    set -- $tj $hj
    if [ "$1" = "1" ] && [ "$2" -ge 1 ] 2>/dev/null && [ "$4" = "200" ]; then
      [ $ok_since -lt 0 ] && ok_since=$SECONDS
      if [ $((SECONDS - ok_since)) -ge "$STEADY_S" ]; then log "healthy for ${STEADY_S}s: tx=$tj hls=$hj procs=$(tx_procs)"; return 0; fi
    else
      ok_since=-1
    fi
    if [ $((SECONDS - t0)) -gt "$HEALTHY_TIMEOUT" ]; then log "NOT healthy after ${HEALTHY_TIMEOUT}s: tx=$tj hls=$hj procs=$(tx_procs)"; return 1; fi
    sleep 2
  done
}

# ---------- triggers (run in the background while sampling) ----------
do_trigger() {
  case "$1" in
    restart)           log "T: docker restart drill-ingest"; docker restart -t 2 drill-ingest >/dev/null; log "T: restart done" ;;
    kill5|kill20|kill60|kill120)
      local n=${1#kill}; log "T: docker kill drill-ingest"; docker kill drill-ingest >/dev/null
      sleep "$n"; log "T: compose up -d ingest after ${n}s"; docker compose up -d ingest >/dev/null 2>&1; log "T: ingest up" ;;
    pause15|pause40)
      local n=${1#pause}; log "T: docker pause drill-ingest"; docker pause drill-ingest >/dev/null
      sleep "$n"; docker unpause drill-ingest >/dev/null; log "T: unpaused after ${n}s" ;;
    restart-srcpaused)
      log "T: docker pause drill-source"; docker pause drill-source >/dev/null; sleep 2
      log "T: docker restart drill-ingest"; docker restart -t 2 drill-ingest >/dev/null; log "T: restart done, source still paused"
      sleep 20; docker unpause drill-source >/dev/null; log "T: source unpaused after 20s" ;;
    restart-x3)        # three restarts 30 s apart: the 2nd and 3rd land inside the recovery window of the previous one
      for i in 1 2 3; do log "T: docker restart drill-ingest ($i/3)"; docker restart -t 2 drill-ingest >/dev/null; [ $i -lt 3 ] && sleep 30; done; log "T: restarts done" ;;
    srcpause40)        # NMW-2106 silence case: SIGSTOP the ffmpeg source only (ingest untouched) for 40 s
      log "T: docker pause drill-source"; docker pause drill-source >/dev/null
      sleep 40; docker unpause drill-source >/dev/null; log "T: source unpaused after 40s" ;;
    none)              log "T: no trigger (control run)" ;;
    *) log "unknown trigger $1"; return 1 ;;
  esac
}

verdict() { # $1 samples.tsv -> prints wedge analysis
  python3 -I - "$1" <<'EOF'
import sys, csv
rows = list(csv.DictReader(open(sys.argv[1]), delimiter="\t"))
def i(v):
    try: return int(v)
    except: return -1
best = cur = 0; start = None; bstart = None
for r in rows:
    wedged = i(r["tssrt"]) > 0 and r["tx_live"] == "1" and i(r["tx_tracks"]) == 0
    if wedged:
        if cur == 0: start = r["t"]
        cur += 1
        if cur > best: best, bstart = cur, start
    else: cur = 0
every = 2
if len(rows) > 1: every = i(rows[1]["t"]) - i(rows[0]["t"])
last = rows[-1] if rows else {}
healthy_at = next((r["t"] for r in rows if r["tx_live"] == "1" and i(r["tx_tracks"]) >= 1 and r["hls"] == "200" and i(r["t"]) > 0), None)
# first unhealthy after t=0, then first healthy after that
unh = next((r["t"] for r in rows if not (r["tx_live"] == "1" and i(r["tx_tracks"]) >= 1 and r["hls"] == "200")), None)
rec = None
if unh is not None:
    rec = next((r["t"] for r in rows if i(r["t"]) > i(unh) and r["tx_live"] == "1" and i(r["tx_tracks"]) >= 1 and r["hls"] == "200"), None)
print(f"samples={len(rows)} first_unhealthy_t={unh} recovered_t={rec} "
      f"longest_live0tracks_with_puller={best*every}s(from t={bstart}) "
      f"end: tx_live={last.get('tx_live')} tx_tracks={last.get('tx_tracks')} hls={last.get('hls')} "
      f"already_running={last.get('already_running')} invalid_track={last.get('invalid_track')} outhls={last.get('outhls')}")
w = best*every > 60 and i(last.get("already_running", "0")) > 0
print("WEDGE: " + ("REPRODUCED" if w else ("partial (live/0-tracks >60s but no 'already running')" if best*every > 60 else "no")))
EOF
}

run_trigger() {
  local name=$1 t0 logf out
  out="$RES/$name"
  mkdir -p "$out"; rm -f "$out"/*
  log "=== trigger $name: fresh stack"
  hls_stop; stack_up
  sleep 5; hls_start
  wait_healthy || log "WARNING: starting trigger $name from an unhealthy stack"
  logf="$out/transcode.log"
  docker logs -f --since 0s drill-transcode > "$logf" 2>&1 &
  local logpid=$!
  docker logs -f --since 0s drill-ingest > "$out/ingest.log" 2>&1 &
  local ilogpid=$!
  sleep 1
  header | tee "$out/samples.tsv"
  t0=$SECONDS
  sample 0 "$logf" | tee -a "$out/samples.tsv"
  ( do_trigger "$name" ) >> "$out/trigger.log" 2>&1 &
  local tpid=$!
  while [ $((SECONDS - t0)) -lt "$SAMPLE_FOR" ]; do
    sleep "$SAMPLE_EVERY"
    sample $((SECONDS - t0)) "$logf" | tee -a "$out/samples.tsv"
  done
  wait $tpid 2>/dev/null
  kill $logpid $ilogpid 2>/dev/null
  # keep the ingest log of the NEW container too (docker logs -f dies with the old one on kill/restart)
  docker logs drill-ingest > "$out/ingest.log" 2>&1
  docker logs drill-transcode > "$logf" 2>&1
  docker exec drill-transcode ps -o pid,etime,args > "$out/transcode.ps" 2>&1
  cat "$out/trigger.log"
  verdict "$out/samples.tsv" | tee "$out/verdict.txt"
}

status() {
  echo "ingest : $(json_probe "$IN_HTTP" live+drill)   procs: $(docker exec drill-ingest ps -o args 2>/dev/null | grep -oE 'Mist(In|Out|Proc)[A-Za-z]+' | sort | uniq -c | tr '\n' ' ')"
  echo "transcode: $(json_probe "$TX_HTTP" drill)   procs(tssrt buffer mkvexec outhls): $(tx_procs)   hls: $(hls_probe)"
}

case "${1:-}" in
  up)      stack_up; sleep 5; hls_start; wait_healthy; status ;;
  down)    stack_down ;;
  status)  status ;;
  list)    echo "$TRIGGERS" ;;
  trigger) run_trigger "${2:?trigger name}" ;;
  all)
    for t in ${2:-$TRIGGERS}; do run_trigger "$t"; done
    log "all triggers done; tearing down"; stack_down
    echo; for t in ${2:-$TRIGGERS}; do echo "== $t"; cat "$RES/$t/verdict.txt"; done ;;
  *) sed -n 2,9p "$0"; exit 1 ;;
esac
