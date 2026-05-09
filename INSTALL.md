# Installation

## 1. Install from the Debian package (recommended)

If you have the `.deb` file built from this source tree:

```sh
sudo dpkg -i libras3d_0.1.0-1_amd64.deb
sudo dpkg -i libras3d-dev_0.1.0-1_amd64.deb
sudo dpkg -i libras3d-python_0.1.0-1_all.deb   # optional Python wrapper
```

Or build and install in one step from the source tree:

```sh
cd /home/yann/dev/ras3d
dpkg-buildpackage -b -us -uc
sudo dpkg -i ../libras3d_0.1.0-1_amd64.deb \
              ../libras3d-dev_0.1.0-1_amd64.deb
```

## 2. Build and install from source (CMake)

### 2a. Install build dependencies

```sh
sudo apt-get install -y \
    build-essential cmake \
    libtiff-dev libgeotiff-dev \
    libhdf5-dev \
    python3-numpy
```

### 2b. Configure

```sh
cd /home/yann/dev/ras3d
cmake -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local
```

To install to `/usr` (system-wide, like a Debian package):

```sh
cmake -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr
```

### 2c. Build

```sh
cmake --build build -j$(nproc)
```

### 2d. Run tests

```sh
cd build
ctest --output-on-failure
```

Both `test_geotiff` and `test_hdf5` read from the hard-coded data paths in
`tests/`. If the paths differ on your machine, edit the `#define` at the top
of each test file before building.

### 2e. Install

```sh
sudo cmake --install build
sudo ldconfig          # update the shared-library cache
```

This installs:

| Path | Contents |
|---|---|
| `$PREFIX/lib/libras3d.so` | Shared library |
| `$PREFIX/lib/libras3d.a` | Static library |
| `$PREFIX/include/ras3d/ras3d.h` | Public header |
| `$PREFIX/include/ras3d/ras3d_internal.h` | Internal header (backend authors) |

### 2f. Install the Python wrapper (optional)

```sh
sudo cp python/ras3d.py $(python3 -c "import site; print(site.getsitepackages()[0])")/
```

Or for a per-user install:

```sh
cp python/ras3d.py ~/.local/lib/python3.*/site-packages/
```

## 3. Build the Debian package

From the source tree root:

```sh
# Install packaging tools if needed
sudo apt-get install -y debhelper devscripts

# Build binary packages (no signing)
dpkg-buildpackage -b -us -uc

# Packages appear one level up:
ls ../libras3d*.deb
```

The build produces three packages:

| Package | Contents |
|---|---|
| `libras3d_0.1.0-1_amd64.deb` | Runtime shared library |
| `libras3d-dev_0.1.0-1_amd64.deb` | Headers + static library |
| `libras3d-python_0.1.0-1_all.deb` | Python ctypes wrapper |

## 4. Using ras3d in a project

### CMake

```cmake
find_library(RAS3D_LIB ras3d)
find_path(RAS3D_INCLUDE ras3d/ras3d.h)

target_include_directories(my_target PRIVATE ${RAS3D_INCLUDE})
target_link_libraries(my_target PRIVATE ${RAS3D_LIB})
target_compile_definitions(my_target PRIVATE HAVE_RAS3D)
```

### pkg-config (after installing the Debian package)

A `ras3d.pc` file is installed by the Debian package:

```sh
pkg-config --cflags ras3d   # → -I/usr/include
pkg-config --libs   ras3d   # → -lras3d
```

### Manual compilation

```sh
gcc -DHAVE_RAS3D \
    $(pkg-config --cflags ras3d) \
    main.c \
    $(pkg-config --libs ras3d) \
    -lgeotiff -ltiff -lhdf5 -lm -fopenmp \
    -o my_module
```

## 5. Compiling [i.hyper.atcorr](https://github.com/yannchemin/i.hyper.atcorr) with ras3d

`i.hyper.atcorr/main.c` already contains the `#ifdef HAVE_RAS3D` guard.
To build it standalone (without GRASS GIS):

```sh
cd /home/yann/dev/i.hyper.atcorr

# Build all the supporting .c files that don't use GRASS I/O
# (atcorr.c, solar_table.c, spatial.c, ...) then link:
gcc -DHAVE_RAS3D -O2 -fopenmp \
    $(pkg-config --cflags ras3d) \
    main.c atcorr.c solar_table.c spatial.c adjacency.c \
    surface_model.c uncertainty.c retrieve.c terrain.c \
    oe_invert.c spectral_brdf.c \
    $(pkg-config --libs ras3d) \
    -lgeotiff -ltiff -lhdf5 -lm \
    -o i.hyper.atcorr.standalone
```

Then run:

```sh
export RAS3D_PATH=/home/yann/RSDATA/Wyvern_Dragonette_001_20240808_agri
export RAS3D_OUTDIR=/tmp/output

./i.hyper.atcorr.standalone \
    input=wyvern_dragonette-001_20240808T073501_51b92993.tiff \
    output=wyvern_atcorr \
    sza=45.0 doy=221
```

## 6. Uninstall

### From Debian package

```sh
sudo apt-get remove libras3d libras3d-dev libras3d-python
```

### From CMake install

```sh
sudo cmake --build build --target uninstall
# or manually:
sudo rm -f /usr/local/lib/libras3d.{so,a}
sudo rm -rf /usr/local/include/ras3d
sudo ldconfig
```

## License

This is free and unencumbered software released into the public domain.  
See <https://unlicense.org> for the full text.
