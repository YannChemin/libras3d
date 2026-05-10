"""
test_ras3d_common.py — shared fixtures and helpers for i.hyper.* libras3d tests.

Import this from any testsuite/test_ras3d_*.py test file:

    from test_ras3d_common import (
        WYVERN_PATH, TANAGER_PATH, RAS3D_AVAILABLE,
        skip_without_ras3d, skip_without_wyvern, skip_without_tanager,
        open_cube_checked, assert_band_valid,
    )
"""
import os
import sys
import pytest
import numpy as np

# ── data file paths ───────────────────────────────────────────────────────────

WYVERN_PATH = os.environ.get(
    'RAS3D_TEST_WYVERN',
    '/home/yann/RSDATA/Wyvern_Dragonette_001_20240808_agri/'
    'wyvern_dragonette-001_20240808T073501_51b92993.tiff',
)
TANAGER_PATH = os.environ.get(
    'RAS3D_TEST_TANAGER',
    '/home/yann/RSDATA/Tanager/Kanpur/'
    '20250321_054913_40_4001_basic_radiance_hdf5.h5',
)

# ── availability checks ───────────────────────────────────────────────────────

def _check_ras3d():
    try:
        import ras3d          # noqa: F401
        import ras3d_grass_shim  # noqa: F401
        import ras3d_write    # noqa: F401
        return True
    except ImportError:
        return False


RAS3D_AVAILABLE  = _check_ras3d()
WYVERN_PRESENT   = os.path.exists(WYVERN_PATH)
TANAGER_PRESENT  = os.path.exists(TANAGER_PATH)

skip_without_ras3d   = pytest.mark.skipif(not RAS3D_AVAILABLE,
    reason='libras3d Python packages not installed')
skip_without_wyvern  = pytest.mark.skipif(not WYVERN_PRESENT,
    reason=f'Wyvern test file not found: {WYVERN_PATH}')
skip_without_tanager = pytest.mark.skipif(not TANAGER_PRESENT,
    reason=f'Tanager test file not found: {TANAGER_PATH}')
skip_without_data    = pytest.mark.skipif(
    not (WYVERN_PRESENT or TANAGER_PRESENT),
    reason='No test data files found')

# ── helpers ───────────────────────────────────────────────────────────────────

def open_cube_checked(path):
    """Open a cube and return (handle, region). Skips test if file missing."""
    import ras3d
    if not os.path.exists(path):
        pytest.skip(f'Data file not found: {path}')
    h = ras3d.open_cube(path)
    r = ras3d.get_region(h)
    return h, r


def assert_band_valid(arr, context=''):
    """Assert band array is non-trivial (not all-NaN, non-zero range)."""
    assert arr is not None, f"{context}: band array is None"
    assert arr.ndim == 2,   f"{context}: expected 2D array, got shape {arr.shape}"
    finite = arr[np.isfinite(arr)]
    assert len(finite) > 0, f"{context}: band contains no finite values"
    assert finite.max() > finite.min(), f"{context}: band is constant (all {finite.min():.4f})"


def install_ras3d_shim():
    """Install the ras3d grass shim so modules can be imported without GRASS."""
    if 'GISBASE' not in os.environ:
        from ras3d_grass_shim import install
        install()


def make_wl_sidecar(cube_path, n_bands, start_nm=400.0, step_nm=10.0):
    """Write a synthetic .wl.json sidecar for a test cube."""
    import json
    base = cube_path.removesuffix('.tiff').removesuffix('.tif')\
                    .removesuffix('.h5').removesuffix('.hdf5')
    wl = [start_nm + i * step_nm for i in range(n_bands)]
    sidecar = base + '.wl.json'
    with open(sidecar, 'w') as f:
        json.dump(wl, f)
    return sidecar, wl
