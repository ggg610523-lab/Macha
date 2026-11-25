# macha — source tree

Everything in this directory is hand-written source for the loader and the
clean-room runtime, plus the one third-party toolkit needed to build the full
window front end. Nothing here is decompiler output, an extracted
application/runtime bundle, or a prebuilt binary.

## Contents

| Path | What it is |
|---|---|
| `appexec/` | From-source Mach-O loader and host ABI (`src/appexec.c`, `src/reef_host.c`), SwiftUI-shaped engine sources, optional Mayonnaise window shell (`src/mayo_ui.cpp`, `src/shell_state.cpp`), generated-fixture test sources and the launcher |
| `cleanroom/` | Clean-room replacements for macOS libraries (`libSystem`, libobjc, Foundation, CoreFoundation, libdispatch, AppKit, SwiftUI family) plus tests and ABI generators |
| `plsr-lite/` | Loader-only variant built from `appexec` sources, with its fixture generator and suite |
| `deps/mayonnaise/` | Vendored Mayonnaise toolkit source (the only third-party build dependency), including its bundled glad/stb; no build tree and no static library are shipped |
| `tools/` | `reef_mtl_probe.c`, `macho-deps`, `re_status.py`, `reef_mtl.h` |
| `runner/` | `pulsar-run` DMG/`.app` mount launcher |
| `conformance/` | Loader conformance harness |
| `scripts/` | `run-cleanroom.sh`, `run-coteditor.sh` |
| `docs/` | Project README, roadmap and progress log |
| `tests/smoke.sh` | Self-contained loader smoke test |

Excluded on purpose: `decompiled/src/macho`, `decompiled/src/elf`,
`decompiled/src/plsr`, `decompiled/ghidra` (Ghidra/Ghidra output), the other
cloned vendor trees (headers, pearOS frameworks), `decompiled/app/*` and
`pulsar-appexec/*` (extracted application and shipped runtime binaries),
`decompiled/src/swiftui/host-interface-functions` (per-function decompiled
dumps), plus every `.o`, `.so`, `.a`, `.dylib`, `.macho`, `.app` fixture and
prebuilt executable.

## Build

    make -j"$(nproc)"        # clean-room libraries + loader, plsr-lite, tools
    make smoke               # generate Mach-O fixtures, run both loaders
    make full-appexec        # loader + Mayonnaise window front end
    make clean

`make` builds:

- `cleanroom/build/`: the seven clean-room libraries and `appexec` (the loader
  built from `appexec/src/appexec.c` against the shared SwiftUI engine).
- `plsr-lite/plsr-lite`: the loader-only variant.
- `tools/reef_mtl_probe`, `tools/macho-deps/macho-deps`.

`make smoke` generates the loader fixtures with `plsr-lite/tests/gen_machos.py`
in a temporary directory and drives both `cleanroom/build/appexec` and
`plsr-lite/plsr-lite` over them (LC_MAIN, LC_UNIXTHREAD, classic rebase and
bind, chained fixups, missing entry, `--info`), then checks the clean-room
libSystem export. It needs only `python3`.

`make full-appexec` builds `deps/mayonnaise` with CMake and links
`appexec/appexec` with the Mayonnaise/SDL3 front end. It skips cleanly with
exit 0 when `cmake` or the SDL3/FreeType development packages are missing; the
rest of the tree does not depend on them. The loader reaches that front end
only through weak `plsr_mayo_*` symbols, so `cleanroom/build/appexec` runs
without it.

`runner/`, the `appexec/tests` and `cleanroom/tests` suites, and the
`scripts/` launchers expect an extracted macOS application/runtime on the host
(and a compositor); they are source, not part of the default build.

## Special thanks

Special thanks to **Alexandru Balan**, and to everyone else who helped along
the way — this tree would not exist without their support and contributions.

Credit is also due to the projects this work builds on:

- The **Darling** project and its contributors — Lubos Dolezel, Wenqi Chen,
  Sergey Bugaev, Andrew Hyatt, Chris Wulff and the wider Darling community —
  for the macOS compatibility layer vendored under `darling/`.
- The **Mayonnaise** toolkit authors, for the windowing front end vendored
  under `deps/mayonnaise/`.
- The maintainers of the compilers, C library and graphics stack it runs on.
