# Progress

Persistent reverse-engineering workspace for `pulsar-appexec` and the test app
`CotEditor 7.1.3`.

## Milestone: launch straight from the .dmg (DONE)

`src/runner/dmgmount.c` gives `pulsar-run` a userspace mount for Apple disk
images, so the launcher can run the app inside a `.dmg` without a prior
extraction step:

    ./run-coteditor.sh --dmg ../CotEditor_7.1.3.dmg

- `--app X.dmg` (or a positional `.dmg`) is detected by suffix and mounted
  under `$XDG_CACHE_HOME/pulsar-dmg` as `<stem>-<size>-<mtime>`; `PULSAR_DMG_CACHE`
  overrides the root and `PULSAR_7Z` the extractor.
- Extraction is `7z x` into a `.partial.<pid>` sibling, nested `.apfs`/`.hfs`/
  `.img` payloads are unpacked if the extractor stopped at the container, then
  the directory is renamed into place with a `.plsr-mounted` marker. Interrupted
  or partial mounts are wiped and redone; a completed mount is reused.
- The `.app` is taken from the volume root (or one directory down), must carry
  `Contents/Info.plist` or `Contents/MacOS`, and is matched against the image
  name when several bundles are present.
- `--mount-only` prints the resolved bundle path on stdout and exits without a
  compositor, which is what `tests/dmg-mount.sh` checks. The existing smoke test
  runs against an image via `PULSAR_APP=../CotEditor_7.1.3.dmg`.

The host kernel still cannot attach the image; this is the same read-only
extraction model the package's `pulsar-dmg-viewer` uses, presented as a mount.

## Milestone: test app runs (DONE)

`CotEditor.app` launches through the shipped `appexec` loader and renders its
window. Verified with a nested KWin virtual compositor because this machine's
session is X11-only (`DISPLAY=:0.0`, no `WAYLAND_DISPLAY`).

Evidence:

- `app/CotEditor.app/Contents/MacOS/CotEditor` process alive, no stderr
- screenshot of the running editor: `/tmp/shot.png`
- launch path: `runtime/bin/appexec app/CotEditor.app`
- `tests/coteditor-smoke.sh` PASS: private compositor, CotEditor pid found,
  1600x1000 screenshot with real content, no toolkit connection error
- `pulsar-run --private-compositor` forces the private KWin path; default mode
  reuses `WAYLAND_DISPLAY` when its socket is reachable

### Why it needed fixing

The loader itself was already functional. The failure
`cutefish-toolkit: cannot connect to the Wayland compositor` comes from
`compat/libpulsar-toolkit.so.1.0`, which requires a Wayland compositor with
`cutefish_*` / `xdg_*` globals and GLES 3. This host only runs an X11 session.

Decompiled proof (`src/elf/...libpulsar-toolkit.../functions/`):

- `00216610_open.c`: `cutefish::ui::wayland::Connection::open` calls
  `wl_display_connect(NULL)` and prints exactly that error on failure
- `002166f0_openFd.c`: `Connection::openFd` uses
  `wl_display_connect_to_fd` (the "over the descriptor given" path)
- `00216310_initialize.c`: after two `wl_display_roundtrip` calls it requires
  four registry globals and otherwise prints
  `cutefish-toolkit: the compositor lacks a required global`; it then creates
  `DataDevice`, `PrimarySelection`, and `TextInput` from `wl_seat`
- `002186e0_global.c` (`ConnectionListeners::global`) maps those four required
  slots to `wl_compositor`, `xdg_wm_base`, `wp_viewporter`, and
  `wp_fractional_scale_manager_v1` (offsets 0x38/0x58/0x60/0x68). KWin supplies
  all four, which is why the virtual compositor path works.

Reproduction:

```sh
export XDG_RUNTIME_DIR=/run/user/1000
kwin_wayland --virtual --socket wayland-test --width 1600 --height 1000 &
export WAYLAND_DISPLAY=wayland-test
./run-coteditor.sh
```

## Loader facts recovered from decompiled `main` (0x112f80)

- Runtime root is resolved from `/proc/self/exe`, then
  `<bindir>/../lib/pulsarappexec` (0x001147xx).
- A fallback root exists at `$HOME/.local/share/pulsarappexec/bin`.
- The package wrapper sets
  `LD_LIBRARY_PATH=<root>/compat:<root>/host:<root>/lib`.
- Bundle mode reads `Contents/Info.plist`, then `Contents/MacOS`.
- Invalid inputs are rejected before any mapping (`not a Mach-O file`,
  `not a 64-bit Mach-O file`, `not position independent`,
  `no segment maps the Mach-O header`).

## Decompilation inventory

Ghidra 12.2, output is pseudocode reference material, not buildable source.

### ELF (13 shipped objects, 10 done)

| Object | Functions | Status |
|---|---:|---|
| `usr/bin/pulsar-appexec.real` | 1249 | DONE |
| `usr/lib/pulsar-appexec/bin/appexec` | 1249 | DONE |
| `compat/libicudata.so.76.1` | 11 | DONE |
| `compat/libjpeg.so.62.3.0` | 333 | DONE |
| `compat/libpulsar-toolkit-base.so.1.0` | 3255 | DONE (8 failed) |
| `compat/libpulsar-toolkit.so.1.0` | - | PARTIAL: 13360 files, corrupt index, no `_DONE` |
| `compat/libicui18n.so.76.1` | - | PENDING (low priority) |
| `compat/libicuuc.so.76.1` | - | PENDING (low priority) |
| `host/libpulsar-app-host.so` | 2482 | DONE (2204 ok, 278 skipped) |
| `host/libpulsar-web-host.so` | 573 | DONE |
| `lib/libpulsar-metal-msl.so` | 1002 | DONE |
| `lib/libpulsar-metal-transl.so` | 1866 | DONE (452 ok, 1414 skipped) |
| `usr/lib/pulsar-appexec/usr/bin/login` | 64 | DONE |

`logs/elf/list.txt` also names a `usr/bin/login` that does not exist in the
package; it produced the stale `FAIL rc=1` row and an empty directory and is
superseded by the resumable list below.

### Mach-O (195 objects, 29 done)

- `Accelerate.framework` 148 functions, DONE
- `Accessibility.framework` 27 functions, DONE
- `AddressBook.framework` 201 functions, DONE (137 resumed + 65 new)
- `Foundation` 7382 functions, DONE (0 failures)
- `libswiftCoreFoundation` 370 functions, DONE
- `libswiftAppKit` 1478 functions, DONE
- `AppKit` 12002 functions, DONE (1 failure)
- `libSystem.B`, `libobjc.A`, `libc++abi`, `libc++.1`, `libdispatch`,
  `CoreFoundation`, `CoreGraphics`, `QuartzCore`, `Metal` DONE
- 166 objects not started, exports already indexed in `src/macho/exports/`
- Largest pending targets: `libswiftCore` 9.9 MB, `SwiftUI` 7.1 MB,
  `AppKit` 5.5 MB, `libswiftFoundation` 5.3 MB, `Foundation` 2.9 MB

### Full-inventory batch (running)

`ghidra/decompile-everything.sh` replaces the ad-hoc lists: it enumerates every
shipped Mach-O (core images first, then the rest), decompiles to one `.c` per
function, replaces the object's `_SUMMARY.tsv` row instead of duplicating it,
and writes `_DONE` at the end. `tools/re_status.py` joins the shipped inventory
with the markers into `re/STATUS.md` and `re/missing.txt`.

Current state: 29/195 Mach-O and 10/13 ELF objects have `_DONE`; the
`plsr-everything.service` unit is processing `libswiftCore`, then `SwiftUI`
and the remaining Mach-O images, then the untouched ELF objects (`libicuuc`,
`libicui18n`, `libpulsar-toolkit`). A second session,
`plsr-worker3.service`, works through the 76 `usr/lib` Mach-O images with
`GHIDRA_HEADLESS_MAXMEM=1200M` and writes `src/macho/_SUMMARY.worker3.tsv`;
both sessions are resumable and skip objects that already have `_DONE`.

Output quality check (Foundation): Ghidra recovers Objective-C selectors from
the metadata, so functions carry real names such as
`NSExpression::expressionValueWithObject:context_`; 4351 of the 7382 functions
reference `objc_msgSend`. The output is usable as a class-by-class reference
for the later clean-room port, not just as anonymous pseudocode.

    systemctl --user status plsr-everything
    tail -f decompiled/logs/macho/everything.log   # logs/elf/everything.log for ELF
    python3 decompiled/tools/re_status.py

### Live CotEditor dependency closure

Captured from `/proc/<CotEditor>/maps` while the editor was open:

- 82 Mach-O images from the package (list: `logs/macho/high.txt`)
- 8 ELF images: ICU trio, libjpeg, both toolkit libraries, app host
- Biggest closure members: `libswiftCore`, `SwiftUI`, `AppKit`, `libswiftFoundation`,
  `Foundation`, `CoreFoundation`, `libc++.1`, `Metal`, `QuartzCore`, `libSystem.B`

Priority is now non-Wayland high value: host ELF (`app-host`) -> Metal
translation -> core Mach-O shims (`libSystem.B`, `libobjc`, `libc++`,
`libdispatch`) -> UI stack (`CoreFoundation`, `Foundation`, `QuartzCore`,
`CoreGraphics`, `Metal`, `AppKit`, `SwiftUI`, `libswiftCore`). The Wayland
toolkit, ICU, Tcl, and unloaded frameworks are deferred.

### Verified behaviour (conformance)

`conformance/report.json`: 9/14 spec-derived expectations met. The five
differing cases are fixture/expectation gaps listed in `roadmap.md`.

## Clean-room runtime (new)

`src/cleanroom` builds four host libraries that `plsr-lite --shim-lib` loads
in place of the shipped ELF host:

| Library | Contents |
|---|---|
| `libplsr-libsystem.so` | Darwin-spelled libc/BSD/math/malloc/machine exports (`_strtoul`, `_snprintf`, `_gettimeofday`, `___error`, `_environ`, `_sysctl`, `_mach_absolute_time`, `_os_log_*`, `_NSGet*`, `_asprintf_l`, `_copyfile`, ...), the Blocks runtime (`__Block_copy/release`), Darwin→Linux translations for `open`/`stat`/`mmap`/`fcntl`/`copyfile`, and pthread handles/mutexes |
| `libplsr-objc.so` | class registry, `__objc_classlist`/`__objc_catlist` scanner, plain and relative method lists, `objc_msgSend`/`Super2`/`stret` trampolines, selectors, property and ARC entry points, method/ivar/property/protocol introspection, associations, weak references, exception and class-pair entry points |
| `libplsr-foundation.so` | `NSObject`, `NSConstantString`, `NSString`/`NSMutableString`, `NSNumber`, `NSArray`/`NSMutableArray`, `NSDictionary`, `NSNull`, `NSData`, `NSLog`/`NSLogv`, Foundation path helpers, reflection helpers, geometry (`NSRect`/`NSPoint`/`NSSize`/ranges/edge insets), version/error/cookie/URL constants, `NSSearchPathForDirectoriesInDomains` |
| `libplsr-dispatch.so` | serial queues, sync/async, `dispatch_once`, semaphores, groups, `dispatch_apply`, block objects, data objects, `dispatch_after`, queue-specific values, source/IO entry points (handlers stored, no kernel events yet) |

Loader additions: `--shim-lib` (dlopen plus the `plsr_host_imports` install-name
manifest) and a post-fixup ObjC scan hook (`plsr_objc_scan_image`).

Verified by `make -C src/cleanroom test`:

- a generated Mach-O importing `_strtoul` executes through the libSystem shim
  and fails to load without it;
- a generated Mach-O carrying a real `MyWorker : NSObject` class with a
  relative method list plus a category runs `objc_getClass` -> `+new` ->
  `-greet` -> `-UTF8String` -> `-shout` (category) -> `strcmp` and exits 0;
- a generated Mach-O drives `+[NSString stringWithUTF8String:]`, an
  `NSArray`, `NSNumber`, `NSDictionary` and `NSLog` through the Foundation
  remake and exits 0.

Implemented since: Darwin→Linux translations for `open`/`openat`/`creat`,
`stat`/`lstat`/`fstat`/`fstatat` (including `$INODE64`), `mmap`/`munmap`/
`mprotect`/`msync`, `fcntl` descriptor flags, pthread create/join/detach/self/
once and mutexes; a serial libdispatch slice with block/data/after/specific
support; the Blocks runtime; ObjC introspection, associations, weak references
and exception/class-pair entry points; and the Foundation constant/geometry/
`NSLogv` layer.

Not implemented yet: 86 Foundation classes (starting with
`NSAppleEventDescriptor`, `NSBundle`, `NSFileManager`, `NSURL`,
`NSNotificationCenter`, `NSUserDefaults`, `NSThread`, `NSProcessInfo`), six
metaclasses, 14 CommonCrypto entry points, and the `_reef_*` / `_Unwind_*`
bridges. CoreFoundation, AppKit and the Swift runtime remain separate
milestones.

### SwiftUI framework ABI scaffold (new)

`src/cleanroom/swiftui` generates `libplsr-swiftui.so` from the recovered
export lists. It currently covers 14,076 generated entries across 13 SwiftUI
family install names (`SwiftUI`, `_StoreKit_SwiftUI`, `_AppIntents_SwiftUI`,
`_AppIntents_AppKit`, `_GeoToolbox_AppIntents`,
`_UserNotifications_AppIntents`, `AppIntents`, `StoreKit`, `Charts`,
`WidgetKit`, `Intents`, `libswiftObservation`, and `libswiftIntents`). The
library also contains an independent text/button/stack/window node tree and a
manifest API.

This is an ABI scaffold, not behavior: generated Swift functions return zero
and metadata/descriptor/witness-table exports are zeroed objects. It is not
wired into the CotEditor launch yet. The loader path with the shim installed
reports 72 images, 0 missing, 29,886 bound symbols, 0 unresolved, and 0 errors
(the 13 replacements collapse the 80-image shipped closure).

The loader now registers every image's Swift metadata with `libswiftCore`
after fixups and code protection: `__swift5_types` (931 records in CotEditor),
`__swift5_protos` (51), and `__swift5_proto` (1,722), plus the same sections in
SwiftUI, Combine, Foundation, AppKit, and the runtime libraries. This removed
the former null-metadata crash at `libswiftCore +0x32d390` (`getGenericArgs`).
Execution now proceeds into Swift object teardown and faults at
`libswiftCore +0x3413eb` in `doDecrementSlow` (`swift_weak` path), exiting 139
before a window exists. Full SwiftUI hosting is still not verified.

### Measured execution gap (CotEditor)

`plsr-lite --load --trace-missing` over the full closure with the four shims
reports 80 images loaded, 0 missing (`libswiftQuickLookUI.dylib` is an
optional stub and is not counted), 39,646 symbols bound, 0 unresolved, 0
errors.
The unique gap fell from 483 -> 248 -> 137 as the runtime and Foundation
layers landed. The remaining names are 86 `_OBJC_CLASS_$_*`, 6
`_OBJC_METACLASS_$_*`, 14 `_CC*`, and the `_reef_*`/`_Unwind_*`/`__NSFormat*`
bridges. Execution now stops at the first missing class,
`_OBJC_CLASS_$_NSAppleEventDescriptor`, rather than at a Foundation constant.

The full sweep is reproducible with:

    decompiled/src/plsr-lite/plsr-lite --load --trace-missing \
      --shim-lib decompiled/src/cleanroom/build/libplsr-libsystem.so \
      --shim-lib decompiled/src/cleanroom/build/libplsr-objc.so \
      --shim-lib decompiled/src/cleanroom/build/libplsr-foundation.so \
      --shim-lib decompiled/src/cleanroom/build/libplsr-dispatch.so \
      --runtime pulsar-appexec/usr/lib/pulsar-appexec \
      decompiled/app/CotEditor.app

## 2026-11-20 — full-coverage decompilation + Mayonnaise UI shell

- The remaining objects are being decompiled as resumable user units:
  `plsr-everything.service` walks every Mach-O (core images first) and
  `plsr-elf.service` starts when it finishes and takes the untouched ELF
  objects (`libicuuc`, `libicui18n`, `libpulsar-toolkit`). Coverage moved from
  88/195 to 110/195 Mach-O during the session; each object writes `_DONE`.
- `appexec`'s `make` never rebuilt the loader: the Makefile's first target is
  the Mayonnaise static library, so the default goal was the `.a` and edits to
  `src/appexec.c` were ignored. Added `.DEFAULT_GOAL := appexec`.
- `PLSR_MAYO_UI=1` now runs the vendored Mayonnaise front-end
  (`appexec/src/mayo_ui.cpp`) instead of entering the app: a real drawn window
  with toolbar, sidebar, line numbers and syntax-coloured editor. Verified with
  `MAYO_SCREENSHOT=/tmp/mayo-ui.png` (1300x900, content present). It is the
  reimplementation's own shell, not the app's view tree, and stays opt-in.
- The ObjC object guard is on by default (`PLSR_NO_OBJC_OBJECT_GUARD=1` opts
  out). Without it the default launch SIGSEGVs at an unmapped rip immediately
  after entry; with it the process exits 0 with no signal.
- Still open: the app creates no window and `applicationDidFinishLaunching:`
  never runs. The guards contain that failure, they do not fix its cause (Swift
  metadata registration).

## 2026-11-21 — clean-room AppKit slice (`libplsr-appkit.so`)

The runtime's missing host library is no longer a stub. `src/cleanroom/appkit.c`
implements the AppKit object graph the decompiled `AppKit` (12,002 functions)
shows the app using, and registers every class with libplsr-objc so a Mach-O
that imports `_OBJC_CLASS_$_NSWindow` binds to it:

- `NSApplication`: `sharedApplication` singleton (`_NSApp`), delegate,
  `mainMenu`, `finishLaunching` (calls `applicationDidFinishLaunching:`),
  `run`, `windows`, `keyWindow`, `sendAction:to:from:`, event poll stub.
- `NSWindow`: `initWithContentRect:styleMask:backing:defer:`, content view and
  content view controller, title, `makeKeyAndOrderFront:`/`orderFront:`/
  `orderOut:`, `isVisible`, toolbar, window controller.
- `NSView`: frame, subviews, `addSubview:`/`removeFromSuperview`, superview,
  window, hidden, no-op display hooks. `NSScrollView` carries `documentView`.
- `NSButton`: `buttonWithTitle:target:action:`, title/target/action,
  `performClick:` (dispatches the target's action), enabled, state.
- `NSTextField`: `labelWithString:`, `textFieldWithString:`, `stringValue`,
  placeholder, editable. `NSTextView`: `string`/`setString:`/`insertText:`.
- `NSMenu`/`NSMenuItem` and `NSToolbar`/`NSToolbarItem`, plus
  `NSApplicationMain` (shared app -> finishLaunching -> run).

Evidence: `make -C src/cleanroom` builds `build/libplsr-appkit.so` (123 KB);
`tests/test_appkit.c` drives it through `objc_getClass` +
`plsr_objc_lookup_imp` and passes 20 checks (`PASS: AppKit slice`), and the
full `sh tests/run.sh` stays green (libSystem, ObjC, Foundation, Darwin
open/stat/mmap, dispatch, AppKit).

Limit: this is the object graph and control behaviour, not a renderer — no
drawing, layout, event delivery, text input, nib/storyboard decoding or
CoreAnimation layer backing yet, and it is not wired into the CotEditor launch
path.

## 2026-11-21 — nib/storyboard decoding (`nib.c`)

A shipped app now builds its own window and menus from its shipped resources
instead of painted placeholders. `src/cleanroom/nib.c` decodes ibtool's
**NIBArchive** format (class table, key table, value table, object table of
`(class, first value, count)`) and instantiates the object graph through the
registered ObjC classes:

- strings/numbers/data/values, arrays, sets, dictionaries, attributed strings;
- `NSMenu`/`NSMenuItem` including `NSName` bindings (`_NSMainMenu`,
  `_NSWindowsMenu`, `_NSServicesMenu`, `_NSHelpMenu`, `_NSAppleMenu`);
- `NSWindowTemplate` -> real `NSWindow` with title, style mask, content view and
  toolbar;
- views (`NSView`, `NSScrollView`/`NSClipView`, `NSBox`, `NSStackView`,
  `NSSegmentedControl`, `NSImageView`, `NSSwitch`, `NSScroller`, `NSTableView`,
  `NSOutlineView`, `NSTableHeaderView`) with frames, subviews, scroll document
  views and cell contents applied as button titles / field string values;
- class swappers resolve to the app's class and fall back to
  `NSOriginalClassName`, so Swift subclasses such as `CotEditor.AppDelegate`,
  `ControlUI.OptionalMenu` and `CotEditor.HeadingMenuItem` still build when the
  Swift runtime is absent;
- `NSNib`/`NSBundle loadNibNamed:owner:topLevelObjects:`, `NSStoryboard`
  (`storyboardWithName:bundle:`, initial controller, identifier lookup, main
  menu) and `NSViewController` (`nibName`, lazy `view` loading from the view
  nib, `setView:` outlets).

Connectors are applied after the graph exists: `NSNibOutletConnector` becomes
`set<Label>:`, `NSNibControlConnector` becomes `setTarget:`/`setAction:`.
`NSApplicationMain` reads `NSMainStoryboardFile` (or `NSMainNibFile`) from the
bundle and loads the main-menu nib with `NSApp` as owner, which is how the
application delegate and the menu bar arrive.

Evidence: `tests/test_nib.c` decodes the shipped CotEditor resources and passes
29 checks — the main storyboard's 10 top-level menus with items, key equivalents
and connector-applied actions (`File > New` = `n`, `newDocument:`), the
PrintPanelAccessory scene's `NSViewController` and its view nib (view tree with
text-field contents), and Sparkle's `SUUpdateAlert.nib` producing an `NSWindow`
with its content view. `sh tests/run.sh` stays green.

Views keep the autoresizing mask ibtool packs into `NSvFlags` (low six bits),
so decoded frames re-layout through `NSView` when their window or content view
resizes; the window sizes its content view to its content rect on install.

Limits: decoding only (no nib writing), no constraint solving; segue execution
and controller presentation are covered by the 2026-11-22 section below.

## 2026-11-21 — NSView layout, drawing and event delivery (host surface)

The AppKit slice now lays out, draws and handles input instead of only holding
the object graph. `src/cleanroom/appkit_surface.h` is a small C ABI
(`plsr_surface_new/free/resize/attach_window/layout/render/hit_test/mouse/key/`
`write_ppm/ink_pixels/distinct_colors`) over a software RGBA surface; any host
(Wayland, a test harness, appexec) can own the pixels and forward input.

- **Layout**: `NSView` frames with autoresizing masks (min/width/max on both
  axes), `autoresizesSubviews` defaulting on, `setFrame:`/`setFrameSize:`/
  `setBounds:` propagating through the tree; `NSWindow setContentSize:` and
  surface resize re-layout the content view; `NSBox`/`NSStackView` layout their
  children; `layoutIfNeeded`/`display` mark and flush dirty state.
- **Drawing**: `drawRect:` runs against the surface with AppKit's bottom-left
  coordinates converted at the ABI boundary; `NSRectFill`, `NSFrameRect`,
  `NSColor` (named colours, `setFill`, `setStroke`), the 5x7 bitmap font shared
  with the SwiftUI text engine, and per-class drawing for buttons, labels, text
  fields/views, boxes and stack views. Only dirty views are redrawn; `render`
  clears the flag and `write_ppm` dumps the frame for inspection.
- **Events**: `NSEvent` mouse/key objects, hit testing through subviews (deepest
  visible view wins, boxes fall through to their controls), first responder and
  `makeFirstResponder:`/`acceptsFirstResponder`, mouse down/up/dragged to the
  hit view, button target/action on mouse-up inside the control, text-field and
  text-view editing (characters, delete), and Command-key menu equivalents that
  walk the main menu and send the item's action to its target.

Evidence: `tests/test_surface.c` builds a window with a box, button and text
field, then passes 22 checks — layout follows the surface size, a width-sizable
button absorbs a 120px resize while a fixed field keeps its size, hit testing
finds the button through the box, `drawRect:` produces a multi-colour 480x320
frame (PPM written), mouse down/up fires the button action, clicking focuses the
field and typing/delete edits it, and Command-N reaches the menu item's target.
`sh tests/run.sh` is green across all eight steps.

Limits: the surface is a software buffer with no hardware window, no IME/native
text input, no tracking areas, scrolling or layer backing, and drawing is a
minimal immediate-mode subset (no CoreGraphics paths/gradients, no text shaping
beyond the bitmap font).

## 2026-11-21 — CoreFoundation slice (`corefoundation.c`)

The runtime stack now has a clean-room `libplsr-corefoundation.so`, the layer
between libSystem and Foundation. It is built on toll-free bridging: a
`CFStringRef` is an `NSString`/`NSMutableString`, `CFArrayRef` an
`NSArray`/`NSMutableArray`, `CFDictionaryRef` an
`NSDictionary`/`NSMutableDictionary`, `CFNumberRef` an `NSNumber` and
`CFNullRef` the `NSNull` singleton, so values cross the CF/NS boundary with no
conversion. `CFBooleanRef`, `CFDataRef`, `CFSetRef`, `CFUUIDRef`, `CFURLRef`,
`CFRunLoop` and `CFNotificationCenter` have their own storage and classes
(`__NSCFBoolean`, `__NSCFData`, `__NSCFSet`, ...).

Covered: retain/release/equal/hash/description and type IDs; strings (C, bytes
and UTF-16 creation, copies, mutable append/insert/delete/replace, compare,
find, prefix/suffix, case conversion, numeric parsing, format); arrays and
dictionaries (create/copy/mutable, mutation, lookup, key/value enumeration);
numbers and booleans; data (append, resize, replace, delete); sets; UUID; URLs
(file paths, appending/deleting components); bundles (main bundle, identifier,
executable and resource URLs through `NSBundle`); a single-threaded run loop
with timers, observers and sources plus stop/timeout/finished results; a local
notification center; and an in-memory preference store.

`CFSTR()` works too: `___CFConstantStringClassReference` is a real
`__NSCFConstantString` class that reads the Darwin
`{ isa, flags, length, cstr }` layout and compares equal to ordinary
`NSString`s.

This also fixed Foundation: `NSMutableDictionary` was never registered, so
`objc_getClass("NSMutableDictionary")` returned nil and mutable-dictionary code
silently fell back to an immutable `NSDictionary`. It is now in
`FOUNDATION_CLASSES` with `NSDictionary` as its superclass.

Evidence: `tests/test_corefoundation.c` passes every check — strings,
collections, data/set growth, UUID/URL, the CotEditor bundle identifier and its
`Contents/MacOS/CotEditor` executable URL, a repeating timer that stops the run
loop after two fires, observer entry/exit, notification delivery, preference
round-trips, distinct type IDs, and CFSTR constants. `sh tests/run.sh` is green
across all nine steps.

Limits: no CFNetwork/CFSocket/CFStream, no on-disk CFPreferences or
CFPropertyList, no CFMachPort/CFMessagePort, run loops are single-threaded and
not integrated with libdispatch sources, and CFNumber type reporting is a
value-based heuristic.

## 2026-11-22 — storyboard segues and controller presentation

`NSViewController` now presents and dismisses real scene controllers, and
`NSStoryboardSegue` executes. `presentViewControllerAsModalWindow:`,
`presentViewControllerAsSheet:` and
`presentViewController:asPopoverRelativeToRect:ofView:preferredEdge:behavior:`
create an `NSWindow` sized to the destination controller's view, install the
view as its content view and the controller as its content view controller,
call `viewWillAppear`/`viewDidAppear` when implemented, order the window front
and record the relationship (`presentedViewControllers`,
`presentingViewController`). `dismissViewController:` orders the window out and
clears the relationship; `dismissController:` dismisses through the presenter.
Sheets and popovers currently use the same window path — their chrome is not
drawn by the software surface.

Segue execution: `NSStoryboardSegue`
(`segueWithIdentifier:source:destination:performHandler:`, `identifier`,
`sourceController`, `destinationController`, `perform`) presents the destination
through its source. `performSegueWithIdentifier:sender:` consults the
`NSStoryboardSegueTemplate` objects decoded from the scene nib (identifier,
`NSSource`/`NSDestination` links, `showSeguePresentationStyle`,
`NSStoryboardSegueDestinationOptions`), runs
`shouldPerformSegueWithIdentifier:sender:`, builds the segue, calls
`prepareForSegue:sender:` and performs it. Templates are attached to their
source controller, or to the scene's only controller when no source is
archived.

Evidence: `tests/test_nib.c` now passes 46 checks. Two real scenes
(`ShortcutTreeView` and `MultipleReplaceView`) are instantiated, presented
modally, as a sheet and as a popover, dismissed, and checked for window
visibility and presenter links; an `NSStoryboardSegue` between them performs
and presents; an unknown segue identifier performs nothing;
`shouldPerformSegueWithIdentifier:` defaults to YES. `sh tests/run.sh` is green
across all nine steps.

Limits: block-based segue perform handlers are stored but not invoked (the
Blocks runtime is not wired into `NSStoryboardSegue` yet); popover/sheet chrome
and animations are not drawn; CotEditor's shipped storyboards contain no segue
templates, so template decoding itself is exercised only by the generic
archive path, not by a shipped fixture.

## 2026-11-22 — X11 host window for the surface

`tests/surface_window.c` (`make surface-window`) is the first real window for
the clean-room stack: it opens an X11 window, attaches an `NSWindow`/view tree
to a `plsr_surface`, renders through `plsr_surface_layout/render`, converts the
RGBA buffer to an XImage and blits it, re-laying out on ConfigureNotify.
Without an app bundle it builds a demo scene (box, labels, editable field, two
buttons); with one it instantiates the `PrintPanelAccessory` scene from the
bundle's storyboard. `--capture out.ppm` grabs the window's own pixels
(XGetImage) after the first expose, so the visual check does not depend on an
external screenshot tool.

Evidence: the 560x360 demo window renders 7 colours and captures to
`tests/artifacts/plsr_window.png` (labels, editable field, Run/Cancel buttons);
the real `PrintPanelAccessory` scene renders 440x381 into
`tests/artifacts/plsr_scene.png` with 25,644 ink pixels and 6 colours.

Limits: X11 only (no Wayland/Cocoa host), no window-manager chrome of its own,
and X input events are not forwarded into the surface ABI yet.

## Open unknowns

- No runtime observation inside CotEditor past first paint (no input tests).
- `libpulsar-toolkit` remains un-decompiled and owns the Wayland/EGL/Skia
  connection; deferred per current focus.
- Which `compat/libpulsar-metal-*` code paths CotEditor actually uses at idle.
- Machine reboots interrupt long Ghidra runs; batches are resumable, but
  `index.tsv` is only complete after `_DONE`.

## Reimplementation: `appexec/` (from-source loader + host ABI)

`appexec/` is a standalone reimplementation of the package loader and host
runtime, built from the decompiled ELF and the decompiled Mach-O adapters:

- `src/appexec.c` - the loader (`main` 0x112f80 / `FUN_00147190` behaviour):
  runtime root, `.app`/`Info.plist`, Mach-O validation, mapping, slide,
  classic + chained fixups, export tries, dylib closure, Swift registration,
  stack and entry jump, plus `--info/--load/--trace-missing/--shim-lib`.
- `src/reef_host.c` - the `_reef_host` ABI. The table layout and slot
  semantics were recovered from a gdb dump of the shipped loader's table
  (208 live slots) and cross-checked against the decompiled call sites in
  `src/macho/...libSystem.B.dylib/functions/`. `+0x48/+0x50/+0x58/+0x60`
  are malloc/calloc/realloc/free, `+0x250` is dlsym, `+0x258` is -0x1818,
  `+0x270` is mmap, `+0x190..+0x1d0` are the thread-state key/mutex object
  calls. Darwin thread-locals use `%gs:-0x1818`; the loader publishes a GS
  block and an empty TLV array before entry.

Verified: `make test` 17/17, and `--load` over CotEditor reports 84 images,
45,548 symbols bound, 0 unresolved, 0 errors. At runtime the loader now also
patches all 50 `__thread_vars` descriptors (synthesised metadata plus
accessor trampolines, which removed the `__tlv_bootstrap` crash), loads the
vendor host libraries, wires CoreGraphics' `_loadHost` through slot `0x250`
(with `compat/libpulsar-toolkit*.so` preloaded), honours the recursive-mutex
flag on slot `0x1b0`, and reaches `NSApplicationMain`; the app then runs its
main loop instead of faulting in the loader.

The earlier `<null selector>` / `_reef_format` crash is gone. The loader now
calls libobjc's `_mapImages` *after* `protect_image` (it was called while
`__TEXT` was still RW, so the call itself was NX), runs libobjc's
`_initializeRuntime` from `__mod_init_func` to register the `_SEL_*` globals,
installs a nil-returning `_objc_setForwardHandler` so a missing selector
cannot recurse until the stack dies, and runs every image's initializers in
dyld order (map images first, then initializers) - 19 initializers now run
(libobjc, AppKit, CoreFoundation, CoreServices, Foundation, libc++, libicu,
libz, libSystem).

The host slots the runtime actually asks for are now implemented, named by
per-slot trampolines under `PLSR_HOST_TRACE`: `0x150` pthread_self, `0x218`
host dlsym, `0x220` `__dyld_objc_notify_register`, `0x240`/`0x248` section
data, `0x100` open (Darwin->Linux flags), `0x298` stat (Darwin layout) and
`0x120` clock_getres.

Not done: a window is still not shown. The crash chain is closed, though:
the post-initializer jump to a thread stack came from `_pthread_create`
calling the then-unimplemented slot `0x138`, and the later faults were
missing ICU/locale, `getpwuid_r`, `poll`, `epoll` and `clock_gettime`
semantics. With those implemented the process stays alive, reports no
unresolved symbols and no missing host functions, and reaches the vendor
`cutefish::base::EventLoop` (`compat/libpulsar-toolkit-base.so`) through
`NSApplicationMain`.

Still open: the event loop polls at 100% CPU without presenting a window, and
`lib/libpulsar-metal-transl.so` cannot be loaded on this host because
`libLLVM.so.19.1` and `libclang-cpp.so.19.1` are not installed (the system
carries LLVM 22.1 only).

## Session: main bundle, stat/readdir ABI, delegate (still no window)

Chain found this session, in order:

1. `_NSGetExecutablePath` reported the *loader* path, so `__CFProcessPath` made
   `+[NSBundle mainBundle]` nil. `_AKLoadMainStoryboard` returned immediately and
   NSApp never got a delegate. The shim now returns the Mach-O executable path
   (`.../CotEditor.app/Contents/MacOS/CotEditor`).
2. Slot `0x298` (stat) is called with the statAt convention
   `(dirfd=AT_FDCWD, path, flags, buf)`; a two-argument shim read -2 as the path
   and failed with EPERM, so CoreFoundation saw mode 0 for the .app and refused
   to create the bundle. The stat family is now bound directly to shims that
   write Darwin's 144-byte `struct stat`; `__CFGetPathProperties` reports
   `mode=0x41ed` (directory).
3. Slot `0x360` is *readdir* (`readdir(dirp, darwin_dirent*)`), not opendir; it
   was bound to glibc `opendir`, so every directory read passed a `DIR*` as a
   path. It now fills ino@0, d_type@0x10 and name@0x11, the layout the
   `readdir$INODE64` adapters repack from.

With those three fixed, `+[NSBundle mainBundle]` is non-nil and bundle creation
plus directory listing run. Remaining blocker: once bundle creation and
directory iteration both succeed, CoreFoundation/Foundation corrupt the heap
(`free()` chunk validation) and the process dies before `finishLaunching`;
disabling directory iteration moves the fault into Foundation instead.
`applicationDidFinishLaunching:` is still never called and `hostWindowCreate`
is still never reached.

`PLSR_BOOTSTRAP_DELEGATE=1` patches `_AKLoadMainStoryboard` (AppKit+0x1b7250)
to install the app's `AppDelegate` directly, but instantiating the Swift
delegate currently faults inside Foundation.

Debug switches added: `PLSR_NO_STAT_SHIM`, `PLSR_NO_READDIR`,
`PLSR_BOOTSTRAP_DELEGATE`, `PLSR_EXEC_PATH_TRACE`, `PLSR_ENTRY_TRAP`.
`make test`: 17 passed, 0 failed.

## Session: a window actually appears (host ABI path)

The app's own path still stops before it asks for a window, so the loader now
drives the shipped host window interface directly.

New in `appexec`:
- `PLSR_HOST_WINDOW=1`  create/show a 900x620 window, pump the host loop for
  `PLSR_WIN_PRESENT_MS` (default 600) so the compositor commits the buffer,
  then continue into the app.
- `PLSR_HOST_WINDOW=keep`  same, but keep pumping instead of entering the app
  (the surface is dropped as soon as the loop stops).
- `PLSR_ENTRY_THREAD=1`  run the app entry on a second thread and pump on the
  main thread (crashes: the toolkit loop is not thread-safe).
- `PLSR_WIN_GRAB=FILE`  save the window surface through the host `grab` entry.

Findings:
- `reef_appkit_host_interface(0x17)` returns a table whose first word is a
  header (`0x17`); interface entry i is `table[i + 1]`. Entry 0 init, 2
  dispatch, 6 theme, 10 window create, 12 title, 16 show, 28 grab.
- `compat/*.so.1` are redirect stubs (text files naming the real soname), so
  the dynamic linker cannot open them. `plsr_preload_compat` loads the real
  `*.so.1.0` objects first; their SONAMEs then satisfy the host library's
  NEEDED entries.
- The window must be created before the app's Mach-O initializers run;
  afterwards the toolkit never maps the surface.
- Entry 21 (`Window::activate`) crashes the toolkit, and dispatching from a
  pthread crashes it as well.

Status: with `PLSR_HOST_WINDOW=keep` a real CotEditor window is on screen
(`appexec/window.png`) and `PLSR_WIN_GRAB` captures its 1800x1240 surface, but
the content is blank. The app does not draw into that window, and its own
storyboard/delegate path is still blocked by the CoreFoundation heap
corruption.

`appexec/tests/hostwin.c` is the standalone harness that proved the backend:
it drives init/create/title/show/grab without any Mach-O code in the process.
`appexec/tests/hostlibs` holds the redirect symlinks it needs.

## Session: why the window is blank, and the allocator fixes

The host window is a raw cutefish surface: the loader creates and shows it, but
only AppKit can paint into it (through CoreGraphics' host interface).  AppKit
never paints because the app dies first, and the crash was a cross-allocator
bug, not a windowing bug:

- `_malloc`/`_calloc`/`_realloc`/`_free` bind to the host allocator, but
  `_malloc_size`/`_malloc_good_size` still resolved into libSystem.B.dylib's
  Darwin libmalloc.  A glibc block has no Darwin zone header, so malloc_size
  read unrelated heap words.  The whole family is now bound to the host
  allocator (`shim_malloc_*` in appexec.c).
- libobjc still frees/reallocs a pointer the host allocator never produced
  (`_hashmapRemove(&_pendingCategories, cls)` in
  `_realizeClassWithoutSwiftInitializer`), which faults in glibc's chunk walk
  with size `0xff00000000000781`.  The host table now tracks every block it
  hands out and leaks unknown pointers instead of aborting
  (`plsr_malloc_tracked` / `plsr_free_checked`), with `PLSR_FREE_TRACE` to see
  the skipped ones.
- `PLSR_NO_READDIR=1` or `PLSR_NO_STAT_SHIM=1` avoids the remaining heap
  corruption; it needs both shims active, i.e. it is in the CFBundle directory
  path.  The libSystem `readdir$INODE64` adapter's buffer (~1056 bytes, name at
  +0x1d) matches the shim, so the write is not the overflow; the cause is still
  open.
- `PLSR_NSWINDOW=1` swizzles `-[NSApplication finishLaunching]` through the ObjC
  runtime (no code patching) and builds a real NSWindow plus an NSTextField
  label, so AppKit paints instead of the blank raw surface.  It reaches
  `finishLaunching` and starts `NSWindow` init, then dies with SIGILL at an
  unmapped rip inside AppKit/libdispatch.  This is the next thing to fix.

## Session: libdispatch gate trap blocks AppKit painting

`PLSR_NSWINDOW=1` swizzles `-[NSApplication nextEventMatchingMask:...]` and
creates an NSWindow plus an NSTextField from the running loop.  It reaches
`NSWindow` init and dies with SIGILL: `ud2` inside
`__dispatch_gate_broadcast_slow` (libdispatch +0x1d458), the gate-mismatch trap
(`invalidInstructionException`).  The same trap fires when the window is built
from `finishLaunching`, and neutralizing `dispatch_assert_queue*` does not
change it, so it is libdispatch's TSD/gate state rather than the window API.

Consequence: AppKit cannot paint into the host window in this runtime yet.  The
host window path still works (blank surface, `appexec/window.png`), and the
app's own CFBundle path still corrupts the heap unless stat or readdir is
disabled.

## 2026-11-20 — drawn window, libdispatch gates, stat sizing

- Host slot 0x438 (`_syscall(0x7e000001)`, `__ulock_wait/__ulock_wake`) now maps
  to futexes and returns the Darwin convention (0 / -ENOENT).
- `_libdispatch_init` + `_libdispatch_tsd_init` run after image initializers,
  before entry; `patch_dispatch_gate` nops the two gate assertion jumps
  (libdispatch+0x1d404, +0x1d438). The `ud2` SIGILL is gone.
- `plsr_install_paint_hook` replaces content-view vtable slot 32 (offset 0x100)
  and draws a header band, accent stripe and two blocks via
  `cutefish::ui::fillDeviceRect`. `PLSR_WIN_GRAB` now yields a surface with five
  distinct colours instead of uniform white; `appexec/window.png` is that grab.
- `reef_stat_darwin` sizes its Darwin struct write (120 bytes for the legacy
  adapter, 144 for statAt) and no longer mistakes AT_FDCWD for a path pointer.
- Still open: the default run crashes in `free()` from CoreFoundation while
  walking the bundle; `PLSR_NO_READDIR=1` or `PLSR_NO_STAT_SHIM=1` avoids it.

## 2026-11-20 — SwiftUI environment, state, lists and the window bridge

- `swiftui/state.c` adds a type-erased value type, a keyed `sw_environment`
  with inheritance (`colorScheme`, `font`, `foregroundColor`, `accentColor`,
  plus arbitrary keys and `sw_environment_resolve`), `sw_binding` (get/set
  closures and constants), `sw_state` (revision + global generation),
  node-local `@State` via `sw_node_state`, and `sw_observable` with watcher
  callbacks. Explicit node styling wins over inherited values through
  `font_explicit` / `foreground_explicit` / `background_explicit`.
- `sw_host_needs_render` compares the global state generation with the last
  rendered frame, so a state or observable write invalidates the host until the
  next render.
- New view kinds: `List` (fills leftover height, hairline row separators, row
  backgrounds), `ForEach` (static vertical container) and `TextField`
  (placeholder/value, rounded plate and border, focus caret).
- `sw_paint` is the host primitive adapter: the engine walks the laid-out tree
  and emits rectangles and text runs to host callbacks. `sw_hosting_view_*`
  wraps the hosting surface in the NSHostingView shape.
- appexec compiles the engine and paints the CotEditor window through it: the
  paint hook calls `sw_paint` with `fillDeviceRect` for rectangles and
  `TextLayout` for text (rectangles scaled to the 1800x1240 surface, text
  already scaled by the toolkit). `PLSR_LEGACY_PAINT=1` restores the old bands.

Evidence: `make test` in `cleanroom` passes the render test (960x640, 298,205
ink pixels, 55 distinct colours, text field 681x34, list 681x72), the new state
test (environment inheritance, dark scheme, binding/state/observable, host
invalidation, painter bridge), and the loader closure (72 images, 0 missing,
29,886 bound, 0 unresolved, 14,230 exported symbols). `appexec` builds with the
engine, keeps 17/17 loader tests green, and the host-window grab
(`PLSR_NO_READDIR=1 PLSR_HOST_WINDOW=keep PLSR_WIN_GRAB=...`) shows the
engine-rendered toolbar, sidebar list, text field and editor text.

Limit: generated Swift exports still return zero; gestures/input events,
animation, real Path geometry, scenes, GraphicsContext and ObjC class
registration remain unimplemented.

## 2026-11-20 — SwiftUI engine: view graph, layout, rasterizer, hosting

`decompiled/src/cleanroom/swiftui` now contains a behavioral SwiftUI engine
behind the generated ABI surface, not just zero-returning stubs:

- `engine.h` / `engine.c` — 15 view kinds (Group, Text, Image, Spacer,
  Divider, Rectangle, RoundedRectangle, Circle, Capsule, Path, HStack, VStack,
  ZStack, Button, Window) and the modifier set (foreground, background, fill,
  padding, frame/min/max, flex, opacity, offset, clip, corner radius, line
  limit, alignment, font, spacing). Layout is proposal based with `NAN` as
  SwiftUI's unspecified proposal; spacers and flexible children absorb the
  leftover space, padding/frame adjust the child proposal, and text wraps on
  word boundaries at the proposed width.
- `canvas.c` — RGBA software rasterizer with alpha blending, a clip stack,
  anti-aliased rounded rectangles/circles, and a zlib PNG writer.
- `text.c` — 5x7 bitmap font, metrics, wrapping and drawing; weight 600+ is
  smeared for a semibold look.
- `host.c` — `NSHostingView` equivalent: `sw_host_new/set_root/resize/render`
  and `sw_host_write_png`.
- `inventory.py` — reads the decompiled framework folder (24,470 functions,
  1,272 mangled types) and writes `generated/inventory.md` with the largest
  types and per-capability coverage.

Run evidence (`sh swiftui/tests/run.sh`): a 960x640 editor window (title bar,
toolbar with buttons, sidebar rows with a badge, heading, wrapped paragraph,
monospaced code block, footer) lays out and renders to `swiftui/render.png`:
294,563 ink pixels, 50 distinct colours, paragraph wraps to 5 lines, sidebar
230pt, editor 729pt, code block 681pt. The same run reports 14,161 exported
symbols and, through the loader with all five shims, 72 images loaded, 0
missing, 29,886 bound, 0 unresolved, 0 errors. `make test` stays green.

Honest limit: the 14,076 generated Swift exports still return zero, and
environment, state/binding, gestures, animation, List/ForEach/TextField, real
Path geometry, scenes and the AppKit hosting bridge are not implemented. The
engine is a working C core, not a complete 24,470-function port, and it is not
wired into the CotEditor launch.

## 2026-11-20 — text in the window, dirent audit

- `plsr_draw_text` drives `cutefish::ui::TextLayout` directly: default
  constructor, `setFont` with a real family, `setText` (libstdc++ string passed
  by value), `beginLayout` -> `createLine(width, PointF)` -> `endLayout` ->
  `draw(painter, PointF)`.  The `createLine` call is what makes `shape()`
  produce lines; without it `draw` silently paints nothing.
- Glyph size lives in the double at `FontSpec+0x20`; `setScale` does not change
  glyph height.  16 gives readable text.  `PLSR_TEXT_EM`, `PLSR_TEXT_PX`,
  `PLSR_TEXT_FONT`, `PLSR_TEXT_DEBUG` and `PLSR_NO_TEXT` control it.
- Verified: 29,308 non-background pixels, 33px title band, four 43px body lines.
- Dirent audit: `reef_readdir_darwin`'s input layout (ino@0, type@0x10,
  name@0x11) matches `_readdir$INODE64`, and the adapter's output (type@0x14,
  name@0x15) matches `__CFIterateDirectory`.  No change required.
- New evidence on the default crash: a gdb breakpoint on `reef_stat_darwin`
  never fires before the SIGSEGV, so the stat path is not the corrupting
  writer.  `PLSR_NO_READDIR=1` still avoids the crash.

## Session: removed the CotEditor facade; vendored QuartzCore/CoreGraphics

- Deleted the hard-coded CotEditor view tree and the demo bands from appexec.
  The paint hook is now opt-in (`PLSR_PAINT_HOOK=1`); by default the toolkit
  paints only the loaded app's own views. `make` and the 17 loader tests pass.
- Confirmed why the window looks nothing like the shipped runtime: with the
  overlay off the host window is blank, and the app's own launch crashes before
  it creates a window — Foundation+0x5f7e1 with `PLSR_BOOTSTRAP_DELEGATE`, and
  Sparkle+0x43db8 with `PLSR_NSWINDOW`. Live-desktop screenshots show no
  CotEditor window in either mode, so there is no hidden real UI to compare.
- Vendored pearOS sources: `decompiled/vendor/QuartzCore` (Core Animation; needs
  GNUstep/libobjc2, CoreGraphics, AppKit and its wsd window server) and
  `decompiled/vendor/CoreGraphics` (placeholder; upstream ravynos CoreGraphics
  and Onyx2D). The host has no objc development headers or `gnustep-config`,
  and the pearOS AppKit targets wsd, so this stack is not buildable here yet
  and is not wired into the Mach-O host path.

## Session: TLV, generic delegate, archiver, and the remaining Swift fault

The hard-coded CotEditor facade is gone and the bootstrap is generic:
`PLSR_BOOTSTRAP_DELEGATE=1` derives the Swift module from the bundle
executable (`PLSR_APP_BUNDLE` or the Mach-O name) and tries
`_TtC<len><module><len>AppDelegate`, `ApplicationDelegate`, `Delegate`, then
plain `AppDelegate`; `PLSR_DELEGATE_CLASS` overrides it. The old
`PLSR_APP_BUNDLE=CotEditor` assumption is no longer required.

The deterministic `libSystem.B+0x4ece9` nil fault was the Darwin TLV accessor:
`_reef_tlv_get_addr` at libSystem.B+0x4ecd0 starts with the `%fs` prefix
(`51 48 8b 0d 18 80 01 00 64`) and reads `%fs:-0x1818`, while the loader
publishes `%gs:-0x1818`. The accessor is replaced with an assembly shim
(`plsr_tlv_get_addr_asm`) that preserves every register except `rax` (including
`rsi`/`rdx`, which `_dispatch_once_f` relies on) and calls a per-thread C
helper. That removed the libSystem fault and the libdispatch gate/`ud2`
follow-on.

The next blocker was `NSKeyedArchiver`. Foundation's class cluster has no
`NSMutableArray`/`NSMutableDictionary`/`NSMutableSet` implementations here:
`+alloc`/`init`/`initWithObjects:`/`initWithCapacity:` return nil, so
`_containers`/`_objects` stay nil. The loader now backs those methods with
toll-free-bridged `CFArrayCreateMutable`/`CFDictionaryCreateMutable`/
`CFSetCreateMutable` objects and `__NSCFArray`/`__NSCFDictionary`/`__NSCFSet`
methods; `__NSKeyedCodingContainer` gets `class_createInstance(cls, 8)` and
identity retain/autorelease so the archiver's UID objects survive.

`_uidForClass:` then appended `NSStringFromClass(cls)` for a class whose
`class_getName` is null (a Swift/Sparkle class), and immediately read
`objectAtIndex:0` from the resulting empty class-name array. `addObject:` now
substitutes a real `"UnknownClass"` string (via `NSString` or
`CFStringCreateWithCString`) instead of dropping the nil; the empty-array
`NSRangeException` is gone. `__NSCFDictionary initWithObjects:forKeys:count:`
and `initWithObjects:forKeys:` were added over `CFDictionarySetValue`.

`finishEncoding` still failed with
`NSInvalidArchiveOperationException: *** finishEncoding: (null)` because the
archive's object table contains non-plist objects (`NSNull`) and
`NSPropertyListSerialization` returns nil. The loader installs a narrow
`+[NSException raise:format:]` wrapper that logs and swallows that exception
(it never calls the original, because forwarding the CF exception object
faults in libobjc). Sparkle's state save is therefore skipped, not fixed.

Current launch status with
`PLSR_BOOTSTRAP_DELEGATE=1 PLSR_NO_READDIR=1 PLSR_PATCH_ARCHIVER=1
PLSR_EXC_TRACE=1 PLSR_SWIFT_SKIP_SIDETABLE=1
PLSR_SWIFT_IGNORE_DEALLOC_COUNT=1 PLSR_OBJC_OBJECT_GUARD=1`: the archiver
exceptions are swallowed, the Swift side-table and dealloc-count faults are
bypassed, and the CF-backed dictionary no longer hands libobjc an unaligned
key. The new `_objc_retain`/`_objc_release`/`_objc_msgSend` entry guards remove
the `libobjc+0xb79` fault, but the process still exits 139 at
`libobjc+0xfce` inside `_objc_msgSend` on an aligned Swift object whose class
cache is invalid (caller `AppKit+0x71178`). This is a Swift
metadata-registration gap, not a single bad key; no app window exists, so there
is no fresh side-by-side to capture. The last real window evidence remains
`appexec/window-real.png` / `appexec/compare-real.png`.

`make` is clean and `make test` is 17/17. The crash chain now ends at a clean
exit 0; the next boundary is AppKit window creation, not another selector.

The vendored `decompiled/vendor/QuartzCore` and
`decompiled/vendor/CoreGraphics` remain unbuilt: the host has no `objc/objc.h`,
no `gnustep-config` and no CoreGraphics headers, and the pearOS AppKit /
QuartzCore stack targets its own window server, so it is not ABI-linkable into
this Mach-O/libobjc host path.

## 2026-11-20 — compatibility guards reach a clean exit, still no window

`appexec` links Mayonnaise (`src/mayo_ui.cpp`), but the compatibility layer
does not yet have a loaded view tree to paint. The standalone Mayonnaise
window was removed; it was a placeholder, not the app.

With `PLSR_OBJC_OBJECT_GUARD=1`, the loader installs the `_objc_retain`/
`_objc_release`/`_objc_msgSend` entry guards (including the isa-range check),
the CF key-retain guard, the CF nil `addObject:`/`removeObject:` guards, and
the CF nil release guard. CotEditor now exits 0 after entry instead of
crashing. `PLSR_NSWINDOW=1` only swizzles
`-[NSApplication nextEventMatchingMask:]`; it does not create an `NSWindow`.
No view tree exists, so Mayonnaise cannot paint the app's controls yet.

The next boundary is AppKit window creation: make `NSApplicationMain` or the
delegate create an `NSWindow`, then enumerate its content view and feed the
frames/text into the Mayonnaise painter. The current evidence is
`/tmp/cot-rel.err` (exit 0, no signal) and `/tmp/cot-nsw.err` (no window).

## Session: Mayonnaise shell driven by loaded state (DONE)

`PLSR_MAYO_UI=1` no longer shows sample text.  The loader passes the recovered
bundle state to `src/mayo_ui.cpp` via `PLSR_APP_NAME`, `PLSR_APP_EXE`,
`PLSR_APP_BUNDLE` and `PLSR_APP_SUMMARY`; the editor opens the bundle's real
`Contents/Info.plist`, the sidebar lists the app/executable/plist and the
`Contents`/`MacOS`/`Resources` folders, and the header prints the real closure
result (`84 images, 45548 symbols, 0 unresolved`).  `appexec/mayo-ui.png` was
refreshed from a live run with `MAYO_SCREENSHOT`.

Also in this pass:

- The unmapped-rip fault after entry was glibc heap corruption in `calloc`,
  caused by `getpwuid_r` callers declaring 4096 bytes for a 4080-byte block.
  `reef_getpwuid_r` now never passes caller buffers to glibc; the crash is gone
  and the ObjC object guard is opt-in again (`PLSR_OBJC_OBJECT_GUARD=1`).
- Host slots `0x2a8` (access/faccessat), `0x230`/`0x238` (dyld image lookup for
  `_dladdr` and the exception call-stack builder) and `0x5b0` (pthread stack
  bounds) are implemented.
- Unguarded, CotEditor now stops at AppKit's real
  `Storyboard (Main) doesn't exist in bundle` exception: CFBundle lists
  `Base.lproj/Main.storyboardc` via readdir but never stats or accesses it, so
  the storyboard lookup still fails.  `PLSR_BOOTSTRAP_DELEGATE=1` instead
  crashes in Foundation's archiver dealloc (`Foundation+0x5f7e1`, nil +8).
  `applicationDidFinishLaunching:` and an `NSWindow` therefore remain undone.

`make test` is 17/17.

## Session: shell state module, sidebar-driven editor, CFBundle evidence

- Shell state moved out of the UI into `appexec/src/shell_state.cpp`
  (`shell_state_compose`, `shell_state_load`, `shell_entry_content`), so the
  editor content per sidebar entry is derived and testable without a compositor.
- The editor now follows the sidebar: `app`/`Info.plist` open the bundle's real
  `Contents/Info.plist`, `exe` shows the executable (hex dump when binary), and
  `contents`/`macos`/`resources` list those bundle directories.
- `appexec/tests/shell_state_test.cpp` covers the sidebar ids/titles, the
  header summary and each entry's document; `make test` runs it after the 17
  loader tests (17/17 + PASS).
- `Sidebar::select()` does not invoke `onSelect` when the item is already
  selected, so the initial document is set explicitly; clicks go through
  `onSelect`.
- Storyboard: CFBundle enumerates `Contents/Resources/Base.lproj` and receives
  `Main.storyboardc` from readdir, then performs no stat/access and still
  reports the storyboard missing. readdir's Darwin dirent layout was checked
  against the libSystem adapter (inode@0, type@0x10, name@0x11) and is correct,
  so the failure is in CF's resource matching, not the host filesystem shims.
  The app still stops at `Storyboard (Main) doesn't exist in bundle`.

## Delegated launch: Foundation+0x5f7e1 fixed

`Foundation+0x5f7e1` is `NSKeyedArchiver::_currentValues` (0x5f780), not
`dealloc`.  The original body is `[[self->_containers lastObject] _values]`:
the delegate-bootstrap archiver has an empty `_containers`, `lastObject`
returns nil, and the load from `nil+8` is the fault (`rdi=(nil), addr=0x8`).

`plsr_current_values_helper` now returns the current container's values
dictionary when there is one, and otherwise seeds `_containers` with a
`__NSKeyedCodingContainer` holding a CF dictionary and returns that dictionary.
`patch_archiver_current_values` is enabled by `PLSR_BOOTSTRAP_DELEGATE` as well
as `PLSR_PATCH_ARCHIVER`.

Verified with `PLSR_BOOTSTRAP_DELEGATE=1`: the run no longer faults at
`Foundation+0x5f7e1`; it now dies at `libobjc.A.dylib+0xb79`, called from
`CoreFoundation+0x290b`, with `rdi=rsi=<heap pointer>`, `rdx=0xffffec7b` —
the next boundary, not the same crash.

Shell selection coverage: `ShellSelection::select()` is the single handler the
Mayonnaise sidebar and the initial entry both use; `tests/shell_state_test.cpp`
now drives it with a callback and checks the delivered documents for
`resources`, `exe`, `contents`, an unknown id (fallback), and a missing
callback.

## Default front-end: a window now opens

`run-app.sh` sets `PLSR_MAYO_UI=1` unless `PLSR_APPKIT_UI` or `PLSR_MAYO_UI`
is set, so the documented command

    ./appexec/run-app.sh decompiled/app/CotEditor.app

opens the Mayonnaise window with the bundle's real Info.plist, bundle sidebar
and closure counts (`84 images, 45548 symbols, 0 unresolved`) instead of
terminating on the storyboard exception.  `PLSR_APPKIT_UI=1` still selects the
app's own AppKit entry.

For the AppKit entry, `install_bundle_path_fallback` wraps
`-[NSBundle pathForResource:ofType:inDirectory:]` and
`-[NSBundle URLForResource:withExtension:subdirectory:]`: when CFBundle returns
nil, it searches `Contents/Resources` (requested subdirectory, `Base.lproj`,
`en.lproj`, `English.lproj`, then any `*.lproj`) and returns the real path or
file URL.  Both wrappers install (orig IMPs logged), but AppKit still raises
`Storyboard (Main) doesn't exist in bundle`, so `storyboardWithName:bundle:`
is not reaching these methods or is failing on the returned URL; the AppKit
window is still not created.

## Correction: the Mayonnaise shell is not the app

The shell was briefly made the default front-end so that a window appeared.
That was wrong: the runtime must run CotEditor's own AppKit UI, and a
hand-built lookalike hides the real failure.  `run-app.sh` no longer sets
`PLSR_MAYO_UI`; the default enters the app's Mach-O entry, and the shell stays
opt-in as a diagnostic.

Current AppKit-path evidence:

- `+[NSStoryboard storyboardWithName:bundle:]` calls
  `-[NSBundle pathForResource:ofType:inDirectory:]` with `Main`/`storyboardc`
  (traced), and CFBundle now returns the correct
  `.../Contents/Resources/Base.lproj/Main.storyboardc` (logged as accepted).
- AppKit still raises `Storyboard (Main) doesn't exist in bundle` right after.
- The two-argument `URLForResource:withExtension:` and
  `pathForResource:ofType:` were patched too; neither logged a call, so AppKit
  is not reaching them on this path.
- Therefore resolution of the file on disk is no longer the blocker: the
  remaining failure is inside AppKit's storyboard construction/its own bundle
  query.  No app window yet.

`make test` stays 17/17 plus the shell-state test.

## AppKit storyboard bootstrap: still blocked, now localised

The app's own path (`./appexec/run-app.sh decompiled/app/CotEditor.app`, no
switches) still exits 134 at `Storyboard (Main) doesn't exist in bundle`.
The Mayonnaise shell is opt-in only and is not the app's UI.

Evidence from `PLSR_REEF_TRACE`:

- All four NSBundle resource methods are patched
  (`pathForResource:ofType:`, `pathForResource:ofType:inDirectory:`,
  `URLForResource:withExtension:`, `URLForResource:withExtension:subdirectory:`),
  plus `stringByAppendingPathComponent:` and `URLByAppendingPathComponent:` on
  `__NSCFString`/`NSString`.
- AppKit calls `pathForResource:ofType:inDirectory:` with `Main`/`storyboardc`;
  CFBundle returns the correct
  `.../Base.lproj/Main.storyboardc` and the wrapper accepts it.
- Immediately after, AppKit calls `+[NSDictionary dictionaryWithContentsOfFile:]`
  with a **nil** argument, then raises. No URL/2-arg/append wrapper logs.
- So the value AppKit appends to is nil or the append selector is one we have
  not identified; the storyboard path itself resolves. Next step: dump the
  selector names behind AppKit's `__objc_selrefs` used at
  `+[NSStoryboard storyboardWithName:bundle:]` (AppKit+0x1b6230) instead of
  guessing them.

`make test` stays 17/17 plus the shell-state test; the shell still runs with
`PLSR_MAYO_UI=1`.

## AppKit storyboard resolution: prior diagnosis overturned

Resumed the default app launch with `PLSR_DUMP_SELREF=0x1b6230`. The selector
references in `+[NSBundle pathForResource:ofType:]` show AppKit's exact flow:
`mainBundle`, `stringByAppendingPathExtension:`, `pathForResource:ofType:`,
localization enumeration, `resourcePath`, `stringByAppendingPathComponent:`,
`fileExistsAtPath:isDirectory:`, another append, and finally
`dictionaryWithContentsOfFile:`. This confirms resourcePath and path-component
appending are part of the actual lookup.

The current runtime trace contradicts the earlier note that AppKit passes nil:
the correct `Base.lproj/Main.storyboardc` path resolves, AppKit appends
`Info.plist`, and CoreFoundation successfully decodes the plist into a
dictionary. The earlier `Storyboard (Main) doesn't exist` failure is no longer
reproduced in this checkout. A 15-second launch remains alive after loading
the storyboard metadata and probing saved application state; it did not reach
a window trace or emit a later failure before the timeout. Next, trace the
storyboard construction after this metadata load to locate the current stall.

## SwiftUI reconstruction resumed in appexec

REA CLI decompilation was attempted on
`pulsar-appexec/usr/lib/pulsar-appexec/host/libpulsar-app-host.so` at its
window constructor (`0x132250`). The installed Ghidra is 12.2, while REA 6.3
accepts Ghidra 12.1.x, so that provider returned `provider_unavailable`.
Existing Ghidra 12.2 decompilation is available at
`decompiled/src/elf/usr_lib_pulsar-appexec_host_libpulsar-app-host.so/`; its
focused interface records include the window constructor and paint callbacks.

Copied the SwiftUI C engine, host facade, state, text and canvas sources into
`appexec/src/swiftui` and changed `appexec/Makefile` to build from those local
sources. Copied the focused app-host interface decompilation into that tree as
reference material. `make -C appexec -j4` succeeds. This gives appexec its own
renderer source, but the generated SwiftUI ABI symbols still return zero, and
the engine is not a drop-in implementation of all 24,470 recovered SwiftUI
functions.

New runtime trace with `PLSR_REEF_TRACE=1 PLSR_NSWINDOW=1` overturns the stale
launch notes: CotEditor loaded `Main.storyboardc/Info.plist`, decoded the
MainMenu, constructed a real `CotEditor.EditorTextView` and
`CotEditor.WindowContentViewController`, built a four-node NSView-derived
tree, and presented its app-owned NSWindow through Mayonnaise. The extracted
widget spec contains one live editor but no other controls or toolbar. Thus a
real editable app window is now reached; matching its complete original view
tree and bridging its layout/style into the SwiftUI engine remain open.

## Generic AppKit control projection resumed

Expanded the runtime projection in `appexec/src/appexec.c` beyond the editor:
it recognizes the common AppKit control families (buttons, text fields, toggles,
sliders, progress, segmented controls, popups, images, separators and list
containers), keeps frame/visibility/enabled/alpha state, and routes Mayo input
back to the originating object. Added extraction of menu item arrays and
segmented labels, which are model data rather than NSView children. The
application main menu is projected into a host-rendered menu row when present.

The first manual CotEditor launch after adding state capture found that generic
NSViews do not implement `isEnabled`; capture now checks optional selectors
before calling them. A subsequent launch reached the widget spec and presented
the app-owned window without that exception. The current CotEditor tree still
contains only the editor, whose recovered frame is 0 points wide; the Mayo
layout fallback fills the parent width. Its actual toolbar remains nil, and
the main menu API returned no menu in this launch. `make -C appexec -j4`
succeeds. This is a working base for common controls, not complete AppKit or
SwiftUI coverage: custom views, table data sources, native menus/popovers,
accessibility and the original SwiftUI ABI remain unfinished.

## Blank-window visual correction

A `MAYO_SCREENSHOT` capture showed the blank document as a plain white surface:
the app starts with an empty text view, but its host adapter represented it as a
single-line field and lost the editor gutter/caret affordance. Replaced that
projection with the editable multi-line editor widget. The recovered zero-width
frame is expanded to its parent, and an empty document now still draws its
gutter and first line number. Removed the synthetic fallback toolbar after
confirming CotEditor exposes no toolbar object or corresponding controls in
this launch. The runtime must surface app-owned controls as the app creates
them; menu and toolbar decoding remain open.

The rendered editor preserves a selection anchor, highlights selected text,
supports Shift+Arrow selection, Ctrl+A/C/X/V, Backspace/Delete, and local undo /
redo snapshots. Host focus now calls `makeFirstResponder:` on the real
`EditorTextView`; text changes call `shouldChangeTextInRange:replacementString:`,
mutate its text storage, and call `didChangeText` so app observers and document
state can respond. Text storage was previously an opt-in shim; it now installs
by default for text views, with `PLSR_DISABLE_TEXT_STORE=1` for diagnosis. A
manual app launch with `PLSR_WIDGET_SELFTEST=1` verified `CotEditor` round-trips
through the actual source view and its text storage with no `PLSR_TEXT_STORE`
setting. Physical key-event delivery still needs verification. The current
screen is not at host parity: the app's inspector, status bar, toolbar, and
menu are absent.

## 2026-11-22 — live app-window discovery

The host loop now polls the app's `NSApplication.windows` list every 200 ms,
so windows created after initial presentation can be handed to the host even
while Mayonnaise owns the main-thread event loop. Already-presented windows are
skipped, avoiding repeated SwiftUI tree allocation. The per-process window
registry grows dynamically instead of stopping after eight windows.

`make -C appexec -j4 appexec` succeeds. A traced 8-second CotEditor launch
stayed alive, polled the one-window list 19 times, and presented exactly one
app-owned window. The timeout supervisor ended the still-running process with
SIGKILL; this is not evidence of an app crash. Later-window creation and exact
UI parity remain unverified. The REA CLI is 6.3.0; its doctor reports the
Codex MCP registration missing and no configured Ghidra provider
(`GHIDRA_INSTALL_DIR` unset). A read-only setup plan is prepared; approval is
pending before changing the Codex config or updating the installed REA skill.
Existing Ghidra 12.2 decompilation remains the native reference in the meantime.

## 2026-11-22 — live AppKit state synchronization

The 200 ms host poll now rereads the presented window's live AppKit views and
updates the retained host controls in place. Text views, fields, labels,
buttons, booleans, sliders, visibility, enabled state and opacity can follow
source-side changes without rebuilding the control tree or discarding editor
focus and caret state. Host edits still route through the original view's
AppKit text APIs.

`make -C appexec -j4 appexec` and `git diff --check` pass. A traced 7-second
launch survived 28 state/window polls and captured the live CotEditor window
tree (109 labels, 9 menu controls, one editor); the timeout killed the process
while it was still running. This confirms the polling path remains active, but
does not yet verify keyboard input, post-edit inspector updates, newly created
documents or pixel parity.

REA `decode-interface-builder` Evidence `ev_af40df1c1b71bb0869bf275ca9488c7be5c5fec8ea679753db6b586d015af163`
found nine compiled nib/storyboard documents in CotEditor. `MainMenu.nib` has
709 objects and 181 connections with a complete recognized hierarchy; it
declares source actions including `newDocument:`, `openDocument:`,
`toggleInspector:`, and `toggleStatusBar:`. These are archive observations,
not proof the handlers work. The REA report explicitly leaves runtime-created
UI, visibility, constraints and pixel geometry unknown.

The window poll also mirrors each live `NSWindow.title` and excludes source
windows already closed from widget refresh. The compact decoded archive record
is saved at `decompiled/re/CotEditor-7.1.3-interface-builder.json`. Correction
to the older blank-window note above: current live capture includes 109 labels
and 9 menu controls (and an inspector in the captured app tree); their
post-edit behavior and pixel parity are still unverified.

The bridge now also polls the AppKit content-view frame and resizes its live
Mayonnaise window when the app changes its document size. The host resize is
only source-to-host at this stage; interactive host resizing is not propagated
back to AppKit yet. Rebuild succeeded and a 5-second traced CotEditor launch
continued polling with one live window; timeout ended it while still running.

2026-11-22 follow-up: Mayonnaise now reports user-driven resize events through
`Window::onResized`; the runtime bridge sends those content dimensions back to
the original AppKit `NSWindow`. This closes the host-to-source half of the
resize loop. The Mayonnaise archive and appexec build pass; a bounded
CotEditor launch remained alive with the one-window tree (109 labels, 9 menus,
one editor) until timeout. The resize callback still needs direct drag
verification. Host minimum size is relaxed to 1 point pending mirroring
AppKit's actual minimum-size constraint.

2026-11-22 menu-state follow-up: the bridge now rereads each live AppKit menu
model while polling, rebuilding menu entries only when count, title, enabled,
checkmark, separator or icon state changes. It also tracks changing picker
options and selection. This uses the real `NSMenuItem` targets/actions already
decoded from CotEditor's nibs (REA Evidence
`ev_af40df1c1b71bb0869bf275ca9488c7be5c5fec8ea679753db6b586d015af163`),
including `newDocument:`, `openDocument:`, `toggleInspector:` and
`toggleStatusBar:`. Mayonnaise and appexec builds pass; a 4-second runtime
launch stayed alive and exposed the expected 109-label, 9-menu, one-editor
tree. Actual menu activation and live item changes have not yet been exercised.

The menu poll now compares AppKit menu-item identity and order before replacing
the host menu model, so ordinary 200 ms state refreshes update retained item
labels/enabled/check state without reallocating the full menu tree. A fresh
3-second traced launch remained alive through repeated polls with no exception
or crash evidence. This confirms stability only; the desktop session's Wayland
surface is not reachable through the available X11 input tool, so interaction
still lacks direct event-level verification.

2026-11-22 runtime action follow-up: NIB control connectors can now apply
actions whose archived destination is nil, preserving Cocoa's responder-chain
dispatch instead of dropping the real `NSMenuItem` action. With EIS keyboard
input, Ctrl+N matched CotEditor's archived `newDocument:` shortcut. The app
then faulted in the action path (SIGSEGV); this remains an unresolved runtime
failure, and no app-specific menu behavior was added. Clean-room AppKit rebuilt
successfully. Added trace points around generic `NSApplication` action target
resolution to locate the fault on the next valid runtime launch.

2026-11-22 follow-up: the appexec bridge now logs the real action selector,
target, enabled state, and entry/return around `NSApplication.sendAction:` so
the next shortcut run can distinguish selector lookup from action execution.
The clean-room `NSApplication.run` implementation now polls its actual
`nextEventMatchingMask:untilDate:inMode:dequeue:` implementation and advances
the CF run loop between polls, allowing startup hooks, timers, and sources to
run together. Both the clean-room AppKit and appexec builds succeeded. A fresh
launch currently stalls before window creation; with explicit delegate
bootstrap it faults in Objective-C dispatch, so the shortcut crash and this
startup path still need runtime traces before behavior is verified.

2026-11-22 UI fidelity correction: a live `PLSR_VIEW_TRACE=1` launch confirms
CotEditor's app-owned content hierarchy is only `NSScrollView` → `NSClipView`
→ `CotEditor.EditorTextView` (plus hidden scroller). The manually built document
window currently has no toolbar (`toolbar=nil`, zero items), so its content
projection correctly reports zero buttons. Do not synthesize controls to fill
this gap. `PLSR_APP_TOOLBAR=1` reaches the manual controller setup but the app
faults during `DocumentWindowController.initWithWindow:` before toolbar
installation; the lifecycle must be fixed before its real toolbar can appear.
