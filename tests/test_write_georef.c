/*
 * test_write_georef.c — outputs written by ras3d must carry their pixel
 * values and the georeferencing of the input cube.
 *
 * Self-contained: writes a small 3-band GeoTIFF cube (projected UTM and
 * geographic WGS84 variants), opens it through the GRASS-compatible API,
 * writes CELL and FCELL 2-D rasters and a 3-D map, and reads them back
 * with libtiff/libgeotiff.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <geotiff.h>
#include <geokeys.h>
#include <geovalues.h>
#include <tiffio.h>
#include <xtiffio.h>

#include "ras3d/ras3d.h"

#define ROWS  5
#define COLS  4
#define BANDS 3

static int failures = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void write_input(const char *path, int epsg, int geographic,
                        double west, double north, double res)
{
    TIFF *tif = XTIFFOpen(path, "w");
    GTIF *gtif = GTIFNew(tif);
    double scale[3] = {res, res, 0.0};
    double tie[6] = {0.0, 0.0, 0.0, west, north, 0.0};
    float row[COLS * BANDS];

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)COLS);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)ROWS);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)BANDS);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)32);
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    {
        uint16_t extra[BANDS - 1] = {EXTRASAMPLE_UNSPECIFIED,
                                     EXTRASAMPLE_UNSPECIFIED};

        TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, (uint16_t)(BANDS - 1), extra);
    }
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, (uint32_t)1);
    TIFFSetField(tif, TIFFTAG_GEOPIXELSCALE, 3, scale);
    TIFFSetField(tif, TIFFTAG_GEOTIEPOINTS, 6, tie);
    GTIFKeySet(gtif, GTModelTypeGeoKey, TYPE_SHORT, 1,
               geographic ? ModelTypeGeographic : ModelTypeProjected);
    GTIFKeySet(gtif, GTRasterTypeGeoKey, TYPE_SHORT, 1, RasterPixelIsArea);
    GTIFKeySet(gtif, geographic ? GeographicTypeGeoKey : ProjectedCSTypeGeoKey,
               TYPE_SHORT, 1, epsg);
    GTIFWriteKeys(gtif);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++)
            for (int b = 0; b < BANDS; b++)
                row[c * BANDS + b] = (float)(100 * b + 10 * r + c);
        TIFFWriteScanline(tif, row, (uint32_t)r, 0);
    }
    GTIFFree(gtif);
    XTIFFClose(tif);
}

/* Check size, georeferencing and CRS of a written GeoTIFF. */
static void check_georef(const char *path, int epsg, int geographic,
                         double west, double north, double res)
{
    TIFF *tif = XTIFFOpen(path, "r");
    GTIF *gtif;
    uint32_t w = 0, h = 0;
    uint16_t n = 0;
    double *vals = NULL;
    unsigned short model = 0, code = 0;

    CHECK(tif != NULL, "cannot reopen <%s>", path);
    if (!tif)
        return;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    CHECK(w == COLS && h == ROWS, "<%s> is %ux%u, expected %dx%d", path, w, h,
          COLS, ROWS);

    CHECK(TIFFGetField(tif, TIFFTAG_GEOPIXELSCALE, &n, &vals) && n >= 2 &&
              fabs(vals[0] - res) < 1e-9 && fabs(vals[1] - res) < 1e-9,
          "<%s> pixel scale missing or wrong", path);
    CHECK(TIFFGetField(tif, TIFFTAG_GEOTIEPOINTS, &n, &vals) && n >= 6 &&
              fabs(vals[3] - west) < 1e-9 && fabs(vals[4] - north) < 1e-9,
          "<%s> tie point missing or wrong", path);

    gtif = GTIFNew(tif);
    CHECK(GTIFKeyGet(gtif, GTModelTypeGeoKey, &model, 0, 1) &&
              model == (geographic ? ModelTypeGeographic : ModelTypeProjected),
          "<%s> model type %u", path, model);
    CHECK(GTIFKeyGet(gtif,
                     geographic ? GeographicTypeGeoKey : ProjectedCSTypeGeoKey,
                     &code, 0, 1) &&
              code == epsg,
          "<%s> EPSG %u, expected %d", path, code, epsg);
    GTIFFree(gtif);
    XTIFFClose(tif);
}

static void check_cell_values(const char *path)
{
    TIFF *tif = XTIFFOpen(path, "r");
    int32_t row[COLS];

    if (!tif)
        return;
    for (int r = 0; r < ROWS; r++) {
        CHECK(TIFFReadScanline(tif, row, (uint32_t)r, 0) == 1,
              "<%s> row %d unreadable", path, r);
        for (int c = 0; c < COLS; c++)
            CHECK(row[c] == 10 * r + c, "<%s> (%d,%d) = %d, expected %d",
                  path, r, c, row[c], 10 * r + c);
    }
    XTIFFClose(tif);
}

static void check_fcell_values(const char *path)
{
    TIFF *tif = XTIFFOpen(path, "r");
    float row[COLS];

    if (!tif)
        return;
    for (int r = 0; r < ROWS; r++) {
        CHECK(TIFFReadScanline(tif, row, (uint32_t)r, 0) == 1,
              "<%s> row %d unreadable", path, r);
        for (int c = 0; c < COLS; c++)
            CHECK(row[c] == 0.5f * (10 * r + c), "<%s> (%d,%d) = %g", path, r,
                  c, row[c]);
    }
    XTIFFClose(tif);
}

static void run_case(const char *dir, const char *tag, int epsg,
                     int geographic, double west, double north, double res)
{
    char in[1024], out[1024];
    RASTER3D_Region region;
    RASTER3D_Map *map;
    CELL cbuf[COLS];
    FCELL fbuf[COLS];
    int fd;

    snprintf(in, sizeof(in), "%s/in_%s.tif", dir, tag);
    write_input(in, epsg, geographic, west, north, res);

    Rast3d_get_window(&region);
    map = Rast3d_open_cell_old(in, G_find_raster3d(in, ""), &region,
                               RASTER3D_TILE_SAME_AS_FILE, RASTER3D_NO_CACHE);
    CHECK(map != NULL, "cannot open <%s>", in);
    if (!map)
        return;
    Rast3d_get_region_struct_map(map, &region);

    snprintf(out, sizeof(out), "cell_%s", tag);
    fd = Rast_open_new(out, CELL_TYPE);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++)
            cbuf[c] = 10 * r + c;
        Rast_put_c_row(fd, cbuf);
    }
    Rast_close(fd);

    snprintf(out, sizeof(out), "fcell_%s", tag);
    fd = Rast_open_new(out, FCELL_TYPE);
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++)
            fbuf[c] = 0.5f * (10 * r + c);
        Rast_put_f_row(fd, fbuf);
    }
    Rast_close(fd);
    Rast3d_close(map);

    snprintf(out, sizeof(out), "%s/cell_%s.tif", dir, tag);
    check_georef(out, epsg, geographic, west, north, res);
    check_cell_values(out);
    snprintf(out, sizeof(out), "%s/fcell_%s.tif", dir, tag);
    check_georef(out, epsg, geographic, west, north, res);
    check_fcell_values(out);

    /* A 3-D output inherits the CRS as well. */
    snprintf(out, sizeof(out), "%s/cube_%s.tif", dir, tag);
    map = Rast3d_open_new_opt_tile_size(out, RASTER3D_USE_CACHE_DEFAULT,
                                        &region, FCELL_TYPE, 32);
    CHECK(map != NULL, "cannot create <%s>", out);
    if (map) {
        for (int z = 0; z < BANDS; z++)
            for (int r = 0; r < ROWS; r++)
                for (int c = 0; c < COLS; c++)
                    Rast3d_put_float(map, c, r, z, (float)(z + r + c));
        Rast3d_close(map);
        check_georef(out, epsg, geographic, west, north, res);
    }
}

int main(void)
{
    char dir[] = "/tmp/ras3d_georef_XXXXXX";

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("RAS3D_OUTDIR", dir, 1);
    setenv("RAS3D_VERBOSE", "0", 1);

    run_case(dir, "utm", 32636, 0, 500000.0, 5260000.0, 60.0);
    run_case(dir, "wgs84", 4326, 1, 35.9, 47.6, 0.0005);

    if (failures == 0)
        printf("PASS: 2-D and 3-D outputs keep values and georeferencing\n");
    return failures ? 1 : 0;
}
