#!/bin/sh
# Self-contained loader smoke test.
#
# Generates Mach-O fixtures from plsr-lite's generator in a temporary
# directory, then drives both loaders (cleanroom/build/appexec and
# plsr-lite/plsr-lite) over them. Needs only python3 and the built loaders;
# no macOS toolchain, app bundle or runtime root.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
APPEXEC="$ROOT/cleanroom/build/appexec"
PLSR="$ROOT/plsr-lite/plsr-lite"

[ -x "$APPEXEC" ] || make -C "$ROOT/cleanroom"
[ -x "$PLSR" ] || make -C "$ROOT/plsr-lite"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
cp "$ROOT/plsr-lite/tests/gen_machos.py" "$TMP/"
python3 "$TMP/gen_machos.py" >/dev/null

pass=0
fail=0
check() {
    name=$1; want_rc=$2; want_out=$3; shift 3
    out=$("$@" 2>&1) && rc=0 || rc=$?
    if [ "$rc" = "$want_rc" ] && printf '%s' "$out" | grep -q "$want_out"; then
        echo "PASS  $name (rc=$rc)"
        pass=$((pass + 1))
    else
        echo "FAIL  $name (rc=$rc, want $want_rc) :: $out"
        fail=$((fail + 1))
    fi
}

for bin in "$APPEXEC" "$PLSR"; do
    tag=$(basename "$bin")
    check "$tag runs LC_MAIN image"        0 "hello from mach-o" "$bin" "$TMP/hello.macho"
    check "$tag runs LC_UNIXTHREAD image"  0 "hello from mach-o" "$bin" "$TMP/hello_unixthread.macho"
    check "$tag applies classic rebase"    0 "hello from mach-o" "$bin" "$TMP/hello_rebase.macho"
    check "$tag applies classic binds"     0 "hello from mach-o" "$bin" "$TMP/hello_bind.macho"
    check "$tag applies chained fixups"    0 "hello from mach-o" "$bin" "$TMP/hello_chained.macho"
    check "$tag reports missing entry"     1 "no LC_MAIN" "$bin" "$TMP/noentry.macho"
    check "$tag --info lists imports"      0 "_write" "$bin" --info "$TMP/hello_chained.macho"
done

# Clean-room libSystem must satisfy an import the loader cannot.
SYS="$ROOT/cleanroom/build/libplsr-libsystem.so"
check "libSystem shim exports _strtoul" 0 "_strtoul" sh -c "nm -D '$SYS' | grep ' T _strtoul'"

printf 'totals: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
