"""
ras3d.py — ctypes wrapper for libras3d.

Usage:
    from ras3d import open_cube, get_band, get_region

    cube = open_cube("/path/to/data.h5")          # or .tif/.tiff
    region = get_region(cube)
    band0 = get_band(cube, 0)                     # numpy float32 array
    close_cube(cube)
"""
import ctypes
import ctypes.util
import os
import numpy as np
from pathlib import Path

# ── load shared library ────────────────────────────────────────────────────
def _find_lib():
    # 1. adjacent build directory
    here = Path(__file__).parent.parent
    for candidate in [
        here / "build" / "libras3d.so",
        here / "build" / "src" / "libras3d.so",
        Path("/usr/local/lib/libras3d.so"),
        Path("/usr/lib/libras3d.so"),
    ]:
        if candidate.exists():
            return str(candidate)
    name = ctypes.util.find_library("ras3d")
    if name:
        return name
    raise OSError("libras3d.so not found. Build with cmake and install or "
                  "set LD_LIBRARY_PATH to the build directory.")

_lib = ctypes.CDLL(_find_lib())

# ── C type definitions ─────────────────────────────────────────────────────
class RASTER3D_Region(ctypes.Structure):
    _fields_ = [
        ("north",  ctypes.c_double),
        ("south",  ctypes.c_double),
        ("east",   ctypes.c_double),
        ("west",   ctypes.c_double),
        ("top",    ctypes.c_double),
        ("bottom", ctypes.c_double),
        ("rows",   ctypes.c_int),
        ("cols",   ctypes.c_int),
        ("depths", ctypes.c_int),
        ("ns_res", ctypes.c_double),
        ("ew_res", ctypes.c_double),
        ("tb_res", ctypes.c_double),
        ("proj",   ctypes.c_int),
        ("zone",   ctypes.c_int),
    ]

# Opaque pointer
RASTER3D_Map_p = ctypes.c_void_p

# G_find_raster3d
_lib.G_find_raster3d.restype  = ctypes.c_char_p
_lib.G_find_raster3d.argtypes = [ctypes.c_char_p, ctypes.c_char_p]

# Rast3d_get_window
_lib.Rast3d_get_window.restype  = None
_lib.Rast3d_get_window.argtypes = [ctypes.POINTER(RASTER3D_Region)]

# Rast3d_open_cell_old
_lib.Rast3d_open_cell_old.restype  = RASTER3D_Map_p
_lib.Rast3d_open_cell_old.argtypes = [
    ctypes.c_char_p, ctypes.c_char_p,
    ctypes.POINTER(RASTER3D_Region),
    ctypes.c_int, ctypes.c_int,
]

# Rast3d_get_region_struct_map
_lib.Rast3d_get_region_struct_map.restype  = None
_lib.Rast3d_get_region_struct_map.argtypes = [
    RASTER3D_Map_p, ctypes.POINTER(RASTER3D_Region),
]

# Rast3d_get_block
DCELL_TYPE = 2
_lib.Rast3d_get_block.restype  = None
_lib.Rast3d_get_block.argtypes = [
    RASTER3D_Map_p,
    ctypes.c_int, ctypes.c_int, ctypes.c_int,   # x0, y0, z0
    ctypes.c_int, ctypes.c_int, ctypes.c_int,   # nx, ny, nz
    ctypes.c_void_p, ctypes.c_int,              # buf, type
]

# Rast3d_close
_lib.Rast3d_close.restype  = ctypes.c_int
_lib.Rast3d_close.argtypes = [RASTER3D_Map_p]

# G_malloc / G_free
_lib.G_malloc.restype  = ctypes.c_void_p
_lib.G_malloc.argtypes = [ctypes.c_size_t]
_lib.G_free.restype    = None
_lib.G_free.argtypes   = [ctypes.c_void_p]

# ── public API ─────────────────────────────────────────────────────────────

def open_cube(path: str) -> RASTER3D_Map_p:
    """Open a hyperspectral cube (GeoTIFF or HDF5). Returns an opaque handle."""
    bpath = os.fsencode(path)
    mapset = _lib.G_find_raster3d(bpath, b"")
    if not mapset:
        raise FileNotFoundError(f"ras3d: cannot find '{path}'")
    region = RASTER3D_Region()
    _lib.Rast3d_get_window(ctypes.byref(region))
    RASTER3D_NO_CACHE      = 0
    RASTER3D_TILE_SAME_AS_FILE = 2
    handle = _lib.Rast3d_open_cell_old(
        bpath, mapset, ctypes.byref(region),
        RASTER3D_TILE_SAME_AS_FILE, RASTER3D_NO_CACHE,
    )
    if not handle:
        raise RuntimeError(f"ras3d: failed to open '{path}'")
    return handle


def get_region(handle: RASTER3D_Map_p) -> dict:
    """Return a dict with rows, cols, depths, north, south, east, west, ..."""
    region = RASTER3D_Region()
    _lib.Rast3d_get_region_struct_map(handle, ctypes.byref(region))
    return {
        "rows":   region.rows,
        "cols":   region.cols,
        "depths": region.depths,
        "north":  region.north,
        "south":  region.south,
        "east":   region.east,
        "west":   region.west,
        "ns_res": region.ns_res,
        "ew_res": region.ew_res,
        "top":    region.top,
        "bottom": region.bottom,
        "tb_res": region.tb_res,
    }


def get_band(handle: RASTER3D_Map_p, z: int, region: dict | None = None) -> np.ndarray:
    """Read one band (z-slice) and return a float32 numpy array [rows, cols]."""
    if region is None:
        region = get_region(handle)
    rows = region["rows"]
    cols = region["cols"]
    buf = np.empty((rows, cols), dtype=np.float64)
    _lib.Rast3d_get_block(
        handle,
        0, 0, z,
        cols, rows, 1,
        buf.ctypes.data_as(ctypes.c_void_p),
        DCELL_TYPE,
    )
    return buf.astype(np.float32)


def read_all_bands(handle: RASTER3D_Map_p,
                   region: dict | None = None) -> np.ndarray:
    """Read entire cube into float32 array [bands, rows, cols]."""
    if region is None:
        region = get_region(handle)
    bands = region["depths"]
    rows  = region["rows"]
    cols  = region["cols"]
    cube = np.empty((bands, rows, cols), dtype=np.float32)
    for z in range(bands):
        cube[z] = get_band(handle, z, region)
    return cube


def close_cube(handle: RASTER3D_Map_p) -> None:
    """Close the cube handle."""
    _lib.Rast3d_close(handle)
