"""
ras3d_grass_shim.py — emulates grass.script (gs.*) for i.hyper.* modules
running outside GRASS GIS via libras3d.

Call install() once, before any 'import grass.script as gs', and the fake
modules are inserted into sys.modules so the existing module source code
requires no further changes.
"""
import sys
import os
import re
import argparse
import inspect
import math

# ── gs.* logging ─────────────────────────────────────────────────────────────

_verbose_level = int(os.environ.get('RAS3D_VERBOSE', '1'))


def _fatal(msg, *args):
    if args:
        msg = msg % args
    print(f"FATAL: {msg}", file=sys.stderr)
    sys.exit(1)


def _warning(msg, *args):
    if args:
        msg = msg % args
    print(f"WARNING: {msg}", file=sys.stderr)


def _message(msg, *args):
    if args:
        msg = msg % args
    print(msg)


def _verbose(msg, *args):
    if _verbose_level >= 2:
        if args:
            msg = msg % args
        print(msg)


def _verbosity():
    return _verbose_level


def _percent(done, total, step=1):
    if _verbose_level < 1 or total <= 0:
        return
    pct = int(done * 100 / total)
    print(f"\r{pct:3d}%", end='', flush=True)
    if done >= total:
        print()


# ── gs.parser() ───────────────────────────────────────────────────────────────

# Standard options whose key/type/description are fixed by GRASS
_STD_OPTS = {
    'G_OPT_R3_INPUT':  {'key': 'input',  'type': str,  'required': True,
                        'description': 'Input 3D raster map'},
    'G_OPT_R3_OUTPUT': {'key': 'output', 'type': str,  'required': True,
                        'description': 'Output 3D raster map'},
    'G_OPT_R_INPUT':   {'key': 'input',  'type': str,  'required': True,
                        'description': 'Input raster map'},
    'G_OPT_R_OUTPUT':  {'key': 'output', 'type': str,  'required': True,
                        'description': 'Output raster map'},
    'G_OPT_F_INPUT':   {'key': 'file',   'type': str,  'required': True,
                        'description': 'Input file path'},
    'G_OPT_F_OUTPUT':  {'key': 'file',   'type': str,  'required': True,
                        'description': 'Output file path'},
}

_TYPE_MAP = {'string': str, 'integer': int, 'double': float}


def _parse_option_blocks(script_path):
    """Parse # %option / # %flag / # %module comment blocks from a script."""
    options = []
    flags   = []
    cur     = None
    kind    = None  # 'option' | 'flag' | 'module'

    try:
        with open(script_path) as fh:
            for raw in fh:
                line = raw.strip()
                if not line.startswith('#'):
                    if line and not line.startswith('import') and \
                       not line.startswith('from') and \
                       not line.startswith('"""') and \
                       not line.startswith("'''"):
                        break
                    continue
                line = line.lstrip('#').strip()
                if not line.startswith('%'):
                    continue
                line = line.lstrip('%').strip()

                if line.startswith('option') or line.startswith('option G_OPT'):
                    if cur and kind == 'option':
                        options.append(cur)
                    elif cur and kind == 'flag':
                        flags.append(cur)
                    # Standard option?
                    std_key = line.split()[-1] if ' ' in line else None
                    if std_key and std_key in _STD_OPTS:
                        cur  = dict(_STD_OPTS[std_key])
                        kind = 'option'
                    else:
                        cur  = {'required': False, 'type': str, 'multiple': False}
                        kind = 'option'
                elif line.startswith('flag'):
                    if cur and kind == 'option':
                        options.append(cur)
                    elif cur and kind == 'flag':
                        flags.append(cur)
                    cur  = {'required': False}
                    kind = 'flag'
                elif line.startswith('module') or line.startswith('end'):
                    if cur and kind == 'option':
                        options.append(cur)
                        cur = None
                    elif cur and kind == 'flag':
                        flags.append(cur)
                        cur = None
                elif '=' in line and cur is not None:
                    k, _, v = line.partition('=')
                    k = k.strip()
                    v = v.strip()
                    if k == 'key':
                        cur['key'] = v
                    elif k == 'type':
                        cur['type'] = _TYPE_MAP.get(v.lower(), str)
                    elif k == 'required':
                        cur['required'] = v.lower() in ('yes', 'true', '1')
                    elif k == 'multiple':
                        cur['multiple'] = v.lower() in ('yes', 'true', '1')
                    elif k == 'answer':
                        cur['default'] = v
                    elif k == 'options':
                        cur['choices'] = [x.strip() for x in v.split(',')]
                    elif k == 'description':
                        cur['description'] = v
    except (OSError, StopIteration):
        pass

    if cur and kind == 'option':
        options.append(cur)
    elif cur and kind == 'flag':
        flags.append(cur)

    return options, flags


def _make_parser(script_path):
    opts, flags = _parse_option_blocks(script_path)

    ap = argparse.ArgumentParser(
        description=f"ras3d standalone mode for {os.path.basename(script_path)}",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    seen_keys = set()
    for o in opts:
        key = o.get('key', '')
        if not key or key in seen_keys:
            continue
        seen_keys.add(key)
        kwargs = dict(
            dest=key,
            help=o.get('description', ''),
            default=o.get('default', None),
            required=o.get('required', False) and o.get('default') is None,
            type=o.get('type', str),
        )
        if 'choices' in o:
            kwargs['choices'] = o['choices']
        ap.add_argument(f'--{key}', **kwargs)

    for f in flags:
        fk = f.get('key', '')
        if not fk or fk in seen_keys:
            continue
        seen_keys.add(fk)
        ap.add_argument(f'--flag-{fk}', dest=f'flag_{fk}',
                        action='store_true', default=False,
                        help=f.get('description', ''))
    return ap, opts, flags


def _parser():
    """
    Drop-in replacement for gs.parser().

    Reads # %option blocks from the calling script (sys.argv[0]), builds
    argparse from them, parses sys.argv[1:] accepting both --key=value and
    key=value GRASS-style syntax, and returns (options_dict, flags_dict).
    """
    script = sys.argv[0]
    ap, opt_defs, flag_defs = _make_parser(script)

    # Convert GRASS-style 'key=value' and '-flag' to '--key=value' / '--flag-X'
    cooked = []
    for arg in sys.argv[1:]:
        if arg.startswith('-') and '=' not in arg and len(arg) == 2:
            cooked.append(f'--flag-{arg[1:]}')
        elif '=' in arg and not arg.startswith('-'):
            k, _, v = arg.partition('=')
            cooked.append(f'--{k}={v}')
        else:
            cooked.append(arg)

    ns = ap.parse_args(cooked)
    ns_dict = vars(ns)

    options = {}
    for o in opt_defs:
        key = o.get('key', '')
        if key:
            options[key] = ns_dict.get(key) or o.get('default') or ''
            if options[key] is None:
                options[key] = ''
            else:
                options[key] = str(options[key])

    flags = {}
    for f in flag_defs:
        fk = f.get('key', '')
        if fk:
            flags[fk] = ns_dict.get(f'flag_{fk}', False)

    return options, flags


# ── gs.raster3d_info ──────────────────────────────────────────────────────────

def _raster3d_info(name):
    """Return a dict compatible with gs.raster3d_info() using ras3d."""
    import ras3d
    handle = ras3d.open_cube(name)
    r = ras3d.get_region(handle)
    ras3d.close_cube(handle)
    return {
        'rows':   r['rows'],
        'cols':   r['cols'],
        'depths': r['depths'],
        'north':  r['north'],
        'south':  r['south'],
        'east':   r['east'],
        'west':   r['west'],
        'nsres':  r['ns_res'],
        'ewres':  r['ew_res'],
        'tbres':  r['tb_res'],
        'top':    r['top'],
        'bottom': r['bottom'],
        'min':    None,
        'max':    None,
        'datatype': 'FCELL',
        'mapset': '.',
    }


# ── gs.run_command / read_command / parse_command ─────────────────────────────

def _run_command(prog, **kwargs):
    """
    Handle GRASS command invocations in ras3d mode.
    Cleanup commands are silently skipped; data commands raise NotImplementedError.
    """
    silent = {'g.remove', 'g.copy', 'g.rename', 'r.support', 'r3.support',
              'g.region', 'r.rescale', 'i.group'}
    if prog in silent:
        return 0
    raise NotImplementedError(
        f"ras3d_grass_shim: '{prog}' is not supported in standalone mode. "
        "Replace this call with a ras3d-native operation."
    )


def _read_command(prog, **kwargs):
    if prog in ('r3.info',):
        # Caller wants metadata text — return empty so callers fall through
        return ''
    if prog in ('r.support', 'r.info'):
        return ''
    _run_command(prog, **kwargs)
    return ''


def _parse_command(prog, **kwargs):
    if prog in ('r3.info',):
        return {}
    _run_command(prog, **kwargs)
    return {}


def _write_command(prog, stdin='', **kwargs):
    if prog in ('r.support',):
        return 0
    _run_command(prog, **kwargs)
    return 0


def _find_file(name, element='cell', mapset=''):
    """Probe filesystem for a raster file, returns dict like gs.find_file()."""
    for ext in ('', '.tif', '.tiff', '.h5', '.hdf5'):
        path = name + ext
        if os.path.exists(path):
            return {'name': name, 'mapset': '.', 'file': path}
        search = os.environ.get('RAS3D_PATH', '')
        if search:
            p = os.path.join(search, name + ext)
            if os.path.exists(p):
                return {'name': name, 'mapset': search, 'file': p}
    return {'name': '', 'mapset': '', 'file': ''}


# ── fake grass.script module ──────────────────────────────────────────────────

class _FakeGrassScript:
    fatal          = staticmethod(_fatal)
    warning        = staticmethod(_warning)
    message        = staticmethod(_message)
    verbose        = staticmethod(_verbose)
    verbosity      = staticmethod(_verbosity)
    percent        = staticmethod(_percent)
    parser         = staticmethod(_parser)
    raster3d_info  = staticmethod(_raster3d_info)
    run_command    = staticmethod(_run_command)
    read_command   = staticmethod(_read_command)
    parse_command  = staticmethod(_parse_command)
    write_command  = staticmethod(_write_command)
    find_file      = staticmethod(_find_file)

    # convenience aliases used by some modules
    @staticmethod
    def error(msg, *a):
        _warning(msg, *a)

    @staticmethod
    def info(msg, *a):
        _message(msg, *a)


class _FakeGrassArray:
    """Minimal grass.script.array shim — returns numpy-backed arrays."""
    class array:
        def __init__(self):
            import numpy as _np
            self._data = None

        def read(self, mapname):
            """Read a 2D raster by name from ras3d band cache or file."""
            import numpy as _np
            data = _ras3d_band_cache.get(mapname)
            if data is not None:
                self._data = data
                return
            # Try as a GeoTIFF
            try:
                from osgeo import gdal
                ds = gdal.Open(mapname if '.' in mapname else mapname + '.tif')
                if ds:
                    self._data = ds.ReadAsArray().astype(_np.float32)
                    return
            except Exception:
                pass
            raise FileNotFoundError(f"ras3d_grass_shim: cannot read raster '{mapname}'")

        def __array__(self, dtype=None):
            import numpy as _np
            return self._data if dtype is None else self._data.astype(dtype)

        def __len__(self):
            return len(self._data) if self._data is not None else 0


class _FakeGrassExceptions:
    class CalledModuleError(RuntimeError):
        pass
    class ScriptError(RuntimeError):
        pass


class _FakeGrassPackage:
    script     = _FakeGrassScript()
    exceptions = _FakeGrassExceptions()


# ── band cache (shared between shim and modules) ──────────────────────────────

_ras3d_band_cache: dict = {}


def get_band_cache():
    return _ras3d_band_cache


# ── install() — called once before any 'import grass.script as gs' ────────────

def install():
    """Insert fake grass.* modules into sys.modules."""
    pkg = _FakeGrassPackage()
    sys.modules.setdefault('grass',                 pkg)
    sys.modules.setdefault('grass.script',          _FakeGrassScript())
    sys.modules.setdefault('grass.script.array',    _FakeGrassArray())
    sys.modules.setdefault('grass.exceptions',      _FakeGrassExceptions())
    sys.modules.setdefault('grass.pygrass',         pkg)
    sys.modules.setdefault('grass.pygrass.raster',  pkg)
    sys.modules.setdefault('grass.pygrass.raster.buffer', pkg)
