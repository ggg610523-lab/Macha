#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUTDIR="${1:-$ROOT/decompiled/re/coteditor-compare}"
MODE="${MODE:-both}"   # ref|ours|both
SECONDS_WAIT="${SECONDS_WAIT:-8}"
WIDTH="${WIDTH:-1600}"
HEIGHT="${HEIGHT:-1000}"
SOCK="cotcmp-$$"
RUNTIME="$ROOT/pulsar-appexec/usr/lib/pulsar-appexec"
APP="$ROOT/decompiled/app/CotEditor.app"
REF_BIN="$ROOT/decompiled/runtime/bin/appexec"
OUR_BIN="$ROOT/appexec/appexec"

mkdir -p "$OUTDIR"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"

have() { command -v "$1" >/dev/null 2>&1; }

capture() {
  local out="$1"
  if have spectacle; then
    WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" timeout 25 spectacle -b -n -o "$out" >/dev/null 2>&1 && [ -s "$out" ] && return 0
    WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" timeout 25 spectacle --fullscreen --background --nonotify --output "$out" >/dev/null 2>&1 && [ -s "$out" ] && return 0
  fi
  if have grim; then
    WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" timeout 25 grim "$out" >/dev/null 2>&1 && [ -s "$out" ] && return 0
  fi
  return 1
}

run_case() {
  local name="$1"; shift
  local log="$OUTDIR/$name.log"
  local shot="$OUTDIR/$name.png"
  local meta="$OUTDIR/$name.meta"
  rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock" "$shot"
  kwin_wayland --virtual --socket "$SOCK" --width "$WIDTH" --height "$HEIGHT" --no-lockscreen --no-global-shortcuts >"$OUTDIR/$name.kwin.log" 2>&1 &
  local kwin=$!
  cleanup() { kill "$apid" 2>/dev/null || true; sleep 0.2; kill "$kwin" 2>/dev/null || true; wait "$kwin" 2>/dev/null || true; }
  trap cleanup RETURN
  for _ in $(seq 1 120); do [ -S "$XDG_RUNTIME_DIR/$SOCK" ] && break; sleep 0.1; done
  [ -S "$XDG_RUNTIME_DIR/$SOCK" ] || { echo "no-socket" >"$meta"; return 1; }
  export WAYLAND_DISPLAY="$SOCK"
  (
    export WAYLAND_DISPLAY="$SOCK"
    "$@"
  ) >"$log" 2>&1 &
  apid=$!
  sleep "$SECONDS_WAIT"
  local alive=no
  kill -0 "$apid" 2>/dev/null && alive=yes
  capture "$shot" || true
  local shot_ok=no
  [ -s "$shot" ] && shot_ok=yes
  {
    echo "name=$name"
    echo "alive=$alive"
    echo "pid=$apid"
    echo "shot=$shot_ok"
    echo "cmd=$*"
  } >"$meta"
  if [ "$shot_ok" = yes ]; then
    python3 - "$shot" "$OUTDIR/$name.crop.png" >"$OUTDIR/$name.metrics" <<'PY'
import sys, json
from PIL import Image
import numpy as np
p=sys.argv[1]; out=sys.argv[2]
im=Image.open(p).convert('RGB')
a=np.array(im)
lum=a.mean(axis=2)
mask=lum>210
ys,xs=np.where(mask)
res={'size':[im.size[0],im.size[1]], 'bright_pixels':int(mask.sum())}
if len(xs):
    row=mask.mean(axis=1); col=mask.mean(axis=0)
    yr=np.where(row>0.03)[0]; xr=np.where(col>0.03)[0]
    if len(xr) and len(yr):
        bb=(int(xr.min()),int(yr.min()),int(xr.max()),int(yr.max()))
        res['bbox']=bb
        im.crop(bb).save(out)
print(json.dumps(res))
PY
  fi
  kill "$apid" 2>/dev/null || true
  sleep 0.2
  kill "$kwin" 2>/dev/null || true
  wait "$kwin" 2>/dev/null || true
  trap - RETURN
}

if [ "$MODE" = ref ] || [ "$MODE" = both ]; then
  run_case ref env \
    LD_LIBRARY_PATH="$RUNTIME/compat:$RUNTIME/host:$RUNTIME/lib" \
    LD_PRELOAD="$RUNTIME/compat/libicudata.so.76.1:$RUNTIME/compat/libicui18n.so.76.1:$RUNTIME/compat/libicuuc.so.76.1:$RUNTIME/compat/libjpeg.so.62.3.0:$RUNTIME/compat/libpulsar-toolkit-base.so.1.0:$RUNTIME/compat/libpulsar-toolkit.so.1.0" \
    "$REF_BIN" "$APP" || true
fi

if [ "$MODE" = ours ] || [ "$MODE" = both ]; then
  run_case ours env \
    PLSR_NSWINDOW=1 PLSR_MAYO_MAIN_THREAD=1 \
    "$OUR_BIN" --runtime "$RUNTIME" "$APP" || true
fi

python3 - "$OUTDIR" <<'PY'
import json, os, sys
out=sys.argv[1]
def load(name):
    p=os.path.join(out,name+'.metrics')
    if not os.path.exists(p): return None
    return json.load(open(p))
ref=load('ref'); ours=load('ours')
print(json.dumps({'ref':ref,'ours':ours}, indent=2))
PY
