#!/usr/bin/env python3
"""Build the clean-room SwiftUI-family framework surface.

The inputs are the export lists recovered from the shipped Mach-O images.  The
output is an ELF assembly file containing one symbol for every public SwiftUI
family export, plus C manifest fragments used by libplsr-swiftui.so.

This is the ABI layer, not a behavior-complete SwiftUI implementation:
  * Swift function symbols become zero-returning stubs;
  * descriptor / witness-table / ObjC-class symbols become zeroed objects.

Keeping the surface generated from the logs means the clean-room framework
tracks the actual dependency closure instead of an invented hand-written list.
"""

from __future__ import annotations

from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
EXPORTS = ROOT / "decompiled" / "src" / "macho" / "exports"
OUT = HERE / "generated"

# name, install name, export-log basename
FRAMEWORKS = [
    ("SwiftUI",
     "/System/Library/Frameworks/SwiftUI.framework/Versions/A/SwiftUI",
     "usr_lib_pulsar-appexec_System_Library_Frameworks_SwiftUI.framework_Versions_A_SwiftUI.txt"),
    ("_StoreKit_SwiftUI",
     "/System/Library/Frameworks/_StoreKit_SwiftUI.framework/Versions/A/_StoreKit_SwiftUI",
     "usr_lib_pulsar-appexec_System_Library_Frameworks__StoreKit_SwiftUI.framework_Versions_A__StoreKit_SwiftUI.txt"),
    ("_AppIntents_SwiftUI",
     "/System/Library/Frameworks/_AppIntents_SwiftUI.framework/Versions/A/_AppIntents_SwiftUI",
     "usr_lib_pulsar-appexec_System_Library_Frameworks__AppIntents_SwiftUI.framework_Versions_A__AppIntents_SwiftUI.txt"),
    ("_AppIntents_AppKit",
     "/System/Library/Frameworks/_AppIntents_AppKit.framework/Versions/A/_AppIntents_AppKit",
     "usr_lib_pulsar-appexec_System_Library_Frameworks__AppIntents_AppKit.framework_Versions_A__AppIntents_AppKit.txt"),
    ("_GeoToolbox_AppIntents",
     "/System/Library/Frameworks/_GeoToolbox_AppIntents.framework/Versions/A/_GeoToolbox_AppIntents",
     "usr_lib_pulsar-appexec_System_Library_Frameworks__GeoToolbox_AppIntents.framework_Versions_A__GeoToolbox_AppIntents.txt"),
    ("_UserNotifications_AppIntents",
     "/System/Library/Frameworks/_UserNotifications_AppIntents.framework/Versions/A/_UserNotifications_AppIntents",
     "usr_lib_pulsar-appexec_System_Library_Frameworks__UserNotifications_AppIntents.framework_Versions_A__UserNotifications_AppIntents.txt"),
    ("AppIntents",
     "/System/Library/Frameworks/AppIntents.framework/Versions/A/AppIntents",
     "usr_lib_pulsar-appexec_System_Library_Frameworks_AppIntents.framework_Versions_A_AppIntents.txt"),
    ("StoreKit",
     "/System/Library/Frameworks/StoreKit.framework/Versions/A/StoreKit",
     "usr_lib_pulsar-appexec_System_Library_Frameworks_StoreKit.framework_Versions_A_StoreKit.txt"),
    ("Charts",
     "/System/Library/Frameworks/Charts.framework/Versions/A/Charts",
     "usr_lib_pulsar-appexec_System_Library_Frameworks_Charts.framework_Versions_A_Charts.txt"),
    ("WidgetKit",
     "/System/Library/Frameworks/WidgetKit.framework/Versions/A/WidgetKit",
     "usr_lib_pulsar-appexec_System_Library_Frameworks_WidgetKit.framework_Versions_A_WidgetKit.txt"),
    ("Intents",
     "/System/Library/Frameworks/Intents.framework/Versions/A/Intents",
     "usr_lib_pulsar-appexec_System_Library_Frameworks_Intents.framework_Versions_A_Intents.txt"),
    ("libswiftObservation",
     "/usr/lib/swift/libswiftObservation.dylib",
     "usr_lib_pulsar-appexec_usr_lib_swift_libswiftObservation.dylib.txt"),
    ("libswiftIntents",
     "/usr/lib/swift/libswiftIntents.dylib",
     "usr_lib_pulsar-appexec_usr_lib_swift_libswiftIntents.dylib.txt"),
]

# Swift metadata descriptors and tables are data, not callable entry points.
DATA_SUFFIXES = (
    "Mn", "Mp", "WP", "Mc", "MF", "N",
    "Wl", "Wr", "Wp", "Wt", "Wu",
)
DATA_PREFIXES = (
    "__swift_FORCE_LOAD_$",
    "_OBJC_CLASS_$_",
    "_OBJC_METACLASS_$_",
    "_OBJC_IVAR_$_",
)
SYMBOL_RE = re.compile(r"^[A-Za-z0-9_.$]+$")


def symbol_kind(name: str) -> str:
    if name.startswith(DATA_PREFIXES) or name.endswith(DATA_SUFFIXES):
        return "data"
    return "function"


def read_exports(path: Path) -> list[str]:
    if not path.exists():
        return []
    return [line.strip() for line in path.read_text(errors="replace").splitlines()
            if line.strip()]


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    symbols: dict[str, str] = {}
    manifest: list[tuple[str, str, int, int, int]] = []
    skipped: list[str] = []

    for name, install, filename in FRAMEWORKS:
        names = sorted(set(read_exports(EXPORTS / filename)))
        funcs = data = 0
        for symbol in names:
            if not SYMBOL_RE.match(symbol):
                skipped.append(symbol)
                continue
            kind = symbol_kind(symbol)
            symbols.setdefault(symbol, kind)
            if kind == "data":
                data += 1
            else:
                funcs += 1
        manifest.append((name, install, len(names), funcs, data))

    funcs = sorted(s for s, k in symbols.items() if k == "function")
    data = sorted(s for s, k in symbols.items() if k == "data")

    asm = [
        "/* Generated by swiftui/gen_frameworks.py.  Do not edit. */",
        ".section .text",
    ]
    for symbol in funcs:
        asm.extend([
            ".p2align 4",
            f".globl {symbol}",
            f".type {symbol}, @function",
            f"{symbol}:",
            "  xorl %eax, %eax",
            "  ret",
            f".size {symbol}, .-{symbol}",
        ])
    asm.append(".section .data")
    for symbol in data:
        asm.extend([
            ".p2align 3",
            f".globl {symbol}",
            f".type {symbol}, @object",
            f".size {symbol}, 64",
            f"{symbol}:",
            "  .zero 64",
        ])
    asm.append('.section .note.GNU-stack,"",@progbits')
    (OUT / "symbols.S").write_text("\n".join(asm) + "\n")

    with (OUT / "frameworks_manifest.inc").open("w") as f:
        for name, install, total, nfunc, ndata in manifest:
            f.write(f'    {{ "{name}", "{install}", {total}, {nfunc}, {ndata} }},\n')

    with (OUT / "install_names.inc").open("w") as f:
        for _, install, *_ in manifest:
            f.write(f'    "{install}",\n')

    total = len(symbols)
    print(f"swiftui: {len(manifest)} frameworks, {total} symbols "
          f"({len(funcs)} functions, {len(data)} data)")
    if skipped:
        print(f"swiftui: skipped {len(skipped)} invalid symbols", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
