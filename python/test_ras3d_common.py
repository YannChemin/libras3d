"""
test_ras3d_common.py — shared fixtures and helpers for i.hyper.* libras3d tests.

Import this from any testsuite/test_ras3d_*.py test file:

    from test_ras3d_common import (
        WYVERN_PATH, TANAGER_PATH, RAS3D_AVAILABLE,
        skip_without_ras3d, skip_without_wyvern, skip_without_tanager,
        open_cube_checked, assert_band_valid, load_addon,
        make_wl_sidecar, remove_wl_sidecar,
    )
"""
import os
import sys
import pytest
import numpy as np

# Run against the libras3d *source* shim, not whatever .deb was last
# installed: the system copy at /usr/lib/python3/dist-packages is root-owned
# and only refreshed by rebuilding the package, so on a development machine
# it drifts behind the tree under test. RAS3D_SHIM_DIR overrides, and an
# empty value falls back to the installed copy.
_SHIM_DIR = os.environ.get(
    'RAS3D_SHIM_DIR',
    os.path.join(os.path.expanduser('~'), 'dev', 'libras3d', 'python'),
)
if _SHIM_DIR and os.path.isdir(_SHIM_DIR) and _SHIM_DIR not in sys.path:
    sys.path.insert(0, _SHIM_DIR)

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


def load_addon(addon, module_dir=None):
    """Import an i.hyper.* addon whose filename contains dots.

    ``i.hyper.albedo.py`` is not an importable module name, so
    ``import_module('i_hyper_albedo')`` can never resolve it however
    ``sys.path`` is set. Load it by file path instead and register it under
    the underscored name, so dataclasses and pickling inside the module
    resolve their own ``__module__``.

    :param str addon: the addon's dotted name, e.g. ``i.hyper.albedo``
    :param str module_dir: folder holding the ``.py``; defaults to
        ``$HOME/dev/<addon>``, overridable with ``RAS3D_ADDON_DIR``
    :return: the imported module
    """
    import importlib.util

    underscored = addon.replace('.', '_')
    if underscored in sys.modules:
        return sys.modules[underscored]

    if module_dir is None:
        module_dir = os.environ.get(
            'RAS3D_ADDON_DIR',
            os.path.join(os.path.expanduser('~'), 'dev', addon),
        )
    path = os.path.join(module_dir, addon + '.py')
    if not os.path.exists(path):
        pytest.skip(f'Addon source not found: {path}')

    install_ras3d_shim()
    if module_dir not in sys.path:
        sys.path.insert(0, module_dir)

    spec = importlib.util.spec_from_file_location(underscored, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[underscored] = mod
    try:
        spec.loader.exec_module(mod)
    except Exception:
        del sys.modules[underscored]
        raise
    return mod


def remove_wl_sidecar(path):
    """Remove a sidecar written by make_wl_sidecar(), tolerating absence.

    The sidecar path is dictated by the contract under test -- the cube's
    own path with the extension replaced by ``.wl.json`` -- so every suite
    touching the same cube writes and deletes the *same* file. Two suites
    running at once therefore race, and the loser used to fail with
    FileNotFoundError on cleanup rather than on anything it was testing.
    Cleanup is not the assertion, so missing is not an error here.

    :param str path: the sidecar path returned by make_wl_sidecar()
    """
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def make_wl_sidecar(cube_path, n_bands, start_nm=400.0, step_nm=10.0):
    """Write a synthetic .wl.json sidecar for a test cube.

    The path is fixed by the contract being tested, so it is shared by
    every suite that uses the same cube. Pair this with
    remove_wl_sidecar() rather than os.unlink(), and do not run two
    cube-sharing suites concurrently.
    """
    import json
    base = cube_path.removesuffix('.tiff').removesuffix('.tif')\
                    .removesuffix('.h5').removesuffix('.hdf5')
    wl = [start_nm + i * step_nm for i in range(n_bands)]
    sidecar = base + '.wl.json'
    with open(sidecar, 'w') as f:
        json.dump(wl, f)
    return sidecar, wl
