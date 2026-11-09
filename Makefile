# Lieve — build without CMake: Qt 6 via pkg-config, KDE Frameworks 6 headers
# from /usr/include/KF6. Usage: make && make install (to ~/.local).
# On Windows it builds the same way in an MSYS2 UCRT64 shell (packaging/windows).

PREFIX ?= $(HOME)/.local
CXX ?= g++
ifeq ($(OS),Windows_NT)
WINDOWS := 1
EXE := .exe
# A window program: no console window behind it.
LDFLAGS += -mwindows
endif
BIN := lieve$(EXE)

CXXFLAGS ?= -O2 -pipe
CXXFLAGS += -std=c++20 -Wall -Wextra -Wno-unused-parameter $(if $(WINDOWS),,-fPIC)
# Distribution packages build with PACKAGED=1: updates then come from the
# package manager, not from Lieve's own updater.
CXXFLAGS += $(if $(PACKAGED),-DLIEVE_PACKAGED)

QT_LIBEXEC := $(shell pkg-config --variable=libexecdir Qt6Core)
QT_BIN := $(shell pkg-config --variable=bindir Qt6Core)
MOC ?= $(firstword $(wildcard $(QT_LIBEXEC)/moc$(EXE) $(QT_BIN)/moc$(EXE) /usr/lib/qt6/moc /usr/lib/qt6/libexec/moc /usr/libexec/qt6/moc /usr/lib64/qt6/libexec/moc))
RCC ?= $(firstword $(wildcard $(QT_LIBEXEC)/rcc$(EXE) $(QT_BIN)/rcc$(EXE) /usr/lib/qt6/rcc /usr/lib/qt6/libexec/rcc /usr/libexec/qt6/rcc /usr/lib64/qt6/libexec/rcc))
LRELEASE ?= $(firstword $(wildcard $(QT_BIN)/lrelease$(EXE) $(QT_LIBEXEC)/lrelease$(EXE) /usr/lib/qt6/bin/lrelease /usr/lib64/qt6/bin/lrelease) $(shell command -v lrelease-qt6 lrelease6 lrelease 2>/dev/null))
LUPDATE ?= $(firstword $(wildcard $(QT_BIN)/lupdate$(EXE) $(QT_LIBEXEC)/lupdate$(EXE) /usr/lib/qt6/bin/lupdate /usr/lib64/qt6/bin/lupdate) $(shell command -v lupdate-qt6 lupdate6 lupdate 2>/dev/null))
QT_CFLAGS := $(shell pkg-config --cflags Qt6Widgets Qt6DBus Qt6Network)
QT_LIBS := $(shell pkg-config --libs Qt6Widgets Qt6DBus Qt6Network)
# KF6 headers sit next to Qt's: /usr/include/KF6, or /ucrt64/include/KF6 in MSYS2.
KF_INCLUDE ?= $(firstword $(wildcard $(dir $(shell pkg-config --variable=includedir Qt6Core))KF6) /usr/include/KF6)
KF_CFLAGS := $(addprefix -I,$(patsubst %/,%,$(wildcard $(KF_INCLUDE)/*/)))
# KF6BreezeIcons: the Breeze icons built into a library, for Windows (no icon
# theme) and for desktops whose theme lacks some (GNOME's Adwaita…).
KF_LIBS := -lKF6TextEditor -lKF6Parts -lKF6XmlGui -lKF6CoreAddons -lKF6DBusAddons -lKF6WindowSystem -lKF6SyntaxHighlighting -lKF6ConfigCore -lKF6WidgetsAddons -lKF6I18n -lKF6BreezeIcons

SRC := $(wildcard src/*.cpp)
MOC_HEADERS := $(shell grep -l Q_OBJECT src/*.h)
MOC_SRC := $(patsubst src/%.h,build/moc_%.cpp,$(MOC_HEADERS))
TS := $(wildcard i18n/lieve_*.ts)
QM := $(patsubst i18n/%.ts,build/%.qm,$(TS))
ifeq ($(strip $(LRELEASE)),)
# Without Qt's lrelease (package qt6-tools / qt6-linguist) Lieve still builds,
# in English only.
$(warning lrelease not found: building without translations (install qt6-tools))
QM :=
endif
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC)) $(MOC_SRC:.cpp=.o) build/qrc_lieve.o build/qrc_i18n.o
ifdef WINDOWS
# lieve.exe's icon and version for Explorer and Task Manager (data/lieve.rc).
OBJ += build/lieve_rc.o
endif
WINDRES ?= windres
comma := ,
# "0.1.4" in src/version.h -> 0,1,4,0
VERSION_NUMBERS = $(subst .,$(comma),$(shell sed -n 's/.*LIEVE_VERSION "\(.*\)".*/\1/p' src/version.h)),0

# The built-in terminal (where the Konsole part is missing), vendored from Qt
# Creator, see src/3rdparty/README.md: libvterm (C), the terminal view, and
# libptyqt with ConPTY on Windows, a Unix pty elsewhere.
PTY := $(if $(WINDOWS),conptyprocess,unixptyprocess)
TERMINAL_SRC := $(wildcard src/3rdparty/terminal/*.cpp) src/3rdparty/ptyqt/ptyqt.cpp src/3rdparty/ptyqt/$(PTY).cpp
TERMINAL_MOC := build/3rdparty/terminal/moc_terminalview.o build/3rdparty/terminal/moc_terminalsurface.o build/3rdparty/ptyqt/moc_$(PTY).o
TERMINAL_OBJ := $(patsubst src/%.c,build/%.o,$(wildcard src/3rdparty/vterm/src/*.c)) \
	$(patsubst src/%.cpp,build/%.o,$(TERMINAL_SRC)) $(TERMINAL_MOC) build/qrc_terminal.o
OBJ += $(TERMINAL_OBJ)
# Its code is static, inside the program (no DLL import/export).
CXXFLAGS += -DTERMINALLIB_STATIC_LIBRARY
# conptyprocess: CreateProcessAsUserW (ntdll is loaded at run time).
TERMINAL_LIBS := $(if $(WINDOWS),-ladvapi32)

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(QT_LIBS) $(KF_LIBS) $(TERMINAL_LIBS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) $(KF_CFLAGS) -MMD -MP -c $< -o $@

build/moc_%.cpp: src/%.h | build
	$(MOC) $< -o $@

# The icon is also built into the program: the window sends its pixels to the
# compositor, so it is right even where the theme icon is cached.
build/qrc_lieve.cpp: data/lieve.qrc data/lieve.svg | build
	$(RCC) -name lieve $< -o $@

build/qrc_lieve.o: build/qrc_lieve.cpp
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c $< -o $@

# data/lieve.ico is made from data/lieve.svg by packaging/windows/make-icon.sh.
build/lieve_rc.o: data/lieve.rc data/lieve.ico src/version.h | build
	$(WINDRES) -I src -I data -DLIEVE_VERSION_NUMBERS=$(VERSION_NUMBERS) $< -O coff -o $@

# Translations (i18n/lieve_<language>.ts, made with "make translations") are
# compiled and built into the program as :/i18n/lieve_<language>.qm.
build/%.qm: i18n/%.ts | build
	$(LRELEASE) -silent $< -qm $@

build/i18n.qrc: $(TS) | build
	printf '<RCC><qresource prefix="/i18n">%s</qresource></RCC>\n' '$(foreach q,$(QM),<file>$(notdir $(q))</file>)' > $@

build/qrc_i18n.cpp: build/i18n.qrc $(QM)
	$(RCC) -name i18n $< -o $@

build/qrc_i18n.o: build/qrc_i18n.cpp
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c $< -o $@

# Collects the interface strings from src/ into the .ts files, for translators
# (Qt Linguist). New language: make translations TS=i18n/lieve_xx.ts
translations:
	$(LUPDATE) -silent -no-obsolete -locations relative src -ts $(TS)

build/moc_%.o: build/moc_%.cpp
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) $(KF_CFLAGS) -Isrc -c $< -o $@

# The vendored code keeps its own warnings to itself (-w). ptyqt's Windows
# part is written for the wide-character API (UNICODE).
ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -O2 -pipe
VENDOR_CFLAGS = $(CFLAGS) -w $(if $(WINDOWS),,-fPIC) -Isrc/3rdparty/vterm/include
VENDOR_CXXFLAGS = $(CXXFLAGS) $(QT_CFLAGS) -w -Isrc/3rdparty/vterm/include $(if $(WINDOWS),-DUNICODE -D_UNICODE -DNOMINMAX)

build/3rdparty/vterm/%.o: src/3rdparty/vterm/%.c
	@mkdir -p $(@D)
	$(CC) $(VENDOR_CFLAGS) -MMD -MP -c $< -o $@

build/3rdparty/%.o: src/3rdparty/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(VENDOR_CXXFLAGS) -MMD -MP -c $< -o $@

build/3rdparty/terminal/moc_%.cpp: src/3rdparty/terminal/%.h
	@mkdir -p $(@D)
	$(MOC) $< -o $@

build/3rdparty/ptyqt/moc_%.cpp: src/3rdparty/ptyqt/%.h
	@mkdir -p $(@D)
	$(MOC) $< -o $@

build/3rdparty/%.o: build/3rdparty/%.cpp
	$(CXX) $(VENDOR_CXXFLAGS) -c $< -o $@

# The lock shown instead of the cursor at a password prompt.
build/qrc_terminal.cpp: src/3rdparty/terminal/terminal.qrc src/3rdparty/terminal/images/passwordlock.png | build
	$(RCC) -name terminal $< -o $@

build/qrc_terminal.o: build/qrc_terminal.cpp
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c $< -o $@

build:
	mkdir -p build

# Tests: make check (they need git and run without a display).
TESTS := $(patsubst tests/%.cpp,build/tests/%$(EXE),$(wildcard tests/*_test.cpp))

check: $(TESTS)
	@for t in $(TESTS); do QT_QPA_PLATFORM=offscreen $$t || exit 1; done

build/tests/%$(EXE): tests/%.cpp | build/tests
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) $(KF_CFLAGS) -Isrc -o $@ $(filter %.cpp %.o,$^) $(QT_LIBS) $(KF_LIBS)

build/tests/blame_test$(EXE): build/blame.o build/moc_blame.o
build/tests/settings_test$(EXE): build/settings.o
build/tests/conflicts_test$(EXE): build/conflicts.o
build/tests/git_test$(EXE): build/git.o build/moc_git.o build/prettier.o build/settings.o build/platform.o
build/tests/npmpackages_test$(EXE): build/npmpackages.o
build/tests/scopes_test$(EXE): build/scopes.o
build/tests/lspedits_test$(EXE): build/lspedits.o
build/tests/hunks_test$(EXE): build/linediff.o
build/tests/staging_test$(EXE): build/linediff.o build/git.o build/moc_git.o build/prettier.o build/settings.o build/platform.o
build/tests/selfupdate_test$(EXE): build/selfupdate.o
build/tests/singleinstance_test$(EXE): build/singleinstance.o build/platform.o
build/tests/terminalengine_test$(EXE): build/terminalengine.o

build/tests:
	mkdir -p build/tests

install: lieve
	install -Dm755 lieve $(DESTDIR)$(PREFIX)/bin/lieve
	install -Dm644 data/lieve.desktop $(DESTDIR)$(PREFIX)/share/applications/lieve.desktop
	sed -i 's|@BIN@|$(PREFIX)/bin/lieve|g' $(DESTDIR)$(PREFIX)/share/applications/lieve.desktop
	install -Dm644 data/lieve.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/lieve.svg
	$(if $(DESTDIR),,-update-desktop-database $(PREFIX)/share/applications 2>/dev/null)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/lieve
	rm -f $(DESTDIR)$(PREFIX)/share/applications/lieve.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/lieve.svg

clean:
	rm -rf build $(BIN)

.PHONY: all install uninstall clean translations check

-include $(OBJ:.o=.d)
