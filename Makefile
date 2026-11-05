# Lieve — build without CMake: Qt 6 via pkg-config, KDE Frameworks 6 headers
# from /usr/include/KF6. Usage: make && make install (to ~/.local).

PREFIX ?= $(HOME)/.local
CXX ?= g++
CXXFLAGS ?= -O2 -pipe
CXXFLAGS += -std=c++20 -fPIC -Wall -Wextra -Wno-unused-parameter

MOC ?= $(firstword $(wildcard /usr/lib/qt6/moc /usr/lib/qt6/libexec/moc /usr/libexec/qt6/moc /usr/lib64/qt6/libexec/moc))
RCC ?= $(firstword $(wildcard /usr/lib/qt6/rcc /usr/lib/qt6/libexec/rcc /usr/libexec/qt6/rcc /usr/lib64/qt6/libexec/rcc))
QT_CFLAGS := $(shell pkg-config --cflags Qt6Widgets Qt6DBus Qt6Network)
QT_LIBS := $(shell pkg-config --libs Qt6Widgets Qt6DBus Qt6Network)
KF_CFLAGS := $(addprefix -I,$(patsubst %/,%,$(wildcard /usr/include/KF6/*/)))
KF_LIBS := -lKF6TextEditor -lKF6Parts -lKF6XmlGui -lKF6CoreAddons -lKF6DBusAddons -lKF6WindowSystem -lKF6SyntaxHighlighting -lKF6ConfigCore -lKF6WidgetsAddons

SRC := $(wildcard src/*.cpp)
MOC_HEADERS := $(shell grep -l Q_OBJECT src/*.h)
MOC_SRC := $(patsubst src/%.h,build/moc_%.cpp,$(MOC_HEADERS))
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC)) $(MOC_SRC:.cpp=.o) build/qrc_lieve.o

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

build/moc_%.o: build/moc_%.cpp
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) $(KF_CFLAGS) -Isrc -c $< -o $@

build:
	mkdir -p build

install: lieve
	install -Dm755 lieve $(DESTDIR)$(PREFIX)/bin/lieve
	install -Dm644 data/lieve.desktop $(DESTDIR)$(PREFIX)/share/applications/lieve.desktop
	sed -i 's|@BIN@|$(PREFIX)/bin/lieve|g' $(DESTDIR)$(PREFIX)/share/applications/lieve.desktop
	install -Dm644 data/lieve.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/lieve.svg
	-update-desktop-database $(DESTDIR)$(PREFIX)/share/applications 2>/dev/null

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/lieve
	rm -f $(DESTDIR)$(PREFIX)/share/applications/lieve.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/lieve.svg

clean:
	rm -rf build lieve

.PHONY: all install uninstall clean

-include $(OBJ:.o=.d)
