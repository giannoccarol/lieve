# Lieve — build without CMake: Qt 6 via pkg-config, KDE Frameworks 6 headers
# from /usr/include/KF6. Usage: make && make install (to ~/.local).

PREFIX ?= $(HOME)/.local
CXX ?= g++
CXXFLAGS ?= -O2 -pipe
CXXFLAGS += -std=c++20 -fPIC -Wall -Wextra -Wno-unused-parameter
# Distribution packages build with PACKAGED=1: updates then come from the
# package manager, not from Lieve's own updater.
CXXFLAGS += $(if $(PACKAGED),-DLIEVE_PACKAGED)

MOC ?= $(firstword $(wildcard /usr/lib/qt6/moc /usr/lib/qt6/libexec/moc /usr/libexec/qt6/moc /usr/lib64/qt6/libexec/moc))
RCC ?= $(firstword $(wildcard /usr/lib/qt6/rcc /usr/lib/qt6/libexec/rcc /usr/libexec/qt6/rcc /usr/lib64/qt6/libexec/rcc))
QT_BIN := $(shell pkg-config --variable=bindir Qt6Core)
LRELEASE ?= $(firstword $(wildcard $(QT_BIN)/lrelease /usr/lib/qt6/bin/lrelease /usr/lib64/qt6/bin/lrelease) $(shell command -v lrelease-qt6 lrelease6 lrelease 2>/dev/null))
LUPDATE ?= $(firstword $(wildcard $(QT_BIN)/lupdate /usr/lib/qt6/bin/lupdate /usr/lib64/qt6/bin/lupdate) $(shell command -v lupdate-qt6 lupdate6 lupdate 2>/dev/null))
QT_CFLAGS := $(shell pkg-config --cflags Qt6Widgets Qt6DBus Qt6Network)
QT_LIBS := $(shell pkg-config --libs Qt6Widgets Qt6DBus Qt6Network)
KF_CFLAGS := $(addprefix -I,$(patsubst %/,%,$(wildcard /usr/include/KF6/*/)))
KF_LIBS := -lKF6TextEditor -lKF6Parts -lKF6XmlGui -lKF6CoreAddons -lKF6DBusAddons -lKF6WindowSystem -lKF6SyntaxHighlighting -lKF6ConfigCore -lKF6WidgetsAddons -lKF6I18n

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

all: lieve

lieve: $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(QT_LIBS) $(KF_LIBS)

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

build:
	mkdir -p build

# Tests: make check (they need git and run without a display).
TESTS := $(patsubst tests/%.cpp,build/tests/%,$(wildcard tests/*_test.cpp))

check: $(TESTS)
	@for t in $(TESTS); do QT_QPA_PLATFORM=offscreen $$t || exit 1; done

build/tests/%: tests/%.cpp | build/tests
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) $(KF_CFLAGS) -Isrc -o $@ $(filter %.cpp %.o,$^) $(QT_LIBS) $(KF_LIBS)

build/tests/blame_test: build/blame.o build/moc_blame.o
build/tests/settings_test: build/settings.o
build/tests/conflicts_test: build/conflicts.o
build/tests/git_test: build/git.o build/moc_git.o build/prettier.o build/settings.o
build/tests/npmpackages_test: build/npmpackages.o
build/tests/scopes_test: build/scopes.o
build/tests/lspedits_test: build/lspedits.o
build/tests/hunks_test: build/linediff.o
build/tests/staging_test: build/linediff.o build/git.o build/moc_git.o build/prettier.o build/settings.o

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
	rm -rf build lieve

.PHONY: all install uninstall clean translations check

-include $(OBJ:.o=.d)
