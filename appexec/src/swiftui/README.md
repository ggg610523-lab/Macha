# SwiftUI reconstruction used by appexec

This directory owns the C view/layout/state/rendering engine linked into
`appexec`. Its implementation is reconstructed against the decompiled
SwiftUI-family exports and CotEditor UI evidence under `decompiled/`; the
recovered source is kept here so building appexec does not compile sources
from the separate clean-room runtime tree.

The engine provides a SwiftUI-shaped retained view graph, proposal-based
layout, environment inheritance, state/bindings/observable notifications,
text wrapping, a software RGBA canvas, PNG output, a hosting-view facade and a
painter callback interface. `engine.h` documents the C API.

`host-interface-index.tsv` and `host-interface-functions/` preserve the
focused Ghidra decompilation of the shipped
`pulsar-appexec/usr/lib/pulsar-appexec/host/libpulsar-app-host.so` rendering
interface. In particular, entry `0x132250` constructs a toolkit Window and
installs its content view and frame/full-screen callbacks. These are raw
decompiler outputs for reference, not compilable source. The ELF host library
is still loaded by appexec for native desktop surfaces; this SwiftUI engine
does not yet replace that 1.3 MB library or implement Swift ABI metadata and
all 24,470 SwiftUI framework functions.

The current presentation hook can paint the reconstructed node graph through
the host painter. `appexec.c` also projects common AppKit view and control APIs
into the Mayo widget tree, preserving frames and routing basic user actions
back to the source views. The editor supports text entry, selection, clipboard
shortcuts and undo/redo, and pushes edits back to the source text view. This is
an AppKit compatibility bridge; it does not
recover the original SwiftUI graph, SwiftUI ABI behavior, custom drawing,
complex data-source views or all native interactions. Those remain required
for broad application compatibility.
