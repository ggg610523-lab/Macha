#!/usr/bin/env python3
"""Read the decompiled SwiftUI framework and report engine coverage.

Inputs:
  * decompiled/src/macho/<SwiftUI>/index.tsv - every recovered function
  * decompiled/src/macho/exports/*.txt      - the public export lists
  * the clean-room engine sources           - what is actually implemented

Output: swiftui/generated/inventory.md
"""

from __future__ import annotations

from collections import Counter
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
EXPORTS = ROOT / "decompiled" / "src" / "macho" / "exports"
FRAMEWORK_DIR = (
    ROOT / "decompiled" / "src" / "macho" /
    "usr_lib_pulsar-appexec_System_Library_Frameworks_SwiftUI.framework_"
    "Versions_A_SwiftUI"
)

FRAMEWORK_EXPORTS = [
    "usr_lib_pulsar-appexec_System_Library_Frameworks_SwiftUI.framework_Versions_A_SwiftUI.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks__StoreKit_SwiftUI.framework_Versions_A__StoreKit_SwiftUI.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks__AppIntents_SwiftUI.framework_Versions_A__AppIntents_SwiftUI.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks__AppIntents_AppKit.framework_Versions_A__AppIntents_AppKit.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks__GeoToolbox_AppIntents.framework_Versions_A__GeoToolbox_AppIntents.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks__UserNotifications_AppIntents.framework_Versions_A__UserNotifications_AppIntents.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks_AppIntents.framework_Versions_A_AppIntents.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks_StoreKit.framework_Versions_A_StoreKit.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks_Charts.framework_Versions_A_Charts.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks_WidgetKit.framework_Versions_A_WidgetKit.txt",
    "usr_lib_pulsar-appexec_System_Library_Frameworks_Intents.framework_Versions_A_Intents.txt",
    "usr_lib_pulsar-appexec_usr_lib_swift_libswiftObservation.dylib.txt",
    "usr_lib_pulsar-appexec_usr_lib_swift_libswiftIntents.dylib.txt",
]

# label, symbol pattern, status, note
CAPABILITIES = [
    ("View protocol / body", r"\$s7SwiftUI4ViewP", "partial",
     "view graph and layout exist; the Swift witness tables stay stubs"),
    ("Text", r"\$s7SwiftUI4TextV", "yes",
     "wrapping, alignment, font size/weight, colour"),
    ("Font", r"\$s7SwiftUI4FontV", "partial",
     "size, weight and monospaced map onto the built-in font"),
    ("Colour", r"\$s7SwiftUI5ColorV", "partial",
     "sRGB fill/foreground colours"),
    ("Image", r"\$s7SwiftUI5ImageV", "partial",
     "placeholder plate plus symbol text"),
    ("HStack", r"\$s7SwiftUI6HStack", "yes",
     "spacing, flex children, cross-axis alignment"),
    ("VStack", r"\$s7SwiftUI6VStack", "yes",
     "spacing, flex children, cross-axis alignment"),
    ("ZStack", r"\$s7SwiftUI6ZStack", "yes", "overlay layout"),
    ("Spacer", r"\$s7SwiftUI6Spacer", "yes", "flexible space"),
    ("Divider", r"\$s7SwiftUI7Divider", "yes", "1pt rule, axis aware"),
    ("Rectangle", r"\$s7SwiftUI9RectangleV", "yes", "filled rectangle"),
    ("RoundedRectangle", r"\$s7SwiftUI16RoundedRectangleV", "yes",
     "anti-aliased rounded fill"),
    ("Circle", r"\$s7SwiftUI6CircleV", "yes", "anti-aliased circle"),
    ("Capsule", r"\$s7SwiftUI7CapsuleV", "yes", "capsule fill"),
    ("Path", r"\$s7SwiftUI4PathV", "no", "filled rectangle fallback only"),
    ("Button", r"\$s7SwiftUI6ButtonV", "partial",
     "plate and label; no action dispatch"),
    ("Frame / padding", r"\$s7SwiftUI15ModifiedContentV", "partial",
     "frame, padding, background, overlay, opacity, offset, clip"),
    ("Layout protocol", r"\$s7SwiftUI6LayoutP", "partial",
     "HStack/VStack/ZStack only"),
    ("List", r"\$s7SwiftUI4ListV", "partial",
     "vertical rows, separators, leftover height"),
    ("ForEach", r"\$s7SwiftUI7ForEachV", "partial",
     "static content in a vertical stack, no identity diffing"),
    ("TextField", r"\$s7SwiftUI9TextFieldV", "partial",
     "placeholder/value, focus caret, no input events"),
    ("EnvironmentValues", r"\$s7SwiftUI17EnvironmentValuesV", "yes",
     "keyed store with inheritance: colorScheme, font, foregroundColor, accentColor"),
    ("Binding", r"\$s7SwiftUI7BindingV", "yes",
     "get/set closures, constants, state and observable bindings"),
    ("State", r"\$s7SwiftUI5StateV", "yes",
     "node-local cells, revisions and host invalidation"),
    ("NSHostingView", r"\$s7SwiftUI13NSHostingViewC", "partial",
     "hosting view wrapper plus the sw_paint primitive adapter; AppKit bridge pending"),
    ("WindowGroup / scenes", r"\$s7SwiftUI11WindowGroupV", "no",
     "windows are created by the host"),
    ("Animation", r"\$s7SwiftUI9AnimationV", "no", "no timeline yet"),
]

ENGINE_FUNCTION_RE = re.compile(
    r"^(?:[A-Za-z_][A-Za-z0-9_ *]*?)\b(sw_[a-z0-9_]+)\s*\(", re.MULTILINE
)


def type_name(symbol: str) -> str:
    match = re.match(r"^\$s7SwiftUI(\d+)([A-Za-z0-9_]+)", symbol)
    if not match:
        return ""
    length = int(match.group(1))
    return match.group(2)[:length]


def read_functions() -> list[str]:
    index = FRAMEWORK_DIR / "index.tsv"
    if not index.exists():
        return []
    names = []
    for line in index.read_text(errors="replace").splitlines()[1:]:
        parts = line.split("\t")
        if len(parts) >= 3 and parts[2]:
            names.append(parts[2].lstrip("_"))
    return names


def engine_functions() -> list[str]:
    names = set()
    for path in sorted(HERE.glob("*.c")):
        names.update(ENGINE_FUNCTION_RE.findall(path.read_text(errors="replace")))
    return sorted(names)


def main() -> int:
    functions = read_functions()
    if not functions:
        print("inventory: no decompiled SwiftUI functions found", file=sys.stderr)
        return 1

    types = Counter(filter(None, (type_name(name) for name in functions)))
    engine = engine_functions()

    export_counts = []
    for filename in FRAMEWORK_EXPORTS:
        path = EXPORTS / filename
        count = 0
        if path.exists():
            count = sum(1 for line in path.read_text(errors="replace").splitlines()
                        if line.strip())
        export_counts.append((filename.split("_")[-1].replace(".txt", ""), count))

    implemented = []
    for label, pattern, status, note in CAPABILITIES:
        regex = re.compile(pattern)
        matches = sum(1 for name in functions if regex.search(name))
        implemented.append((label, matches, status, note))

    lines = [
        "# SwiftUI framework inventory and engine coverage",
        "",
        "Generated by `swiftui/inventory.py` from the decompiled framework.",
        "",
        f"- recovered functions in the SwiftUI framework: **{len(functions):,}**",
        f"- distinct mangled types: **{len(types):,}**",
        f"- clean-room engine functions: **{len(engine)}**",
        f"- generated ABI symbols across 13 install names: "
        f"**{sum(count for _, count in export_counts):,}**",
        "",
        "## Largest types in the framework",
        "",
        "| Type | Functions |",
        "|---|---:|",
    ]
    for name, count in types.most_common(25):
        lines.append(f"| `{name}` | {count} |")

    lines += [
        "",
        "## Engine coverage",
        "",
        "| Capability | Framework functions | Status | Notes |",
        "|---|---:|---|---|",
    ]
    for label, matches, status, note in implemented:
        lines.append(f"| {label} | {matches} | {status} | {note} |")

    lines += [
        "",
        "## Engine entry points",
        "",
        "```",
        "\n".join(engine),
        "```",
        "",
    ]

    out = HERE / "generated" / "inventory.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines))

    covered = sum(1 for _, _, status, _ in implemented if status == "yes")
    partial = sum(1 for _, _, status, _ in implemented if status == "partial")
    print(f"inventory: {len(functions):,} framework functions, "
          f"{len(types):,} types, {len(engine)} engine functions, "
          f"{covered} covered / {partial} partial capabilities -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
