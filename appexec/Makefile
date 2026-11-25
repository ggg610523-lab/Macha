.DEFAULT_GOAL := appexec

CC ?= cc
CXX ?= g++
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++20
LDLIBS = -lz -lm -lpthread -ldl

SWIFTUI = src/swiftui
MAYO = ../deps/mayonnaise
MAYO_LIB = $(MAYO)/build/src/libmayonnaise.a
MAYO_CFLAGS = $(shell pkg-config --cflags sdl3 freetype2)
MAYO_LIBS = $(shell pkg-config --libs sdl3 freetype2) -lGL -lvulkan
INCLUDES = -Isrc -I$(SWIFTUI) -I$(MAYO)/include

C_SRC = src/appexec.c src/reef_host.c \
        $(SWIFTUI)/engine.c $(SWIFTUI)/state.c $(SWIFTUI)/text.c \
        $(SWIFTUI)/canvas.c $(SWIFTUI)/host.c
C_OBJ = $(C_SRC:.c=.o)
CXX_OBJ = src/mayo_ui.o src/shell_state.o build/glad.o

$(MAYO_LIB):
	cmake -S $(MAYO) -B $(MAYO)/build -DCMAKE_BUILD_TYPE=Release \
	  -DMAYONNAISE_BUILD_APPS=OFF -DMAYONNAISE_BUILD_TESTS=OFF \
	  -DMAYONNAISE_USE_SYSTEM_DEPS=ON
	cmake --build $(MAYO)/build -j"$$(nproc)"

appexec: $(C_OBJ) $(CXX_OBJ) $(MAYO_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(C_OBJ) $(CXX_OBJ) $(MAYO_LIB) $(MAYO_LIBS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) $(MAYO_CFLAGS) -c $< -o $@

src/mayo_ui.o: src/mayo_ui.cpp src/widget_spec.h src/swiftui/engine.h src/shell_state.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(MAYO_CFLAGS) -c $< -o $@

build/glad.o: $(MAYO)/third_party/glad/src/gl.c
	mkdir -p build
	$(CC) $(CFLAGS) -fPIC -I$(MAYO)/third_party/glad/include -c $< -o $@

src/shell_state.o: src/shell_state.cpp src/shell_state.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

tests/shell_state_test: tests/shell_state_test.cpp src/shell_state.cpp src/shell_state.h
	$(CXX) $(CXXFLAGS) -Isrc tests/shell_state_test.cpp src/shell_state.cpp -o $@

test: appexec tests/shell_state_test
	sh tests/run.sh
	./tests/shell_state_test

clean:
	rm -f appexec $(C_OBJ) $(CXX_OBJ) tests/shell_state_test

.PHONY: test clean
