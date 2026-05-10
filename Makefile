# Top-level Makefile for libras3d
# Wraps CMake (for development builds) and dpkg-buildpackage (for Debian packages).
#
# Targets:
#   make              — CMake Release build in ./build/
#   make debug        — CMake Debug build in ./build/
#   make test         — run ctest (requires data files)
#   make install      — cmake --install build (default PREFIX=/usr/local)
#   make deb          — build Debian binary packages (no signing)
#   make deb-src      — build Debian source + binary packages
#   make clean        — remove ./build/
#   make distclean    — remove ./build/ and debian build artefacts

BUILD_DIR  ?= build
PREFIX     ?= /usr/local
BUILD_TYPE ?= Release

# ── CMake targets ─────────────────────────────────────────────────────────────

.PHONY: all debug test install clean distclean deb deb-src

all: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) -j$$(nproc)

debug:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD_DIR) -j$$(nproc)

$(BUILD_DIR)/Makefile:
	cmake -B $(BUILD_DIR) \
	      -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	      -DCMAKE_INSTALL_PREFIX=$(PREFIX)

test: all
	cd $(BUILD_DIR) && ctest --output-on-failure

install: all
	cmake --install $(BUILD_DIR)
	ldconfig

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf debian/.debhelper debian/tmp \
	       debian/libras3d debian/libras3d-dev debian/libras3d-python \
	       debian/files debian/*.substvars debian/debhelper-build-stamp \
	       obj-x86_64-linux-gnu

# ── Debian package targets ─────────────────────────────────────────────────────

deb:
	dpkg-buildpackage -b -us -uc
	@echo ""
	@echo "Packages built in parent directory:"
	@ls ../libras3d*.deb 2>/dev/null

deb-src:
	dpkg-buildpackage -us -uc
	@echo ""
	@echo "Packages built in parent directory:"
	@ls ../libras3d*.deb ../libras3d*.dsc 2>/dev/null
