# cleanroom

Clean-room host libraries: ELF replacements for the macOS frameworks and
libraries a Mach-O image imports. Each library exports the leading-underscore
names the image binds to and lists the install names it replaces through
`plsr_host_imports`.

This directory is self-contained. It builds the libraries plus the Mach-O
loader (`build/appexec`) from source — there is no prebuilt host binary, no
application bundle and no reference-app fixture. `swiftui/generated/` is
checked in, so the build needs only a C/C++ compiler and the system
zlib/OpenSSL.

    make            # build all seven libraries + build/appexec
    make -j"$(nproc)"
    make clean

## Libraries

| Library | Replaces |
|---|---|
| `libplsr-libsystem.so` | `libSystem.B`, `libsystem_c`, `libsystem_m` — libc/BSD/math symbols, Darwin→Linux translations for `open`/`stat`/`mmap`/`fcntl`, pthread handles, CommonCrypto via OpenSSL |
| `libplsr-objc.so` | `libobjc.A` — class/category scan, plain and relative method lists, `objc_msgSend`/`Super2`/`stret`, property and ARC entry points |
| `libplsr-foundation.so` | `Foundation` — `NSObject`, `NSString`/`NSMutableString`, `NSNumber`, `NSArray`, `NSDictionary`, `NSNull`, `NSData`, `NSLog`, plus the long-tail class surface |
| `libplsr-corefoundation.so` | `CoreFoundation` — CF object model, CFString/Array/Dictionary/Number/Data/Set/UUID/URL/Bundle, run loops, notification center, in-memory preferences, toll-free bridging |
| `libplsr-dispatch.so` | `libdispatch` — serial queues, sync/async, `dispatch_once`, semaphores, groups, `dispatch_apply` |
| `libplsr-appkit.so` | `AppKit` — `NSApplication`/`NSWindow`/`NSView`/controls, menus and toolbars, `NSNib`/`NSStoryboard`/view controllers with NIBArchive decoding, autoresizing layout, software RGBA drawing, responder chain and events, `plsr_surface_*` host ABI |
| `libplsr-swiftui.so` | SwiftUI family (13 install names) — generated ABI surface plus view graph, proposal layout, rasterizer, text and hosting engine |

## Loader (`appexec/`)

`appexec/appexec.c` is the from-scratch x86_64 Mach-O loader. It validates and
maps segments, applies dyld rebases/binds and chained fixups, loads the
dependency closure under a runtime root, and can substitute the clean-room
libraries for macOS images with `--shim-lib` (their `plsr_host_imports`
manifest supplies the install names). It also carries the `--shim-lib`
clean-room launch mode and the `PLSR_ABORT_STACK` backtrace dump.

    build/appexec <macho|.app> [args...]
    build/appexec --info  <macho|.app>
    build/appexec --load  <macho|.app>
    build/appexec --shim-lib build/libplsr-libsystem.so <macho>

`appexec/mayo_ui.cpp` and `appexec/shell_state.cpp` are the optional
Mayonnaise/SDL3 window frontend. They need the external toolkit and are not
part of this build; `appexec.c` reaches them only through weak
`plsr_mayo_*` symbols and falls back when they are absent.

## Layout

```
*.c, *.S, *.h        library sources
appkit_surface.h     host surface ABI exported by libplsr-appkit.so
swiftui/             view-graph engine, text, canvas and framework manifest
swiftui/generated/   pre-generated ABI symbols and install-name manifests
appexec/             Mach-O loader source (built to build/appexec)
build/               build output (git-ignored)
```

## Limits

The SwiftUI ABI symbols return zero and metadata/tables are zeroed objects;
the behavioral engine is the C API in `swiftui/engine.h`. AppKit has no native
window, IME input or scroll/tracking behavior — a host attaches a surface and
feeds events through `plsr_surface_*`. libSystem still lacks Darwin pthread
condition-variable exports and rwlocks, file-lock `fcntl` commands, and Darwin
error-code translation; libdispatch lacks concurrent queues, QoS and sources.
