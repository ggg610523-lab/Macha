# Clean-room host libraries.
#
# Builds the replacement frameworks/libraries plus the Mach-O loader from
# source (build/appexec); no prebuilt host binary and no application bundle
# is involved.  Every library exports the leading-underscore symbols a Mach-O
# image binds to, plus the plsr_host_imports install-name manifest the loader
# reads through --shim-lib.
#
# The SwiftUI-family ABI file (swiftui/generated/symbols.S) and its manifests
# are checked in, so the build needs no generator step and no input beyond
# this directory.

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -fPIC
LDLIBS = -lm
BUILD = build

LIBS = $(BUILD)/libplsr-libsystem.so \
       $(BUILD)/libplsr-objc.so \
       $(BUILD)/libplsr-foundation.so \
       $(BUILD)/libplsr-corefoundation.so \
       $(BUILD)/libplsr-dispatch.so \
       $(BUILD)/libplsr-appkit.so \
       $(BUILD)/libplsr-swiftui.so

# The loader shares this tree's SwiftUI engine sources.  The Mayonnaise
# frontend (appexec/mayo_ui.cpp, appexec/shell_state.cpp) needs the external
# SDL3/OpenGL toolkit and is not part of this build; appexec.c calls its
# plsr_mayo_* entry points through weak symbols and checks for NULL.
LOADER_SRC = appexec/appexec.c appexec/reef_host.c \
             swiftui/engine.c swiftui/state.c swiftui/text.c \
             swiftui/canvas.c swiftui/host.c

all: $(LIBS) $(BUILD)/appexec

$(BUILD):
	mkdir -p $(BUILD)

# libSystem: libc/BSD/math/pthread entry points, Darwin syscall translations,
# CommonCrypto (through the host OpenSSL).
$(BUILD)/libplsr-libsystem.so: libsystem_shim.c runtime_extra.c \
		platform_symbols.c crypto_shim.c | $(BUILD)
	$(CC) $(CFLAGS) -shared -o $@ libsystem_shim.c runtime_extra.c \
		platform_symbols.c crypto_shim.c $(LDLIBS) -pthread -lcrypto -lgcc_s

# Objective-C runtime: class/category scan, method lists, msgSend.
$(BUILD)/libplsr-objc.so: objc_runtime.c objc_msg_send.S objc_abi.h | $(BUILD)
	$(CC) $(CFLAGS) -shared -o $@ objc_runtime.c objc_msg_send.S

$(BUILD)/libplsr-foundation.so: foundation.c foundation_classes.c objc_abi.h \
		$(BUILD)/libplsr-objc.so | $(BUILD)
	$(CC) $(CFLAGS) -shared -o $@ foundation.c foundation_classes.c \
		-L$(BUILD) -lplsr-objc -Wl,-rpath,'$$ORIGIN'

$(BUILD)/libplsr-corefoundation.so: corefoundation.c objc_abi.h \
		$(BUILD)/libplsr-objc.so $(BUILD)/libplsr-foundation.so | $(BUILD)
	$(CC) $(CFLAGS) -shared -o $@ corefoundation.c -L$(BUILD) \
		-lplsr-foundation -lplsr-objc -Wl,-rpath,'$$ORIGIN' $(LDLIBS)

$(BUILD)/libplsr-dispatch.so: dispatch.c | $(BUILD)
	$(CC) $(CFLAGS) -shared -o $@ $< -pthread

$(BUILD)/libplsr-appkit.so: appkit.c nib.c appkit_surface.h objc_abi.h \
		$(BUILD)/libplsr-objc.so $(BUILD)/libplsr-foundation.so \
		$(BUILD)/libplsr-corefoundation.so | $(BUILD)
	$(CC) $(CFLAGS) -shared -o $@ appkit.c nib.c -L$(BUILD) \
		-lplsr-foundation -lplsr-corefoundation -lplsr-objc \
		-Wl,-rpath,'$$ORIGIN' $(LDLIBS)

$(BUILD)/libplsr-swiftui.so: swiftui/frameworks.c swiftui/canvas.c \
		swiftui/text.c swiftui/engine.c swiftui/state.c swiftui/host.c \
		swiftui/engine.h swiftui/swiftui.h \
		swiftui/generated/symbols.S swiftui/generated/install_names.inc \
		swiftui/generated/frameworks_manifest.inc | $(BUILD)
	$(CC) $(CFLAGS) -Iswiftui -shared -o $@ swiftui/frameworks.c \
		swiftui/canvas.c swiftui/text.c swiftui/engine.c swiftui/state.c \
		swiftui/host.c swiftui/generated/symbols.S $(LDLIBS) -lz

# A from-scratch x86_64 Mach-O loader: maps segments, applies rebases and
# binds, resolves chained fixups and the dependency closure under a runtime
# root, and can replace macOS libraries with the clean-room builds above via
# --shim-lib.
$(BUILD)/appexec: $(LOADER_SRC) appexec/reef_host.h appexec/widget_spec.h \
		swiftui/engine.h swiftui/swiftui.h | $(BUILD)
	$(CC) $(CFLAGS) -std=gnu11 -Iappexec -Iswiftui -o $@ $(LOADER_SRC) \
		-lz -lm -lpthread -ldl

clean:
	rm -rf $(BUILD)

.PHONY: all clean
