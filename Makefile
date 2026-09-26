CXX      ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter
PKGS     := gtkmm-3.0 libcurl nlohmann_json
CXXFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(shell pkg-config --libs $(PKGS))

SRC := $(wildcard cpp/*.cpp)
OBJ := $(patsubst cpp/%.cpp,build/obj/%.o,$(SRC))
BIN := build/monkeylauncher

# System-wide install layout used by the Arch/Debian packages (install.sh
# does its own per-user install under ~/.local).
PREFIX  ?= /usr
LIBDIR  ?= $(PREFIX)/lib/monkeylauncher

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/obj/%.o: cpp/%.cpp $(wildcard cpp/*.hpp)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

install: all
	install -Dm755 $(BIN) $(DESTDIR)$(LIBDIR)/monkeylauncher
	install -Dm644 VERSION $(DESTDIR)$(LIBDIR)/VERSION
	install -Dm755 src/MonkeyLauncherCLI.sh $(DESTDIR)$(PREFIX)/bin/MonkeyLauncherCLI
	printf '#!/bin/sh\nexec $(LIBDIR)/monkeylauncher "$$@"\n' > build/MonkeyLauncher.wrapper
	install -Dm755 build/MonkeyLauncher.wrapper $(DESTDIR)$(PREFIX)/bin/MonkeyLauncher
	install -Dm644 src/monkeylauncher.desktop $(DESTDIR)$(PREFIX)/share/applications/monkeylauncher.desktop
	install -Dm644 src/logo.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/1024x1024/apps/monkeylauncher.png

clean:
	rm -rf build

.PHONY: all install clean
