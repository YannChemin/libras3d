# ras3d

> **GitHub**: <https://github.com/yannchemin/libras3d>

Standalone C+OpenMP library that reimplements the GRASS GIS `raster3d` API for use outside GRASS GIS. It reads hyperspectral cubes from **GeoTIFF** (multi-band) and **HDF5** (HDF-EOS SWATH layout) and writes results as LZW-compressed GeoTIFF. A Python ctypes wrapper and a drop-in header let existing `i.hyper.*` GRASS modules — in particular [i.hyper.atcorr](https://github.com/yannchemin/i.hyper.atcorr) — run directly on file paths without any GRASS installation.

## Features

- **Drop-in GRASS API** — `<ras3d/ras3d.h>` replaces `<grass/gis.h>`, `<grass/glocale.h>`, `<grass/raster.h>`, and `<grass/raster3d.h>` with identical struct layouts and function signatures.
- **GeoTIFF backend** — handles tiled/strip, contiguous/separate planar configs, any bit depth and sample format; reads via `libtiff` + `libgeotiff`.
- **HDF5 backend** — auto-detects radiance/reflectance datasets (name-contains match); falls back to `$RAS3D_HDF5_DATASET` override; extracts geo-extent from co-located Lat/Lon arrays.
- **Output** — always GeoTIFF, LZW-compressed, 512×512 tiles, `PLANARCONFIG_SEPARATE` (one plane per band).
- **OpenMP** — band-level and tile-level parallelism for compute-heavy operations; HDF5 I/O stays serial (library is not thread-safe).
- **Python wrapper** — `python/ras3d.py` (ctypes) exposes `open_cube`, `get_band`, `get_region`, `read_all_bands` returning NumPy arrays.

## Verified data sources

| Sensor | Format | Dimensions |
|---|---|---|
| Wyvern Dragonette | GeoTIFF (BigTIFF, tiled, contiguous) | 23 bands × 7825 rows × 6003 cols |
| Tanager (Planet) | HDF-EOS5 (HDF5) | 426 bands × 732 rows × 607 cols |

## Quick start

### Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Run tests

```sh
cd build
ctest --output-on-failure
```

### Use from C

```c
#include <ras3d/ras3d.h>

RASTER3D_Region region;
Rast3d_get_window(&region);
RASTER3D_Map *map = Rast3d_open_cell_old(
    "/path/to/data.h5", ".", &region,
    RASTER3D_TILE_SAME_AS_FILE, RASTER3D_NO_CACHE);
Rast3d_get_region_struct_map(map, &region);

DCELL *band = G_malloc((size_t)region.rows * region.cols * sizeof(DCELL));
Rast3d_get_block(map, 0, 0, 0, region.cols, region.rows, 1, band, DCELL_TYPE);
Rast3d_close(map);
```

### Use from Python

```python
from ras3d import open_cube, get_band, get_region, close_cube

cube   = open_cube("/path/to/data.tiff")
region = get_region(cube)
band0  = get_band(cube, 0)        # numpy float32 [rows, cols]
close_cube(cube)
```

### [i.hyper.atcorr](https://github.com/yannchemin/i.hyper.atcorr) integration

Compile with `-DHAVE_RAS3D` and point to the ras3d headers:

```sh
gcc -DHAVE_RAS3D -I/usr/include/ras3d \
    main.c ... -lras3d -lgeotiff -ltiff -lhdf5 -lm -fopenmp
```

The module will detect `HAVE_RAS3D` and use ras3d instead of GRASS headers.  
Set environment variables to control file lookup:

| Variable | Purpose |
|---|---|
| `RAS3D_PATH` | Search directory for input maps (by name, without extension) |
| `RAS3D_OUTDIR` | Directory for output GeoTIFF files |
| `RAS3D_HDF5_DATASET` | Override HDF5 dataset path (e.g. `/HDFEOS/SWATHS/HYP/Data Fields/toa_radiance`) |
| `RAS3D_VERBOSE` | Verbosity level (0 = quiet, 1 = default, 2 = verbose) |
| `GISDBASE`, `LOCATION_NAME`, `MAPSET` | Emulate GRASS location (used by wavelength metadata helpers) |

## Wavelength metadata

ras3d stores band wavelengths in a sidecar JSON file `<mapname>.wl.json`:

```json
[0.40, 0.41, 0.42, ..., 2.50]
```

`ras3d_read_wavelengths()` and `ras3d_write_wavelengths()` handle this.  
The same file is read by [`i.hyper.atcorr`](https://github.com/yannchemin/i.hyper.atcorr) when looking for band centre wavelengths.

## Dependencies

| Library | Debian package | Purpose |
|---|---|---|
| libtiff | `libtiff-dev` | TIFF I/O |
| libgeotiff | `libgeotiff-dev` | Geo-reference metadata |
| libhdf5 (serial) | `libhdf5-dev` | HDF5 I/O |
| OpenMP | `gcc` / `libomp-dev` | Parallelism |

## Directory layout

```
ras3d/
├── include/ras3d/
│   ├── ras3d.h            # Public API (GRASS-compatible)
│   └── ras3d_internal.h   # Internal backend interface
├── src/
│   ├── api.c              # Rast3d_* entry points
│   ├── grass_compat.c     # G_*, Rast_*, G_parser emulation
│   ├── region.c           # RASTER3D_Region helpers
│   ├── backend_geotiff.c  # libtiff + libgeotiff backend
│   └── backend_hdf5.c     # libhdf5 backend
├── python/
│   └── ras3d.py           # ctypes wrapper
├── tests/
│   ├── test_geotiff.c
│   └── test_hdf5.c
├── debian/                # Debian packaging
├── CMakeLists.txt
├── README.md
└── INSTALL.md
```

## Related repositories

| Repository | Relationship | Description |
|---|---|---|
| [i.hyper.atcorr](https://github.com/yannchemin/i.hyper.atcorr) | **Primary consumer** | GRASS hyperspectral atmospheric correction module; uses libras3d for standalone Debian builds (`DEBIAN_BUILD=1`) |
| [libsixsv](https://github.com/yannchemin/libsixsv) | **Peer — Debian standalone** | 6SV2.1 RT physics library; used alongside libras3d when building i.hyper.atcorr without GRASS |

## License

This is free and unencumbered software released into the public domain.  
See <https://unlicense.org> for the full text.

## Notes

- **libhdf5 thread safety**: the serial `libhdf5-dev` is not thread-safe. All `H5D*` calls are made from the main thread; only post-read computation is parallelised with OpenMP. Using `libhdf5-openmpi-dev` would require MPI process setup and is not beneficial on a single machine.
- **GeoTIFF tag include order**: `<geotiff.h>` defines `TYPE_DOUBLE` as an enum member; `<ras3d/ras3d.h>` defines it as a macro (GRASS convention, value=2). In ras3d source files, geotiff headers are always included before ras3d headers.
- **Output format**: always GeoTIFF LZW regardless of input format (HDF5 input → GeoTIFF output).
