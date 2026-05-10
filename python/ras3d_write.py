"""
ras3d_write.py — write 2D and 3D GeoTIFF outputs from NumPy arrays,
copying CRS and geotransform from an open ras3d cube handle.

Used by i.hyper.* modules in standalone (DEBIAN_BUILD / ras3d) mode.
"""
import numpy as np


def _geo_from_handle(handle):
    """Return (geotransform_tuple, epsg_or_None) from a ras3d cube handle."""
    import ras3d
    r = ras3d.get_region(handle)
    # GDAL geotransform: (west, ew_res, 0, north, 0, -ns_res)
    gt = (r['west'], r['ew_res'], 0.0, r['north'], 0.0, -r['ns_res'])
    return gt, r


def write_raster2d(path: str, data: np.ndarray, handle,
                   nodata: float = float('nan'), dtype=None) -> None:
    """
    Write a 2-D NumPy array as a single-band LZW GeoTIFF.

    :param path:    output file path (`.tif` appended if missing)
    :param data:    2-D array [rows, cols]
    :param handle:  open ras3d cube handle (provides geo-metadata)
    :param nodata:  nodata value written to the file
    :param dtype:   output dtype (default: float32)
    """
    from osgeo import gdal, osr

    if not path.endswith(('.tif', '.tiff')):
        path = path + '.tif'

    if dtype is None:
        dtype = np.float32
    data = np.asarray(data, dtype=dtype)

    gt, r = _geo_from_handle(handle)
    rows, cols = data.shape

    drv = gdal.GetDriverByName('GTiff')
    opts = ['COMPRESS=LZW', 'PREDICTOR=2', 'TILED=YES',
            'BLOCKXSIZE=512', 'BLOCKYSIZE=512', 'BIGTIFF=IF_SAFER']
    gdal_type = _np2gdal(dtype)
    ds = drv.Create(path, cols, rows, 1, gdal_type, opts)
    ds.SetGeoTransform(gt)

    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)   # fallback WGS84; ras3d does not yet expose EPSG
    ds.SetProjection(srs.ExportToWkt())

    band = ds.GetRasterBand(1)
    band.WriteArray(data)
    band.SetNoDataValue(float(nodata) if not (nodata != nodata) else -9999.0)
    band.FlushCache()
    ds = None  # close


def write_raster3d(path: str, cube: np.ndarray, handle,
                   wavelengths: list | None = None,
                   nodata: float = float('nan')) -> None:
    """
    Write a 3-D NumPy array [bands, rows, cols] as a multi-band LZW GeoTIFF.
    Also writes a .wl.json sidecar if wavelengths are provided.

    :param path:        output file path (`.tif` appended if missing)
    :param cube:        3-D float32 array [bands, rows, cols]
    :param handle:      open ras3d cube handle
    :param wavelengths: list of band-centre wavelengths in nm (optional)
    :param nodata:      nodata value
    """
    from osgeo import gdal, osr

    if not path.endswith(('.tif', '.tiff')):
        path = path + '.tif'

    cube = np.asarray(cube, dtype=np.float32)
    bands, rows, cols = cube.shape
    gt, r = _geo_from_handle(handle)

    drv = gdal.GetDriverByName('GTiff')
    opts = ['COMPRESS=LZW', 'PREDICTOR=2', 'TILED=YES',
            'BLOCKXSIZE=512', 'BLOCKYSIZE=512', 'BIGTIFF=IF_SAFER']
    ds = drv.Create(path, cols, rows, bands, gdal.GDT_Float32, opts)
    ds.SetGeoTransform(gt)

    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    ds.SetProjection(srs.ExportToWkt())

    nd = float(nodata) if not (nodata != nodata) else -9999.0
    for b in range(bands):
        band = ds.GetRasterBand(b + 1)
        band.WriteArray(cube[b])
        band.SetNoDataValue(nd)
        if wavelengths and b < len(wavelengths):
            band.SetDescription(f"{wavelengths[b]:.4f} nm")
        band.FlushCache()
    ds = None

    if wavelengths:
        base = path.removesuffix('.tiff').removesuffix('.tif')
        wl_path = base + '.wl.json'
        import json
        with open(wl_path, 'w') as f:
            json.dump([round(float(w), 6) for w in wavelengths], f)


def _np2gdal(dtype):
    from osgeo import gdal
    m = {
        np.float32: gdal.GDT_Float32,
        np.float64: gdal.GDT_Float64,
        np.int32:   gdal.GDT_Int32,
        np.int16:   gdal.GDT_Int16,
        np.uint8:   gdal.GDT_Byte,
    }
    return m.get(np.dtype(dtype).type, gdal.GDT_Float32)


def outpath(name: str, outdir: str | None = None) -> str:
    """Resolve an output map name to a .tif file path, honouring RAS3D_OUTDIR."""
    import os
    if outdir is None:
        outdir = os.environ.get('RAS3D_OUTDIR', '')
    if outdir:
        base = os.path.basename(name)
        name = os.path.join(outdir, base)
    if not name.endswith(('.tif', '.tiff')):
        name = name + '.tif'
    return name
