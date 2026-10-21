# um1x-macos - user-space CoreMIDI driver for the Roland/Edirol UM-1X.
#
#   make             build build/um1x-midi-driver (universal: arm64 + x86_64)
#   make test        run the unit tests
#   make install     install for this user and start it at login
#   make uninstall   stop it and remove everything make install put in place
#   make dist        release archive + checksum in build/dist
#
# libusb is downloaded once (checksum verified), built as a static universal
# library and linked in, so the binary depends on nothing but macOS.
# To use an installed libusb instead: make LIBUSB_PREFIX=/opt/homebrew ARCHS=arm64

VERSION     := 1.0.0
BUNDLE_ID   := io.github.mihasm.um1x-midi-driver
ARCHS       ?= arm64 x86_64
MACOS_MIN   ?= 11.0

LIBUSB_VER  := 1.0.30
LIBUSB_SHA  := fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
LIBUSB_URL  := https://github.com/libusb/libusb/releases/download/v$(LIBUSB_VER)/libusb-$(LIBUSB_VER).tar.bz2

BUILD       := build
DEPS        := $(BUILD)/deps
LIBUSB_PREFIX ?= $(DEPS)/libusb
LIBUSB_A    := $(LIBUSB_PREFIX)/lib/libusb-1.0.a

CC          := clang
ARCHFLAGS   := $(foreach a,$(ARCHS),-arch $(a)) -mmacosx-version-min=$(MACOS_MIN)
CFLAGS      ?= -O2 -g
CFLAGS      += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fblocks \
               -DUM1X_VERSION='"$(VERSION)"' -I$(LIBUSB_PREFIX)/include/libusb-1.0
FRAMEWORKS  := -framework CoreMIDI -framework CoreFoundation -framework IOKit -framework Security -lobjc

SRC         := src/main.c src/driver.c src/usbmidi.c src/devices.c src/tools.c src/log.c
HDR         := $(wildcard src/*.h)
BIN         := $(BUILD)/um1x-midi-driver
DIST_NAME   := um1x-midi-driver-$(VERSION)
ARCHIVE     := um1x-midi-driver-macos-universal.tar.gz

.PHONY: all test install uninstall dist clean distclean

all: $(BIN)

$(BUILD)/Info.plist: packaging/Info.plist.in Makefile
	@mkdir -p $(BUILD)
	sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@BUNDLE_ID@/$(BUNDLE_ID)/g' $< > $@

$(BIN): $(SRC) $(HDR) $(LIBUSB_A) $(BUILD)/Info.plist
	$(CC) $(CFLAGS) $(ARCHFLAGS) -o $@ $(SRC) $(LIBUSB_A) $(FRAMEWORKS) \
		-Wl,-sectcreate,__TEXT,__info_plist,$(BUILD)/Info.plist
	codesign --force --sign - --identifier $(BUNDLE_ID) $@

$(DEPS)/libusb/lib/libusb-1.0.a:
	@mkdir -p $(DEPS)
	curl -fsSL -o $(DEPS)/libusb.tar.bz2 $(LIBUSB_URL)
	echo "$(LIBUSB_SHA)  $(DEPS)/libusb.tar.bz2" | shasum -a 256 -c -
	rm -rf $(DEPS)/libusb-src && mkdir -p $(DEPS)/libusb-src
	tar -xjf $(DEPS)/libusb.tar.bz2 -C $(DEPS)/libusb-src --strip-components 1
	cd $(DEPS)/libusb-src && ./configure --disable-shared --enable-static --quiet \
		--prefix="$(abspath $(DEPS)/libusb)" CC=clang CFLAGS="-O2 $(ARCHFLAGS)"
	$(MAKE) -C $(DEPS)/libusb-src -j8 install

$(BUILD)/test_usbmidi: tests/test_usbmidi.c src/usbmidi.c src/usbmidi.h
	@mkdir -p $(BUILD)
	$(CC) -std=gnu11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -o $@ tests/test_usbmidi.c src/usbmidi.c

test: $(BUILD)/test_usbmidi
	$(BUILD)/test_usbmidi

install: $(BIN)
	scripts/install.sh $(BIN)

uninstall:
	scripts/uninstall.sh

dist: $(BIN) test
	rm -rf $(BUILD)/dist/$(DIST_NAME) && mkdir -p $(BUILD)/dist/$(DIST_NAME)
	cp $(BIN) scripts/install.sh scripts/uninstall.sh README.md LICENSE $(BUILD)/dist/$(DIST_NAME)/
	cp packaging/THIRD-PARTY-NOTICES.md $(BUILD)/dist/$(DIST_NAME)/
	cp $(DEPS)/libusb-src/COPYING $(BUILD)/dist/$(DIST_NAME)/LICENSE.libusb
	cd $(BUILD)/dist && tar -czf $(ARCHIVE) $(DIST_NAME)
	cd $(BUILD)/dist && shasum -a 256 $(ARCHIVE) > $(ARCHIVE).sha256
	cd $(BUILD)/dist/$(DIST_NAME) && shasum -a 256 um1x-midi-driver > ../um1x-midi-driver.sha256
	cp $(BIN) $(BUILD)/dist/um1x-midi-driver
	@cat $(BUILD)/dist/*.sha256

clean:
	rm -rf $(BUILD)/um1x-midi-driver $(BUILD)/test_usbmidi* $(BUILD)/Info.plist $(BUILD)/dist $(BUILD)/*.dSYM

distclean:
	rm -rf $(BUILD)
