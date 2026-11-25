#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=$(dirname "$HERE")/appexec
DMG=$HERE/../../CotEditor_7.1.3.dmg
FIX=$HERE/../../decompiled/conformance/fixtures
APP=$HERE/../../decompiled/app/CotEditor.app/Contents/MacOS/CotEditor
PKG=$HERE/../../pulsar-appexec/usr/lib/pulsar-appexec
RT=$HERE/runtime
python3 "$HERE/gen_machos.py" >/dev/null || exit 1
pass=0; fail=0
check() {
  name=$1; erc=$2; eout=$3; shift 3
  out=$("$@" 2>&1); rc=$?
  if [ "$rc" = "$erc" ] && printf '%s' "$out" | grep -q "$eout"; then
    echo "PASS  $name (rc=$rc)"; pass=$((pass+1))
  else
    echo "FAIL  $name (rc=$rc, want $erc) :: $out"; fail=$((fail+1))
  fi
}
check "LC_MAIN executable runs"          0 "hello from mach-o" "$BIN" "$HERE/hello.macho"
check "LC_UNIXTHREAD entry runs"         0 "hello from mach-o" "$BIN" "$HERE/hello_unixthread.macho"
check "classic dyld rebase under slide"  0 "hello from mach-o" "$BIN" "$HERE/hello_rebase.macho"
check "classic dyld bind opcodes"        0 "hello from mach-o" "$BIN" "$HERE/hello_bind.macho"
check "chained fixups rebase + bind"     0 "hello from mach-o" "$BIN" "$HERE/hello_chained.macho"
check ".app bundle resolution runs"      0 "hello from mach-o" "$BIN" "$HERE/Test.app"
check "rejects non-Mach-O (DMG)"         1 "is not a Mach-O file" "$BIN" "$DMG"
check "rejects 32-bit Mach-O"            1 "64-bit" "$BIN" "$FIX/macho32.bin"
check "rejects dylib filetype"           1 "unsupported Mach-O file type" "$BIN" "$FIX/dylib_x86.bin"
check "reports missing entry point"      1 "no LC_MAIN" "$BIN" "$HERE/noentry.macho"
check "--info on chained fixture"        0 "_write" "$BIN" --info "$HERE/hello_chained.macho"
check "--info on real CotEditor"         0 "libSystem" "$BIN" --info "$APP"
check "loads a disk dylib dependency"    0 "hello from dylib" "$BIN" --runtime "$RT" "$HERE/hello_dylib.macho"
check "recursive dylib dependency"       0 "hello from dylib" "$BIN" --runtime "$RT" "$HERE/hello_recursive.macho"
check "rpath framework dependency"       0 "hello from dylib" "$BIN" "$HERE/Rpath.app"
check "loads real CotEditor closure"     0 "images: 84 loaded, 0 missing" "$BIN" --load --runtime "$PKG" "$APP"
check "real closure fixups apply cleanly" 0 "errors: 0" "$BIN" --load --runtime "$PKG" "$APP"
printf 'totals: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
