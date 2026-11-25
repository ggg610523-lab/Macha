#!/usr/bin/env bash
# Build, test, and run CotEditor through the clean-room plsr-lite runtime.
#
# Usage:
#   ./decompiled/run-cleanroom.sh                 # build + tests + run
#   ./decompiled/run-cleanroom.sh --test-only     # build + tests only
#   ./decompiled/run-cleanroom.sh --no-build      # skip rebuild
#   ./decompiled/run-cleanroom.sh --trace         # enable ObjC/reef traces
#   PLSR_APP=/path/to/App.app ./decompiled/run-cleanroom.sh
#   PLSR_RUN_SECONDS=30 ./decompiled/run-cleanroom.sh
#
# If WAYLAND_DISPLAY is already set, it is used as-is. Otherwise the script
# starts `kwin_wayland --virtual` on a private socket and tears it down on exit.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
CLEANROOM="$ROOT/decompiled/src/cleanroom"
LOADER_DIR="$ROOT/decompiled/src/plsr-lite"
LOADER="$LOADER_DIR/plsr-lite"
SHIMDIR="$CLEANROOM/build"
RUNTIME="${PLSR_RUNTIME:-$ROOT/pulsar-appexec/usr/lib/pulsar-appexec}"
APP="${PLSR_APP:-$ROOT/decompiled/app/CotEditor.app}"
RUN_SECONDS="${PLSR_RUN_SECONDS:-20}"

DO_BUILD=1
DO_TESTS=1
DO_RUN=1
TRACE=0

for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        --no-test) DO_TESTS=0 ;;
        --test-only) DO_RUN=0 ;;
        --trace) TRACE=1 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown option: $arg" >&2
            exit 2
            ;;
    esac
done

if [ "$DO_BUILD" = 1 ]; then
    echo "== building clean-room shims =="
    make -C "$CLEANROOM"
    echo "== building loader =="
    make -C "$LOADER_DIR"
fi

for f in "$LOADER" "$SHIMDIR/libplsr-libsystem.so" "$SHIMDIR/libplsr-objc.so" \
         "$SHIMDIR/libplsr-foundation.so" "$SHIMDIR/libplsr-dispatch.so"; do
    if [ ! -e "$f" ]; then
        echo "missing build artifact: $f" >&2
        exit 1
    fi
done

if [ "$DO_TESTS" = 1 ]; then
    echo "== clean-room tests =="
    sh "$CLEANROOM/tests/run.sh"
fi

if [ "$DO_RUN" = 0 ]; then
    exit 0
fi

if [ ! -d "$APP" ]; then
    echo "app bundle not found: $APP" >&2
    exit 1
fi

KWIN_PID=""
cleanup() {
    if [ -n "$KWIN_PID" ] && kill -0 "$KWIN_PID" 2>/dev/null; then
        kill "$KWIN_PID" 2>/dev/null || true
        wait "$KWIN_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    if ! command -v kwin_wayland >/dev/null 2>&1; then
        echo "WAYLAND_DISPLAY is unset and kwin_wayland is not installed." >&2
        echo "Start a Wayland compositor and set WAYLAND_DISPLAY, or install KWin." >&2
        exit 1
    fi
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    SOCKET="plsr-wayland-$$"
    rm -f "$XDG_RUNTIME_DIR/$SOCKET"
    echo "== starting virtual kwin_wayland on $SOCKET =="
    kwin_wayland --virtual --socket "$SOCKET" --width 1024 --height 768 \
        --no-lockscreen --no-global-shortcuts >/tmp/plsr-kwin.log 2>&1 &
    KWIN_PID=$!
    for _ in $(seq 1 100); do
        [ -S "$XDG_RUNTIME_DIR/$SOCKET" ] && break
        sleep 0.1
    done
    if [ ! -S "$XDG_RUNTIME_DIR/$SOCKET" ]; then
        echo "kwin_wayland did not create a socket; see /tmp/plsr-kwin.log" >&2
        exit 1
    fi
    export WAYLAND_DISPLAY="$SOCKET"
fi

ENV_TRACE=()
if [ "$TRACE" = 1 ]; then
    ENV_TRACE=(env PLSR_OBJC_TRACE=1 PLSR_REEF_TRACE=1)
fi

echo "== running CotEditor (timeout ${RUN_SECONDS}s) =="
set +e
"${ENV_TRACE[@]}" timeout "$RUN_SECONDS" "$LOADER" \
    --shim-lib "$SHIMDIR/libplsr-libsystem.so" \
    --shim-lib "$SHIMDIR/libplsr-objc.so" \
    --shim-lib "$SHIMDIR/libplsr-foundation.so" \
    --shim-lib "$SHIMDIR/libplsr-dispatch.so" \
    --runtime "$RUNTIME" "$APP"
rc=$?
set -e

if [ "$rc" -eq 124 ]; then
    echo "PASS: CotEditor stayed alive for ${RUN_SECONDS}s"
    exit 0
fi
if [ "$rc" -eq 0 ]; then
    echo "PASS: CotEditor exited cleanly"
    exit 0
fi
echo "FAIL: CotEditor exited with status $rc" >&2
exit "$rc"
