/*
 * backend_geotiff.c — GeoTIFF read/write backend using libtiff + libgeotiff.
 *
 * Handles both:
 *  - Contiguous (interleaved) tiled multi-band TIFFs (e.g. Wyvern):
 *      one IFD, SAMPLESPERPIXEL=nbands, PLANARCONFIG_CONTIG, tiled
 *  - Band-sequential (separate IFDs per band, planar separate)
 *
 * geotiff headers included FIRST to avoid TYPE_DOUBLE macro clash.
 * OpenMP parallelises per-tile reads in read_block.
 */
#include <tiffio.h>
#include <geotiff.h>
#include <xtiffio.h>
#include <geokeys.h>
#include <geovalues.h>

/* ras3d headers after geotiff — TYPE_DOUBLE macro (=2) now safe */
#include "ras3d/ras3d.h"
#include "ras3d/ras3d_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>

/* ── private backend context ─────────────────────────────────────────────── */
typedef struct {
    TIFF  *tif;
    GTIF  *gtif;
    int    nrows, ncols, nbands;
    int    writing;
    char  *path;
    float *out_buf;        /* [bands][rows][cols], only for writing */
    double origin_x, origin_y;
    double pixel_x,  pixel_y;
    int    epsg;
    int    geographic;     /* 1 = lat/lon CRS, 0 = projected */
    int    is_tiled;       /* 1 = use TIFFReadTile, 0 = TIFFReadScanline */
    int    planar_contig;  /* 1 = PLANARCONFIG_CONTIG, 0 = SEPARATE */
    uint32_t tile_w, tile_h;
    /* storage type of file samples */
    uint16_t sf, bps, spp;
} geotiff_ctx_t;

/* ── region fill from TIFF tags ──────────────────────────────────────────── */

static void fill_region(TIFF *tif, RASTER3D_Region *reg, int nbands)
{
    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,  &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    reg->cols   = (int)w;
    reg->rows   = (int)h;
    reg->depths = nbands;

    double scale[3] = {1.0, 1.0, 0.0};
    double tie[6]   = {0.0};
    uint16_t cnt = 0;
    double  *vals = NULL;

    if (TIFFGetField(tif, TIFFTAG_GEOPIXELSCALE, &cnt, &vals) && cnt >= 2) {
        scale[0] = vals[0]; scale[1] = vals[1];
    }
    if (TIFFGetField(tif, TIFFTAG_GEOTIEPOINTS, &cnt, &vals) && cnt >= 6) {
        tie[3] = vals[3]; tie[4] = vals[4];
    }

    reg->ew_res = scale[0];
    reg->ns_res = scale[1];
    reg->west   = tie[3];
    reg->north  = tie[4];
    reg->east   = tie[3] + scale[0] * (double)w;
    reg->south  = tie[4] - scale[1] * (double)h;
    reg->top    = (double)nbands;
    reg->bottom = 0.0;
    reg->tb_res = 1.0;
}

/* ── raw sample → double conversion ─────────────────────────────────────── */

static inline double sample_to_double(const void *ptr, uint16_t sf, uint16_t bps)
{
    if (sf == SAMPLEFORMAT_IEEEFP)
        return (bps == 64) ? *(const double *)ptr : (double)*(const float *)ptr;
    if (sf == SAMPLEFORMAT_INT) {
        if (bps == 32) return (double)*(const int32_t *)ptr;
        if (bps == 16) return (double)*(const int16_t *)ptr;
        return (double)*(const int8_t *)ptr;
    }
    /* UINT */
    if (bps == 32) return (double)*(const uint32_t *)ptr;
    if (bps == 16) return (double)*(const uint16_t *)ptr;
    return (double)*(const uint8_t *)ptr;
}

/* ── read backend ─────────────────────────────────────────────────────────── */

/*
 * Block reads. The requested window [x0, x0+nx) x [y0, y0+ny) x
 * [z0, z0+nz) is written to out[(dz * ny + row) * nx + col]; cells outside
 * the image stay NaN (null). Each tile or scanline is decoded once and all
 * requested bands are scattered from it: with pixel-interleaved (contiguous)
 * data every tile holds all bands, so decoding it once per band would cost
 * nbands times the whole file.
 */
struct window {
    int x0, y0, z0, nx, ny, nz;
};

static inline DCELL *win_cell(DCELL *out, const struct window *w, int dz,
                              int row, int col)
{
    return out + ((size_t)dz * w->ny + (size_t)(row - w->y0)) * w->nx +
           (col - w->x0);
}

static void read_tiled(geotiff_ctx_t *g, const struct window *w, DCELL *out)
{
    int bps_bytes = (int)(g->bps / 8);
    int spp = (int)g->spp;
    uint32_t tw = g->tile_w, th = g->tile_h;
    int xe = w->x0 + w->nx < g->ncols ? w->x0 + w->nx : g->ncols;
    int ye = w->y0 + w->ny < g->nrows ? w->y0 + w->ny : g->nrows;
    int xs = w->x0 > 0 ? w->x0 : 0, ys = w->y0 > 0 ? w->y0 : 0;
    void *tile_buf = malloc((size_t)TIFFTileSize(g->tif));

    if (!tile_buf) {
        G_warning("ras3d: tile buffer allocation failed");
        return;
    }
    for (uint32_t ty = (uint32_t)ys / th * th; ty < (uint32_t)ye; ty += th) {
        for (uint32_t tx = (uint32_t)xs / tw * tw; tx < (uint32_t)xe; tx += tw) {
            int r0 = (int)ty > ys ? (int)ty : ys;
            int r1 = (int)(ty + th) < ye ? (int)(ty + th) : ye;
            int c0 = (int)tx > xs ? (int)tx : xs;
            int c1 = (int)(tx + tw) < xe ? (int)(tx + tw) : xe;

            if (g->planar_contig) {
                TIFFReadTile(g->tif, tile_buf, tx, ty, 0, 0);
                for (int r = r0; r < r1; r++)
                    for (int c = c0; c < c1; c++) {
                        const char *px = (const char *)tile_buf +
                                         ((size_t)(r - ty) * tw + (c - tx)) *
                                             spp * bps_bytes;
                        for (int dz = 0; dz < w->nz; dz++)
                            *win_cell(out, w, dz, r, c) = sample_to_double(
                                px + (size_t)(w->z0 + dz) * bps_bytes, g->sf,
                                g->bps);
                    }
            }
            else {
                for (int dz = 0; dz < w->nz; dz++) {
                    TIFFReadTile(g->tif, tile_buf, tx, ty, 0,
                                 (uint16_t)(w->z0 + dz));
                    for (int r = r0; r < r1; r++)
                        for (int c = c0; c < c1; c++)
                            *win_cell(out, w, dz, r, c) = sample_to_double(
                                (const char *)tile_buf +
                                    ((size_t)(r - ty) * tw + (c - tx)) *
                                        bps_bytes,
                                g->sf, g->bps);
                }
            }
        }
    }
    free(tile_buf);
}

static void read_scanlines(geotiff_ctx_t *g, const struct window *w,
                           DCELL *out)
{
    int bps_bytes = (int)(g->bps / 8);
    int spp = (int)g->spp;
    int xe = w->x0 + w->nx < g->ncols ? w->x0 + w->nx : g->ncols;
    int ye = w->y0 + w->ny < g->nrows ? w->y0 + w->ny : g->nrows;
    int xs = w->x0 > 0 ? w->x0 : 0, ys = w->y0 > 0 ? w->y0 : 0;
    void *row_buf = malloc((size_t)TIFFScanlineSize(g->tif));

    if (!row_buf) {
        G_warning("ras3d: scanline buffer allocation failed");
        return;
    }
    if (g->planar_contig) {
        for (int r = ys; r < ye; r++) {
            TIFFReadScanline(g->tif, row_buf, (uint32_t)r, 0);
            for (int c = xs; c < xe; c++) {
                const char *px =
                    (const char *)row_buf + (size_t)c * spp * bps_bytes;
                for (int dz = 0; dz < w->nz; dz++)
                    *win_cell(out, w, dz, r, c) = sample_to_double(
                        px + (size_t)(w->z0 + dz) * bps_bytes, g->sf, g->bps);
            }
        }
    }
    else {
        /* Separate planes are stored one after another: read plane by
         * plane, rows in order. */
        for (int dz = 0; dz < w->nz; dz++)
            for (int r = ys; r < ye; r++) {
                TIFFReadScanline(g->tif, row_buf, (uint32_t)r,
                                 (uint16_t)(w->z0 + dz));
                for (int c = xs; c < xe; c++)
                    *win_cell(out, w, dz, r, c) = sample_to_double(
                        (const char *)row_buf + (size_t)c * bps_bytes, g->sf,
                        g->bps);
            }
    }
    free(row_buf);
}

static int geotiff_read_block(void *ctx,
                              int x0, int y0, int z0,
                              int nx, int ny, int nz,
                              void *buf, int type)
{
    geotiff_ctx_t *g = ctx;
    struct window w = {x0, y0, z0, nx, ny, nz};
    size_t ntotal = (size_t)nx * ny * nz;
    DCELL *dcell_tmp = G_malloc(ntotal * sizeof(DCELL));

    for (size_t i = 0; i < ntotal; i++)
        dcell_tmp[i] = NAN;
    /* Bands beyond the cube stay null. */
    if (w.z0 + w.nz > g->nbands)
        w.nz = g->nbands - w.z0 > 0 ? g->nbands - w.z0 : 0;
    if (w.nz > 0) {
        if (g->is_tiled)
            read_tiled(g, &w, dcell_tmp);
        else
            read_scanlines(g, &w, dcell_tmp);
    }

    if (type == DCELL_TYPE) {
        memcpy(buf, dcell_tmp, ntotal * sizeof(DCELL));
    } else {
        float *fbuf = buf;
#pragma omp parallel for simd schedule(static)
        for (size_t i = 0; i < ntotal; i++) fbuf[i] = (float)dcell_tmp[i];
    }
    G_free(dcell_tmp);
    return 1;
}

static int geotiff_read_close(void *ctx)
{
    geotiff_ctx_t *g = ctx;
    if (g->gtif) GTIFFree(g->gtif);
    XTIFFClose(g->tif);
    free(g->path);
    free(g);
    return 1;
}

/* ── write backend ────────────────────────────────────────────────────────── */

static int geotiff_write_value(void *ctx, int col, int row, int z, double val)
{
    geotiff_ctx_t *g = ctx;
    g->out_buf[(size_t)z * g->nrows * g->ncols
             + (size_t)row * g->ncols + col] = (float)val;
    return 1;
}

static int geotiff_write_close(void *ctx)
{
    geotiff_ctx_t *g = ctx;
    int bands = g->nbands, rows = g->nrows, cols = g->ncols;

    TIFFSetField(g->tif, TIFFTAG_IMAGEWIDTH,      (uint32_t)cols);
    TIFFSetField(g->tif, TIFFTAG_IMAGELENGTH,     (uint32_t)rows);
    TIFFSetField(g->tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)bands);
    TIFFSetField(g->tif, TIFFTAG_BITSPERSAMPLE,   (uint16_t)32);
    TIFFSetField(g->tif, TIFFTAG_SAMPLEFORMAT,    SAMPLEFORMAT_IEEEFP);
    TIFFSetField(g->tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_SEPARATE);
    TIFFSetField(g->tif, TIFFTAG_COMPRESSION,     COMPRESSION_LZW);
    TIFFSetField(g->tif, TIFFTAG_PREDICTOR,       PREDICTOR_FLOATINGPOINT);
    TIFFSetField(g->tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_MINISBLACK);
    if (bands > 1) {
        /* Greyscale has one colour channel; declare the other bands. */
        uint16_t *extra = G_malloc((size_t)(bands - 1) * sizeof(uint16_t));
        for (int b = 0; b < bands - 1; b++)
            extra[b] = EXTRASAMPLE_UNSPECIFIED;
        TIFFSetField(g->tif, TIFFTAG_EXTRASAMPLES, (uint16_t)(bands - 1), extra);
        G_free(extra);
    }
    uint32_t tile = 512;
    TIFFSetField(g->tif, TIFFTAG_TILEWIDTH,  tile);
    TIFFSetField(g->tif, TIFFTAG_TILELENGTH, tile);

    ras3d_geotiff_set_georef(g->tif, g->gtif, g->origin_x, g->origin_y,
                             g->pixel_x, g->pixel_y, g->epsg, g->geographic);

    /* Write tiles — band-by-band (PLANARCONFIG_SEPARATE) */
    tmsize_t tile_bytes = (tmsize_t)tile * tile * sizeof(float);
    float *tile_buf = G_malloc((size_t)tile_bytes);

    for (int b = 0; b < bands; b++) {
        const float *bdata = g->out_buf + (size_t)b * rows * cols;
        for (uint32_t ty = 0; ty < (uint32_t)rows; ty += tile) {
            for (uint32_t tx = 0; tx < (uint32_t)cols; tx += tile) {
                uint32_t tw = ((uint32_t)cols - tx < tile) ? (uint32_t)cols - tx : tile;
                uint32_t th = ((uint32_t)rows - ty < tile) ? (uint32_t)rows - ty : tile;
                memset(tile_buf, 0, (size_t)tile_bytes);
                for (uint32_t r = 0; r < th; r++)
                    memcpy(tile_buf + r * tile,
                           bdata + ((ty + r) * (uint32_t)cols + tx),
                           (size_t)tw * sizeof(float));
                TIFFWriteTile(g->tif, tile_buf, tx, ty, 0, (uint16_t)b);
            }
        }
    }
    G_free(tile_buf);
    free(g->out_buf);

    if (g->gtif) GTIFFree(g->gtif);
    XTIFFClose(g->tif);
    free(g->path);
    free(g);
    return 1;
}

/* ── georeferencing ───────────────────────────────────────────────────────── */

void ras3d_geotiff_set_georef(void *tif_, void *gtif_, double west,
                              double north, double ew_res, double ns_res,
                              int epsg, int geographic)
{
    TIFF *tif = tif_;
    GTIF *gtif = gtif_;

    if (!(ew_res > 0.0 && ns_res > 0.0))
        return;
    double scale[3] = { ew_res, ns_res, 0.0 };
    double tie[6]   = { 0.0, 0.0, 0.0, west, north, 0.0 };
    TIFFSetField(tif, TIFFTAG_GEOPIXELSCALE, 3, scale);
    TIFFSetField(tif, TIFFTAG_GEOTIEPOINTS,  6, tie);
    if (!gtif)
        return;
    GTIFKeySet(gtif, GTRasterTypeGeoKey, TYPE_SHORT, 1, RasterPixelIsArea);
    if (epsg > 0) {
        GTIFKeySet(gtif, GTModelTypeGeoKey, TYPE_SHORT, 1,
                   geographic ? ModelTypeGeographic : ModelTypeProjected);
        GTIFKeySet(gtif, geographic ? GeographicTypeGeoKey
                                    : ProjectedCSTypeGeoKey,
                   TYPE_SHORT, 1, epsg);
    }
    GTIFWriteKeys(gtif);
}

/* Read the EPSG code and model type of a GeoTIFF; *epsg = 0 when the CRS
 * is missing or user-defined. */
static void read_crs(GTIF *gtif, int *epsg, int *geographic)
{
    unsigned short model = 0, code = 0;

    *epsg = 0;
    *geographic = 0;
    if (!gtif || !GTIFKeyGet(gtif, GTModelTypeGeoKey, &model, 0, 1))
        return;
    if (model == ModelTypeGeographic &&
        GTIFKeyGet(gtif, GeographicTypeGeoKey, &code, 0, 1)) {
        *geographic = 1;
    }
    else if (model != ModelTypeProjected ||
             !GTIFKeyGet(gtif, ProjectedCSTypeGeoKey, &code, 0, 1)) {
        return;
    }
    if (code != KvUserDefined)
        *epsg = code;
}

/* ── public constructors ──────────────────────────────────────────────────── */

int ras3d_open_geotiff_read(const char *path, RASTER3D_Map *map)
{
    TIFF *tif = XTIFFOpen(path, "r");
    if (!tif) { G_warning("ras3d_open_geotiff_read: cannot open <%s>", path); return 0; }
    GTIF *gtif = GTIFNew(tif);

    /* Always use IFD 0 (main/full-resolution image).
     * Skip IFDs flagged as reduced (overview pyramids). */
    TIFFSetDirectory(tif, 0);

    uint16_t spp = 1, sf = SAMPLEFORMAT_IEEEFP, bps = 32;
    uint16_t planar = PLANARCONFIG_CONTIG;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT,    &sf);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE,   &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG,    &planar);

    int is_tiled = TIFFIsTiled(tif);

    /* Bands are samples of the first directory in both layouts:
     * interleaved per pixel (contiguous) or stored as separate planes of
     * the same image (PLANARCONFIG_SEPARATE, e.g. GDAL INTERLEAVE=BAND and
     * ras3d's own 3-D writer). Further directories are overviews. */
    int nbands = spp > 0 ? (int)spp : 1;

    fill_region(tif, &map->region, nbands);
    map->window     = map->region;
    map->type       = FCELL_TYPE;
    map->typeIntern = DCELL_TYPE;
    map->operation  = RASTER3D_READ_DATA;

    geotiff_ctx_t *g = calloc(1, sizeof(*g));
    g->tif    = tif;
    g->gtif   = gtif;
    g->nrows  = map->region.rows;
    g->ncols  = map->region.cols;
    g->nbands = nbands;
    g->writing = 0;
    g->sf      = sf;
    g->bps     = bps;
    g->spp     = spp;
    g->is_tiled     = is_tiled;
    g->planar_contig = (planar == PLANARCONFIG_CONTIG);
    read_crs(gtif, &g->epsg, &g->geographic);
    ras3d_current_epsg = g->epsg;
    ras3d_current_geographic = g->geographic;
    if (is_tiled) {
        TIFFGetField(tif, TIFFTAG_TILEWIDTH,  &g->tile_w);
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &g->tile_h);
    }
    size_t plen = strlen(path) + 1;
    g->path = malloc(plen); memcpy(g->path, path, plen);

    map->backend.read_block  = geotiff_read_block;
    map->backend.write_value = NULL;
    map->backend.close       = geotiff_read_close;
    map->backend.ctx         = g;
    map->out_buf             = NULL;
    return 1;
}

int ras3d_open_geotiff_write(const char *path, RASTER3D_Map *map,
                             int tile_x, int tile_y,
                             int compression, int precision)
{
    (void)tile_x; (void)tile_y; (void)compression; (void)precision;

    TIFF *tif = XTIFFOpen(path, "w");
    if (!tif) { G_warning("ras3d_open_geotiff_write: cannot open <%s>", path); return 0; }
    GTIF *gtif = GTIFNew(tif);

    int rows = map->region.rows, cols = map->region.cols, bands = map->region.depths;

    geotiff_ctx_t *g = calloc(1, sizeof(*g));
    g->tif      = tif;
    g->gtif     = gtif;
    g->nrows    = rows;
    g->ncols    = cols;
    g->nbands   = bands;
    g->writing  = 1;
    g->origin_x = map->region.west;
    g->origin_y = map->region.north;
    g->pixel_x  = map->region.ew_res;
    g->pixel_y  = map->region.ns_res;
    g->epsg       = ras3d_current_epsg;
    g->geographic = ras3d_current_geographic;
    size_t plen = strlen(path) + 1;
    g->path = malloc(plen); memcpy(g->path, path, plen);

    size_t total = (size_t)bands * rows * cols;
    g->out_buf = calloc(total, sizeof(float));
    if (!g->out_buf) G_fatal_error("ras3d: out_buf alloc failed (%zu floats)", total);

    map->out_buf   = g->out_buf;
    map->operation = RASTER3D_WRITE_DATA;

    map->backend.read_block  = NULL;
    map->backend.write_value = geotiff_write_value;
    map->backend.close       = geotiff_write_close;
    map->backend.ctx         = g;
    return 1;
}
