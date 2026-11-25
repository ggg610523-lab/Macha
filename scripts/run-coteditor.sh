#!/bin/sh
# Launch the test app through pulsar-run. The runner provisions a private
# Wayland compositor when the host has none, then execs the staged appexec.
# Pass --dmg FILE (or a .dmg path) to run the .app straight from the image.
A=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNNER="$A/src/runner/pulsar-run"
APP="$A/app/CotEditor.app"

case ${1-} in
  --dmg)
    [ $# -ge 2 ] || { echo "usage: $0 --dmg FILE [app-args...]" >&2; exit 2; }
    shift
    APP=$1
    shift
    ;;
  *.dmg)
    APP=$1
    shift
    ;;
esac

if [ -x "$RUNNER" ]; then
  exec "$RUNNER" --app "$APP" -- "$@"
fi
case $APP in
  *.dmg)
    echo "$0: mounting a .dmg needs $RUNNER; run 'make -C $A/src/runner'" >&2
    exit 1
    ;;
esac
exec env LD_LIBRARY_PATH="$A/libs/compat:$A/libs/host:$A/libs/lib" \
  "$A/runtime/bin/appexec" "$APP" "$@"
