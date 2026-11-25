#!/usr/bin/env bash
# Launch a macOS .app through the from-source appexec loader.
#
#   ./appexec/run-app.sh [.app | .dmg] [app args...]
#
# PULSAR_RUNTIME overrides the pulsar runtime root (default:
# pulsar-appexec/usr/lib/pulsar-appexec).  If WAYLAND_DISPLAY is unset a
# private kwin_wayland compositor is started and torn down on exit.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
LOADER="$HERE/appexec"
RUNTIME="${PULSAR_RUNTIME:-$ROOT/pulsar-appexec/usr/lib/pulsar-appexec}"
APP="${1:-$ROOT/decompiled/app/CotEditor.app}"
shift || true

# Run the app's AppKit event-loop bridge by default. Without this hook the
# executable reaches its entry point but the loader never bootstraps the
# storyboard delegate or presents the app-owned NSWindow.
export PLSR_NSWINDOW="${PLSR_NSWINDOW:-1}"
export PLSR_MAYO_MAIN_THREAD="${PLSR_MAYO_MAIN_THREAD:-1}"

[ -x "$LOADER" ] || make -C "$HERE"

if [ "${APP##*.}" = "dmg" ]; then
    MOUNT="$ROOT/decompiled/src/runner/pulsar-run"
    [ -x "$MOUNT" ] || make -C "$ROOT/decompiled/src/runner"
    APP="$("$MOUNT" --mount-only --app "$APP")"
fi

KWIN=""
cleanup() {
    if [ -n "$KWIN" ] && kill -0 "$KWIN" 2>/dev/null; then
        kill "$KWIN" 2>/dev/null || true
        wait "$KWIN" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    command -v kwin_wayland >/dev/null 2>&1 || {
        echo "run-app: no WAYLAND_DISPLAY and kwin_wayland is not installed" >&2
        exit 1
    }
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    SOCK="appexec-$$"
    kwin_wayland --virtual --socket "$SOCK" --width 1600 --height 1000 \
        --no-lockscreen --no-global-shortcuts >/tmp/appexec-kwin.log 2>&1 &
    KWIN=$!
    for _ in $(seq 1 100); do
        [ -S "$XDG_RUNTIME_DIR/$SOCK" ] && break
        sleep 0.1
    done
    [ -S "$XDG_RUNTIME_DIR/$SOCK" ] || {
        echo "run-app: kwin_wayland did not start; see /tmp/appexec-kwin.log" >&2
        exit 1
    }
    export WAYLAND_DISPLAY="$SOCK"
fi

# The runtime layer must run the app's own AppKit UI.  PLSR_MAYO_UI=1 selects
# appexec's diagnostic shell instead; it is not a CotEditor substitute.
exec "$LOADER" --runtime "$RUNTIME" "$APP" "$@"
