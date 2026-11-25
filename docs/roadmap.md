# Roadmap

Ordered work for making the macOS runtime testable and understood end to end.

## 1. Keep the test app launchable (done)

- [x] Extract `CotEditor_7.1.3.dmg`
- [x] Stage runtime root and SONAME symlink farm
- [x] Launch through `appexec` and confirm a rendered window
- [x] Add a launcher that starts a compositor only when `WAYLAND_DISPLAY` is absent
- [x] Add a smoke check: process alive + screenshot non-blank + no toolkit errors
- [x] Mount a `.dmg` in userspace and launch the `.app` inside it directly
      (`pulsar-run --app X.dmg`, cache under `$XDG_CACHE_HOME/pulsar-dmg`)

## 2. Finish high-value ELF decompilation (non-Wayland)

Current focus. Run order:

- [x] `host/libpulsar-app-host.so` - AppKit host bridge
- [x] `host/libpulsar-web-host.so` - WebKit host bridge
- [x] `lib/libpulsar-metal-msl.so`
- [x] `lib/libpulsar-metal-transl.so` (1866 functions)
- [x] `usr/lib/pulsar-appexec/usr/bin/login`
- [ ] ICU (`libicui18n`, `libicuuc`) - large, low behavioural value
- [ ] `compat/libpulsar-toolkit.so.1.0` - 13360 files, corrupt index; re-run queued

Acceptance: every targeted ELF object has `src/elf/<slug>/_DONE` and a row in
`src/elf/_SUMMARY.tsv`; failed-function counts recorded.

## 3. Finish Mach-O analysis

- [x] Resume the interrupted `AddressBook.framework` batch (201 functions)
- [x] Decompile load-bearing shims first: `libSystem.B.dylib`,
      `libc++.1.dylib`, `libobjc`, `libdispatch`, `CoreFoundation`,
      `CoreGraphics`, `QuartzCore`, `Metal`
- [x] Decompile `Foundation` (7382), `libswiftCoreFoundation` (370),
      `libswiftAppKit` (1478), and `AppKit` (12002)
- [ ] Decompile the Swift UI stack: `libswiftCore`, `SwiftUI`
- [ ] Bulk-decompile the remaining small frameworks (median object ~43 KB)
- [ ] Cross-link exported symbols (`src/macho/exports/`) to ELF host symbols

Driver: `ghidra/decompile-everything.sh macho` (core first, resumable).
Coverage: `tools/re_status.py` -> `re/STATUS.md`, `re/missing.txt`.

Acceptance: every Mach-O in `logs/macho/list.txt` has a status row; the
CotEditor dependency closure (52 dylibs) is covered.

## 4. Behaviour questions to answer from the source

- (Deferred) Exact Wayland global set required by `cutefish-toolkit` and fallback options
- (Deferred) GLES/EGL config selection and why X11 cannot satisfy it
- `LC_DYLD_CHAINED_FIXUPS` binding path in the real loader vs `plsr-lite`
- Sandbox / entitlement checks in `main` before `execv`
- Which Metal translation paths CotEditor triggers

## 5. Conformance and replacement loader

Current parity 9/14. Fix fixture construction, then expectation logic:

- `dylib_as_program`: fixture has no header-mapping segment; expected
  `MH_DYLIB` rejection, not segment error
- `exe_no_MH_PIE`: add a segment mapping the header so the non-PIE check fires
- `exe_no_entry`: same header-mapping fixture fix
- `exe_unixthread`: same header-mapping fixture fix
- `fat_x86_dylib`: fat wrapper around a dylib needs a mapped header segment

Then extend `src/plsr-lite` with chained-fixup and classic bind parity tests.

## 6. Clean-room runtime (started)

- [x] `libplsr-libsystem.so`: first slice of ~150 Darwin-spelled libc exports
- [x] `--shim-lib` loader support and `plsr_host_imports` manifests
- [x] `libplsr-objc.so`: class-list scan, metadata registration, `objc_msgSend`
- [x] Relative method lists, `__objc_catlist` categories, `Super2`/`stret` dispatch
- [x] `libplsr-foundation.so`: `NSObject`, `NSConstantString`, `NSString`, `NSNumber`, collections, `NSLog`
- [x] Generated Mach-O ObjC and Foundation fixtures pass end to end
- [ ] Translating surfaces: `open`, `stat`, `mmap`, `pthread`, `dispatch`
- [ ] CoreFoundation bridging and the remaining Foundation surface
- [ ] AppKit on the host bridge; Swift runtime entry points

## 7. End-state

A reproducible, documented runtime tree that launches a real macOS `.app`
under the shipped `pulsar-appexec`, with decompiled evidence for every
load-bearing dependency and a parity suite that separates loader behavior from
host-environment behavior.
