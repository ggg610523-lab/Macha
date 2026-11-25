# Builds everything in this tree.
#
# all          clean-room libraries + loader, plsr-lite, tools
# smoke        generates Mach-O fixtures and runs both loaders over them
# full-appexec the appexec binary including the Mayonnaise window front end
#              (needs deps/mayonnaise plus system SDL3/FreeType/OpenGL/Vulkan)

CC ?= cc
CXX ?= g++

all: cleanroom plsr-lite tools

cleanroom:
	$(MAKE) -C cleanroom

plsr-lite:
	$(MAKE) -C plsr-lite

tools:
	$(MAKE) -C tools
	$(MAKE) -C tools/macho-deps

smoke: all
	sh tests/smoke.sh

# Build the optional Mayonnaise front end when its dependencies are present.
# Skipped cleanly (exit 0) when cmake or the SDL3/FreeType development
# packages are missing, so the source drop still builds anywhere.
full-appexec:
	@command -v cmake >/dev/null 2>&1 || { \
		echo "full-appexec: cmake not found; skipping the Mayonnaise front end"; exit 0; }
	@pkg-config --exists sdl3 freetype2 || { \
		echo "full-appexec: SDL3/FreeType dev packages not found; skipping"; exit 0; }
	$(MAKE) -C appexec

clean:
	$(MAKE) -C cleanroom clean
	$(MAKE) -C plsr-lite clean
	$(MAKE) -C tools clean
	$(MAKE) -C tools/macho-deps clean
	$(MAKE) -C appexec clean
	rm -rf deps/mayonnaise/build

.PHONY: all cleanroom plsr-lite tools smoke full-appexec clean
