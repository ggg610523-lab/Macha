# appexec — a from-source reimplementation of `pulsar-appexec`

This tree is a clean-room reimplementation of the loader and host runtime in
`pulsar-appexec/` (the `reef-mac` package tree). It loads a macOS `.app`,
maps and fixes up the whole Mach-O image graph, installs the pulsar host ABI
(`_reef_host`) and jumps to the bundle's entry point.

The SwiftUI-shaped renderer is owned by `src/swiftui/`; `make` compiles these
local sources. Focused Ghidra decompilations of the shipped
`host/libpulsar-app-host.so` window interface are retained in
`src/swiftui/host-interface-functions/` as reference. That native ELF host is
still used for desktop surfaces. The recovered C engine does not yet replace
the original SwiftUI runtime or reconstruct CotEditor's SwiftUI graph.

    make
    make test
    ./run-app.sh ../decompiled/app/CotEditor.app

`make` links the Mayonnaise C++ drawing backend, so it also needs the
Mayonnaise static library at
`decompiled/vendor/mayonnaise/build/src/libmayonnaise.a` and the system
SDL3/FreeType/OpenGL/Vulkan development packages.

## What is implemented

The presented AppKit window is projected through a generic control bridge in
`src/appexec.c` and `src/mayo_ui.cpp`. It reads each visible `NSView` class,
frame, hidden/enabled state, text and common control values, then renders
containers, labels, buttons, editable fields, checkboxes, toggles, sliders,
progress indicators, segmented controls, popup menus, images and separators.
Menu item and segment titles are read from their model APIs because AppKit does
not expose them as subviews. Input is sent back through each source object's
`performClick:`, setter, selection API or target/action pair. The app's
`NSMenu` hierarchy stays in the AppKit object model. Since this Linux host has
no macOS global menu bar, the runtime projects its real top-level items and
submenus into host menu controls; it does not invent app-specific menu content.
Text storage compatibility is enabled by default for live text views so source
`NSTextView` objects retain input and their `didChangeText` observers run. Set
`PLSR_DISABLE_TEXT_STORE=1` to diagnose the native text-storage implementation.
The bundled launcher sets `PLSR_NSWINDOW=1` by default so the app's event loop
can bootstrap its real storyboard delegate and present its own window.
This is an API-family bridge, not a claim of complete AppKit compatibility:
custom-drawn views, complex table data sources, native popovers, accessibility
semantics and the full SwiftUI ABI still need dedicated implementations.

`src/appexec.c` — the loader (derived from the decompiled `main` and the
loader entry at `0x112f80` / `FUN_00147190`):

- argument handling and runtime-root discovery from `/proc/self/exe`
  (`<bindir>/../lib/pulsarappexec`, `$HOME/.local/share/pulsarappexec`,
  `--runtime DIR`)
- `.app` resolution through `Contents/Info.plist` -> `CFBundleExecutable`,
  falling back to the first file in `Contents/MacOS`
- Mach-O validation (`not a Mach-O file`, `not a 64-bit Mach-O file`,
  non-PIE rejection, header-mapping segment check)
- thin and fat images, x86_64 slice selection, `__PAGEZERO` handling
- per-segment mapping, slide, deferred protection, `LC_MAIN` and
  `LC_UNIXTHREAD` entry points
- `LC_LOAD_DYLIB`/weak/reexport/lazy/upward dependencies, `@rpath`,
  `@loader_path`, `@executable_path`, absolute install names under the
  runtime root, recursive closure with dedup
- classic `LC_DYLD_INFO` rebase/bind/lazy/weak opcodes
- `LC_DYLD_CHAINED_FIXUPS`: import formats 1/2/3, plain and zlib symbol
  pools, `DYLD_CHAINED_PTR_64` and `_64_OFFSET`
- export tries (including reexports) and `LC_SYMTAB` fallback
- Swift image registration (`__swift5_types`, `__swift5_protos`,
  `__swift5_proto`) with libswiftCore after fixups
- ObjC image registration: after `protect_image`, `map_objc_images()` calls
  libobjc's `_mapImages` (local symbol, resolved at `libobjc+0x2d70`) over all
  images, then libobjc's `__mod_init_func` (`_initializeRuntime`) registers the
  `_SEL_*` globals, and every image's remaining initializers run in reverse
  load order.  This is dyld's order: map images, then run initializers.
- `_objc_setForwardHandler` is pointed at a nil-returning handler so a missing
  selector cannot recurse through `_reef_objc_forward` until the stack dies
- the process stack (`argc/argv/envp`), Darwin's fourth `main` argument
  (`apple`), and the jump to the image entry
- `--info`, `--load`, `--trace-missing`, `--stub-missing`, `--image-map`,
  `--shim-lib` diagnostic modes

`src/reef_host.c` — the host ABI. The shipped Mach-O `libSystem.B.dylib` is
an adapter: each libc/POSIX entry point loads a function pointer from a
`0x680`-byte table and tail-calls it. The loader stores the table in
libSystem.B's `_reef_host` data slot and publishes `table[0x258]` as
`_reef_tlv_offset`. `reef_host_init()` builds that table over the host libc
(memory, strings, formatting, descriptors, directory streams, dynamic symbol
lookup) and loads the vendor ELF host libraries
(`host/libpulsar-app-host.so`, `lib/libpulsar-metal-*.so`) that AppKit and
SwiftUI use for rendering.

Darwin addresses thread-locals through `%gs`, not `%fs`: the runtime reads
its TLV array at `%gs:-0x1818`, so `install_reef_host()` allocates a GS block,
publishes an empty TLV array in it and calls `arch_prctl(ARCH_SET_GS, ...)`.

## Evidence used

- Decompiled loader: `decompiled/src/elf/usr_lib_pulsar-appexec_bin_appexec/`
  (`main` at Ghidra `0x112f80`, the run path in `FUN_00147190`, the host
  table constructor in `FUN_00128e20`).
- Runtime dump of the shipped loader's table: 208 live slots, with
  `+0x48` = `malloc`, `+0x50` = `calloc`, `+0x58` = `realloc`, `+0x60` =
  `free`, `+0x250` = `dlsym`, `+0x258` = `-0x1818`, `+0x270` = `mmap`.
- Slot semantics cross-checked against decompiled Mach-O call sites
  (`decompiled/src/macho/usr_lib_pulsar-appexec_usr_lib_libSystem.B.dylib/`),
  e.g. `_exit` -> `+0x28`, `_mmap` -> `+0x270`, `_reef_tlv_lookup` ->
  `+0x48/+0x50/+0x60/+0x78`, `_threadState` -> `+0x190..+0x1d0`.

## Verified

`make test` — 17 passed, 0 failed. The suite generates Mach-O fixtures
without a macOS toolchain and covers thin/fat images, both entry commands,
classic rebase and bind opcodes, chained rebases and imports, disk dylibs,
recursive dependencies, an `@rpath` framework inside an `.app`, input
rejection, `--info`, and a load-only pass over the real CotEditor closure.

`./appexec --load --runtime pulsar-appexec/usr/lib/pulsar-appexec
decompiled/app/CotEditor.app` reports **84 images loaded, 0 missing,
45,548 symbols bound, 0 unresolved, 0 errors**.

Runtime: the loader installs the host table, sets up Darwin TLS, patches all
50 `__thread_vars` descriptors (synthesised metadata plus per-descriptor
accessor trampolines), maps 84 ObjC images, initialises the runtime's
selectors and runs all 19 `__mod_init_func` initializers (libobjc, AppKit,
CoreFoundation, CoreServices, Foundation, libc++, libicu, libz, libSystem).
CoreGraphics' `_loadHost` bridge is wired through host slot `0x250`, which
loads `host/libpulsar-app-host.so` on demand with the vendored
`compat/libpulsar-toolkit*.so` preloaded; host slot `0x1b0` honours the
runtime's recursive-mutex flag.

Host slots added for the runtime: `0x150` `pthread_self`, `0x218` host
`dlsym`, `0x220` `__dyld_objc_notify_register`, `0x240`/`0x248` section data,
`0x100` `open` (Darwin->Linux flag translation), `0x298` `stat` (Darwin
`struct stat` layout), `0x120` `clock_getres`.

Known gap: the app's own AppKit path shows no window yet. (The optional
Mayonnaise backend does; see the end of this file.) The loader and host ABI
now carry the app
into its event loop instead of faulting: the full closure maps, all
initializers run, and the process stays alive with no unresolved symbols and
no missing host functions. The run loop is the vendor
`cutefish::base::EventLoop` in `compat/libpulsar-toolkit-base.so`.

Slots added while chasing that path: `0x138`/`0x5a8` `pthread_create`, `0x148`
`pthread_detach`, `0x188` `sched_yield`, `0x158` `__thread_selfid`, `0x3d8`
`poll`, `0x3f0..0x420` epoll/eventfd/timerfd, `0x358` `opendir` plus `0x380`
`dirfd`, `0x3c8` `sysconf`, `0x4d8` `getpwuid_r`/`getpwnam_r`, and `0x120`
was corrected to `clock_gettime(id, &sec, &nsec)`. Slot `0x250`
(`_reef_host_symbol`) now splits colon-separated sonames and falls back to the
shim table, which is how libicucore's stubs find the ICU entry points; host
`dlsym` (slot `0x218`) falls back to the loaded images' export tries
(`libdispatch_init`, `OBJC_CLASS_$_OS_os_log`).

Remaining: the app polls its event loop at 100% CPU without presenting a
window, and the vendored Metal translator cannot load on this host because
`libLLVM.so.19.1` and `libclang-cpp.so.19.1` are absent (only LLVM 22.1 is
installed). `PLSR_HOST_TRACE=1` names any slot the runtime still asks for.

## Layout

    src/appexec.c    loader, bundle resolution, host-table installation, launch
    src/reef_host.c  host ABI table and wrappers
    src/reef_host.h  slot offsets and ABI declarations
    tests/           generated Mach-O fixtures and the loader suite
    run-app.sh       launcher (private kwin_wayland when needed)

## Status (latest)

CotEditor does not yet create its document window through its normal app
lifecycle. With `PLSR_NSWINDOW=1`, loader recovery constructs an `NSWindow`,
attaches an `NSScrollView`, clip view and the app's `EditorTextView`, then Mayo
presents it. The recovered editor frame is zero points wide, so the bridge
fills that missing width from its parent during layout. The app's real archived
`NSMenu` tree is projected into nine host menu controls; menu entries retain
their archived titles, source `NSMenuItem` objects and target/action dispatch.
The latest trace reports 109 labels, 34 separators, nine menus, one editor and
no source buttons. The host shows the editor surface with a gutter and line
number. A manual runtime self-check sent `CotEditor` through the editor action
bridge and read it back from both the source text view and its storage with the
default compatibility path. The native toolbar is still absent: opting into
the source controller's toolbar callbacks faults in Swift because its current
`initWithWindow:` path bypasses the controller's document initializer. The
window creation path is still loader-assisted, and CotEditor toolbar/inspector
surfaces and full visual parity are not reconstructed. Broad AppKit/SwiftUI
coverage is not complete.

The prebuilt `pulsar-appexec.real` and source-built `appexec` were both launched
against this same CotEditor executable and bundled runtime. The prebuilt
process loaded CotEditor, AppKit and `libpulsar-app-host.so` and remained alive;
its captured stderr contained only missing optional Enchant spell-check
plugins. The source-built process logs its recovered window and nine
source-backed menus, but also confirms that the toolbar is absent and the
window is made by the loader recovery path. REA's native UI capture is
unavailable on this Linux host, and KWin rejected screenshot capture, so the
host window's pixels and accessibility tree could not be compared here. This
is a process/runtime comparison, not evidence of visual parity.

## Window path (host ABI)

The app's own launch path still stops before it asks for a window, so the
loader drives the shipped host window interface itself:

- `PLSR_HOST_WINDOW=1` creates and shows a 900x620 window, pumps the host loop
  for `PLSR_WIN_PRESENT_MS`, then continues into the app.
- `PLSR_HOST_WINDOW=keep` keeps pumping instead of entering the app, because
  the compositor drops the surface as soon as the loop stops.
- `PLSR_WIN_GRAB=FILE` writes the window surface through the host `grab` entry.

`reef_appkit_host_interface(0x17)` returns a table with a header word, so
interface entry i is `table[i + 1]` (0 init, 2 dispatch, 6 theme, 10 create,
12 title, 16 show, 28 grab). The `compat/*.so.1` files are redirect stubs, so
`plsr_preload_compat` loads the real `*.so.1.0` objects first and lets their
SONAMEs satisfy the host library's dependencies. The window must be created
before the app's initializers, which otherwise leave the toolkit unable to map
a surface.

Result: the host window is created and can be grabbed, but it stays blank —
the app's own storyboard/delegate path still dies before it creates a view.  A
CotEditor-shaped view tree used to be painted on top of that blank surface; it
was removed on 2026-11-20 because it made appexec look like it was running the
app when it was not.

`tests/hostwin.c` is a standalone harness that exercises the same host entry
points without Mach-O code; `tests/hostlibs` holds the redirect symlinks it
needs.

## Allocator and window findings

`_malloc_size`/`_malloc_good_size` used to resolve into libSystem.B's Darwin
libmalloc while `_malloc`/`_free` used the host allocator; a glibc block has no
Darwin zone header, so sizes were garbage. The whole malloc family now binds to
the host allocator, and the host table tracks the blocks it hands out so a
`free`/`realloc` of a foreign pointer (libobjc's pending-category map) leaks
instead of crashing. `PLSR_FREE_TRACE=1` reports those.

The remaining heap corruption needs the readdir shim (`PLSR_NO_READDIR=1`
avoids it), but a gdb breakpoint on `reef_stat_darwin` shows the SIGSEGV fires
*before* the first stat call, so the stat path is not the corrupting writer.
The dirent layout itself was audited against both consumers and is correct:
the shim's ino@0 / d_type@0x10 / name@0x11 match `_readdir$INODE64`, and the
adapter's d_type@0x14 / name@0x15 match `__CFIterateDirectory`.

`PLSR_NSWINDOW=1` swizzles `-[NSApplication finishLaunching]` and creates a real
NSWindow with an NSTextField label so AppKit paints through the host interface
instead of the blank raw surface. It reaches `finishLaunching` and `NSWindow`
init, then dies with SIGILL at an unmapped rip inside AppKit/libdispatch.

## libdispatch gate traps

`PLSR_NSWINDOW=1` used to die with SIGILL (`ud2`) in
`__dispatch_gate_broadcast_slow`. Three separate things were wrong:

- host slot `0x438` (`_syscall(0x7e000001)`, `__ulock_wait`/`__ulock_wake`)
  was an unimplemented stub; libSystem traps when that call fails with
  anything but `ENOENT`. It now maps the Darwin ulock operations onto futexes
  and returns the Darwin convention.
- `_libdispatch_init` and `_libdispatch_tsd_init` were never run on the thread
  that enters the image, so the TSD thread id and the main-queue gate started
  out inconsistent. Both now run after the image initializers, before entry.
- the gate's two diagnostic assertions still fired because the TSD block and
  the gate are touched by different threads here. `patch_dispatch_gate`
  nops the two conditional jumps (`libdispatch+0x1d404`, `+0x1d438`); the wake
  itself is correct. `PLSR_NO_GATE_PATCH=1` disables this.

The SIGILL is gone. The AppKit path now proceeds past window creation and
fails later with SIGSEGV in CoreFoundation, unrelated to libdispatch.

## Mayonnaise front-end (`PLSR_MAYO_UI=1`)

`src/mayo_ui.cpp` is the reimplementation's own window shell, drawn with the
vendored Mayonnaise toolkit in `decompiled/vendor/mayonnaise`.  It is strictly
opt-in (`PLSR_MAYO_UI=1`) and is a diagnostic view of appexec's own state, not
a CotEditor substitute: it does not draw the app's view tree.  With the switch
the loader loads and fixes up the app, installs the host shims, and then runs
the shell on the main thread instead of entering the app's Mach-O entry:

    PLSR_MAYO_UI=1 MAYO_SCREENSHOT=/tmp/ui.png ./run-app.sh ../decompiled/app/CotEditor.app

Without the switch the loader enters the app's own Mach-O entry.  That is the
runtime's goal; the shell is only a debugging window and is never the app's UI.

The shell is driven by the state the loader recovered, not by sample content:
`PLSR_APP_NAME`, `PLSR_APP_EXE`, `PLSR_APP_BUNDLE` and `PLSR_APP_SUMMARY` are
set from the bundle and the load result before the window is built.  The editor
opens the bundle's real `Contents/Info.plist`, the sidebar lists that app, its
executable, `Info.plist`, `Contents`, `MacOS` and `Resources`, and the header
shows the real closure counts (for CotEditor: `84 images, 45548 symbols, 0
unresolved`).  `appexec/mayo-ui.png` is that window.

The editor follows the sidebar: `app`/`Info.plist` open the bundle's real
`Contents/Info.plist`, the executable entry shows its bytes (a hex dump when it
is a Mach-O), and the folder entries list the corresponding bundle directory.
The state derivation lives in `src/shell_state.cpp`, so
`tests/shell_state_test.cpp` checks the sidebar, header and per-entry document
without a compositor; `make test` runs it after the loader tests.

It is the front-end of the reimplementation, not the loaded app's own view
tree; the app does not paint into it.  Without the switch the loader uses the
host surface as before.

`make` now builds the loader by default (`.DEFAULT_GOAL := appexec`); before
this fix the first target was the Mayonnaise archive, so `make` silently
skipped `src/appexec.c`.

The ObjC object guard is opt-in (`PLSR_OBJC_OBJECT_GUARD=1`).  It used to be on
by default to contain an unmapped-rip fault, which turned out to be heap
corruption from `getpwuid_r`, now fixed in the host shim (the shim never hands
caller buffers to glibc).  The guards change CF semantics, so they are off by
default; the unguarded launch gets all the way to AppKit's own
`Storyboard (Main) doesn't exist in bundle` exception.

## Painting (app view tree only)

`plsr_install_paint_hook` is opt-in (`PLSR_PAINT_HOOK=1`). The shipped app's
own launch now reaches `NSApplication` window presentation: the traced
CotEditor run constructs its document window, builds an NSView-derived node
tree, and presents it through Mayonnaise. The adapter paints a fallback node
graph only when no live widget specification is available. The graph is still
a diagnostic projection of AppKit labels and controls; it is not CotEditor's
original SwiftUI graph. The engine source and the recovered app-host interface
reference now live under `src/swiftui/`.

`PLSR_LEGACY_PAINT` and `PLSR_NO_PAINT_HOOK` are gone; use `PLSR_PAINT_HOOK=1`
to install the pass-through hook while debugging.

## Earlier launch failures (historical)

The following `Storyboard (Main) doesn't exist in bundle` and CFBundle heap
corruption notes describe older runs and are superseded by the latest trace:
the app now reads `Main.storyboardc/Info.plist`, decodes the menu archive,
constructs the document window, and presents the editor through Mayonnaise.
The current remaining SwiftUI gap is fidelity and behavior: CotEditor's full
AppKit view hierarchy is not represented by the SwiftUI engine, and text
editing needs live bridging through the real NSTextView.

## Earlier CFBundle heap corruption (historical)

`stat`'s legacy adapter only has the 120-byte Darwin struct; the shim used to
write 144 bytes there, overrunning its local struct. The shim now sizes the
write to the calling convention. The default (no `PLSR_NSWINDOW`) run still
crashes in `free()` from CoreFoundation on the bundle-resource path, and
`PLSR_NO_READDIR=1` still avoids it, so the remaining corruption is in the
directory enumeration path and is not yet fixed.

## Latest: generic bootstrap, TLV, archiver

`PLSR_BOOTSTRAP_DELEGATE=1` now derives the delegate class from the bundle
(`_TtC<len><module><len>AppDelegate`, `ApplicationDelegate`, `Delegate`,
`AppDelegate`; `PLSR_DELEGATE_CLASS` overrides), so the launcher is not tied to
CotEditor.

The `libSystem.B+0x4ece9` nil fault was the `%fs` TLV accessor; it is replaced
by a register-preserving `%gs`-backed accessor. Foundation's missing mutable
collection class cluster is backed by toll-free-bridged CoreFoundation
collections, `__NSKeyedCodingContainer` gets a real allocation, and
`addObject:` substitutes a real class-name string for nil. `finishEncoding`'s
`NSInvalidArchiveOperationException` (the plist serializer rejects the
archive's `NSNull` entries) is logged and swallowed, because forwarding the CF
exception object faults in libobjc.

The delegate bootstrap's `Foundation+0x5f7e1` fault was
`NSKeyedArchiver::_currentValues`: it evaluates
`[[self->_containers lastObject] _values]`, and that archiver's `_containers`
is empty, so `lastObject` is nil and the original loads from `nil+8`.  The
loader now patches `_currentValues` on this path to return the current
container's values dictionary, or to seed the table with a
`__NSKeyedCodingContainer` carrying a CF dictionary.  With that, the bootstrap
no longer faults there.

The launch now gets past the archiver, the Swift weak teardown, and the
bridged-dictionary dealloc abort. `PLSR_OBJC_OBJECT_GUARD=1` installs entry
guards on `_objc_retain`/`_objc_release`/`_objc_msgSend`, the CF key-retain
callback, CF nil `addObject:`/`removeObject:`, and CF nil release. CotEditor
now exits 0 after entry instead of crashing. It still does not create an
`NSWindow`: `PLSR_NSWINDOW=1` only swizzles
`-[NSApplication nextEventMatchingMask:]`. No app window exists, so
`window-real.png` / `compare-real.png` remain the last real-window evidence.

`appexec` links Mayonnaise (`src/mayo_ui.cpp`) for the opt-in
`PLSR_MAYO_UI=1` shell described above.  It renders appexec's own chrome from
the loaded app's state, not the app's view tree, so it is not the compatibility
layer; the earlier static placeholder was removed for that reason.

After the CF key/nil/release guards and the `_objc_msgSend` isa-range check,
CotEditor no longer crashes: it exits 0 after entry. `PLSR_NSWINDOW=1` only
swizzles `-[NSApplication nextEventMatchingMask:]`; no `NSWindow` is created.
There is therefore no loaded content view to feed into a Mayonnaise painter.

`make` is clean; `make test` is 17/17. The build still links Mayonnaise
(`decompiled/vendor/mayonnaise/build/src/libmayonnaise.a`) and the glad loader,
but the entry point is not wired to a separate UI.

## Latest: launch reaches the app entry point (2026-11-21)

The `PLSR_OBJC_OBJECT_GUARD` msgSend guard was the reason the loader stalled
before the app: its receiver check calls `_objc_getClassList`, which re-enters
libobjc through the guard. With the guard off, every framework initializer
(including `UniformTypeIdentifiers._createTypes` and AppKit
`_installConstraintBehaviour`) completes, the bundle fallback installs, the
`nextEventMatchingMask:` swizzle installs, and the launcher reaches the app's
entry point and bootstraps the delegate.

The launch then stops at `_objc_msgSend` on an object whose isa is not a mapped
class (`libobjc+0xe5d2`, e.g. isa `0x6a806de3cb416cd8`). That is the exact case
the guard was written for, but the guard cannot run during loader-side class
work. `PLSR_GUARD_LATE=1` installs it only after the swizzle, just before the
entry jump; with that switch the guard rejects the garbage object, but its
implementation lookup still faults inside the loader, so it is a diagnostic,
not a working configuration.

New switches: `PLSR_SKIP_INIT=substr,...` skips initializers of matching
images; `PLSR_NO_BUNDLE_FALLBACK`, `PLSR_NO_BUNDLE_PROBE`,
`PLSR_NO_ARCHIVER_PATCH`, `PLSR_NO_NSCOLOR_PATCH`, `PLSR_NO_NSWINDOW_TRACE`,
`PLSR_NO_EXC_PATCH` isolate the post-initializer patches.

`plsr_class_object()` reads `_OBJC_CLASS_$_<Name>` from the image export
tables instead of `_objc_getClass`, which deadlocks when the guard is active.
`patch_defaults_publisher_observers` now falls back to replacing
`-observeValueForKeyPath:ofObject:change:context:` on `NSObject` when the
Combine subscription class is not in the class list; that removes the
`NSInternalInconsistencyException` for `encodingList`.

The unguarded full launch (no `PLSR_SKIP_INIT`) reaches the entry point and
bootstraps the delegate, then faults in `_objc_msgSend` on a garbage isa
(`libobjc+0xe5d2`). The guard now contains that fault without libobjc
recursion:

- `plsr_msg_send_gate` checks the isa against the loaded image ranges (a
  static, sorted table) and returns the isa or nil. It never calls libobjc.
- The SIGSEGV handler resumes at libobjc's nil-return epilogue (`+0xe70c`)
  when the faulting rip is `+0xe5d2`, which covers callers that reach the slow
  path without passing the patched entry.

The next failure was `NSInvalidArgumentException` (`-[__NSCFArray length]`)
from `libswiftFoundation` during launch. `patch_cf_array_add_nil` now installs
`-length` (CF count) and a bounds-checked `-objectAtIndex:` on `__NSCFArray`,
which removes that abort and the earlier empty-array `NSRangeException`.

After those fixes the launch still stops before the event loop in a Swift
metadata fault: `libswiftCore+0x3934b2` with selector `length`, called from
`libswiftFoundation+0x6b243` (`keyPathsForValuesAffectingValueForKey:`) and
`Foundation+0x11c6fb`. With `PLSR_STORYBOARD_NOOP=1` (make
`_AKLoadMainStoryboard` a no-op) the entry path instead faults in a tagged
pointer send. `PLSR_SKIP_PRESENT_WINDOWS=1` and `PLSR_SKIP_DELEGATE_LAUNCH=1`
isolate the `[NSApplication windows]` probe and the delegate launch callback.

This block records an older failure state. The latest trace creates and
presents the document window; see the current status above.

Reproduction (guard off, full launch):

    PLSR_REEF_TRACE=1 PLSR_NSWINDOW=1 PLSR_BOOTSTRAP_DELEGATE=1 \
      PLSR_PATCH_ARCHIVER=1 PLSR_SWIFT_SKIP_SIDETABLE=1 \
      PLSR_SWIFT_IGNORE_DEALLOC_COUNT=1 \
      ./run-app.sh ../decompiled/app/CotEditor.app
