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
 * Read one z-slice (band z0) from a tiled contiguous TIFF.
 * Layout: each tile stores spp values per pixel, interleaved [b0,b1,...,bN].
 */
static void read_band_tiled_contig(geotiff_ctx_t *g, int band, DCELL *out)
{
    int cols = g->ncols, rows = g->nrows;
    int spp  = (int)g->spp;
    int bps_bytes = (int)(g->bps / 8);
    uint32_t tw = g->tile_w, th = g->tile_h;
    size_t tile_bytes = (size_t)TIFFTileSize(g->tif);

    /* Decompose into tiles and process each tile serially (libtiff is not
     * thread-safe for concurrent reads on the same TIFF handle).
     * OpenMP is applied at the caller (band) level. */
    void *tile_buf = malloc(tile_bytes);
    if (!tile_buf) { G_warning("read_band_tiled_contig: malloc failed"); return; }

    for (uint32_t ty = 0; ty < (uint32_t)rows; ty += th) {
        for (uint32_t tx = 0; tx < (uint32_t)cols; tx += tw) {
            TIFFReadTile(g->tif, tile_buf, tx, ty, 0, 0);

            uint32_t actual_h = ((uint32_t)rows - ty < th) ? ((uint32_t)rows - ty) : th;
            uint32_t actual_w = ((uint32_t)cols - tx < tw) ? ((uint32_t)cols - tx) : tw;

            for (uint32_t r = 0; r < actual_h; r++) {
                for (uint32_t c = 0; c < actual_w; c++) {
                    /* In contig tile: pixel layout is [spp samples per pixel] */
                    const void *ptr = (const char *)tile_buf
                                    + ((size_t)r * tw + c) * spp * bps_bytes
                                    + band * bps_bytes;
                    out[(ty + r) * (uint32_t)cols + (tx + c)] =
                        sample_to_double(ptr, g->sf, g->bps);
                }
            }
        }
    }
    free(tile_buf);
}

/*
 * Read one band from a tiled SEPARATE TIFF (each band in its own plane).
 */
static void read_band_tiled_separate(geotiff_ctx_t *g, int band, DCELL *out)
{
    int cols = g->ncols, rows = g->nrows;
    int bps_bytes = (int)(g->bps / 8);
    uint32_t tw = g->tile_w, th = g->tile_h;
    size_t tile_bytes = (size_t)TIFFTileSize(g->tif);

    void *tile_buf = malloc(tile_bytes);
    if (!tile_buf) { G_warning("read_band_tiled_separate: malloc failed"); return; }

    for (uint32_t ty = 0; ty < (uint32_t)rows; ty += th) {
        for (uint32_t tx = 0; tx < (uint32_t)cols; tx += tw) {
            TIFFReadTile(g->tif, tile_buf, tx, ty, 0, (uint16_t)band);

            uint32_t actual_h = ((uint32_t)rows - ty < th) ? ((uint32_t)rows - ty) : th;
            uint32_t actual_w = ((uint32_t)cols - tx < tw) ? ((uint32_t)cols - tx) : tw;

            for (uint32_t r = 0; r < actual_h; r++) {
                for (uint32_t c = 0; c < actual_w; c++) {
                    const void *ptr = (const char *)tile_buf
                                    + ((size_t)r * tw + c) * bps_bytes;
                    out[(ty + r) * (uint32_t)cols + (tx + c)] =
                        sample_to_double(ptr, g->sf, g->bps);
                }
            }
        }
    }
    free(tile_buf);
}

/*
 * Read one band from a scanline TIFF (strip or plain scanline).
 * Works for both CONTIG and SEPARATE planar configs.
 */
static void read_band_scanline(geotiff_ctx_t *g, int band, DCELL *out)
{
    int cols = g->ncols, rows = g->nrows;
    int spp  = (int)g->spp;
    int bps_bytes = (int)(g->bps / 8);
    tmsize_t row_bytes = TIFFScanlineSize(g->tif);
    void *row_buf = malloc((size_t)row_bytes);
    if (!row_buf) { G_warning("read_band_scanline: malloc failed"); return; }

    for (int r = 0; r < rows; r++) {
        uint16_t sample = g->planar_contig ? 0 : (uint16_t)band;
        TIFFReadScanline(g->tif, row_buf, (uint32_t)r, sample);
        for (int c = 0; c < cols; c++) {
            const void *ptr;
            if (g->planar_contig)
                ptr = (const char *)row_buf + ((size_t)c * spp + band) * bps_bytes;
            else
                ptr = (const char *)row_buf + (size_t)c * bps_bytes;
            out[(size_t)r * cols + c] = sample_to_double(ptr, g->sf, g->bps);
        }
    }
    free(row_buf);
}

static int geotiff_read_block(void *ctx,
                              int x0, int y0, int z0,
                              int nx, int ny, int nz,
                              void *buf, int type)
{
    (void)x0; (void)y0;
    geotiff_ctx_t *g = ctx;
    int npix = nx * ny;
    DCELL *dcell_tmp = G_malloc((size_t)npix * nz * sizeof(DCELL));

    for (int dz = 0; dz < nz; dz++) {
        int band = z0 + dz;
        DCELL *dst = dcell_tmp + (size_t)dz * npix;

        if (g->is_tiled) {
            if (g->planar_contig)
                read_band_tiled_contig(g, band, dst);
            else
                read_band_tiled_separate(g, band, dst);
        } else {
            read_band_scanline(g, band, dst);
        }
    }

    if (type == DCELL_TYPE) {
        memcpy(buf, dcell_tmp, (size_t)npix * nz * sizeof(DCELL));
    } else {
        float *fbuf = buf;
        int ntotal = npix * nz;
#pragma omp parallel for simd schedule(static)
        for (int i = 0; i < ntotal; i++) fbuf[i] = (float)dcell_tmp[i];
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
    uint32_t tile = 512;
    TIFFSetField(g->tif, TIFFTAG_TILEWIDTH,  tile);
    TIFFSetField(g->tif, TIFFTAG_TILELENGTH, tile);

    /* Geo-tags */
    if (g->pixel_x > 0.0 || g->pixel_y > 0.0) {
        double scale[3] = { g->pixel_x, g->pixel_y, 0.0 };
        double tie[6]   = { 0.0, 0.0, 0.0, g->origin_x, g->origin_y, 0.0 };
        TIFFSetField(g->tif, TIFFTAG_GEOPIXELSCALE, 3, scale);
        TIFFSetField(g->tif, TIFFTAG_GEOTIEPOINTS,  6, tie);
        if (g->gtif && g->epsg) {
            GTIFKeySet(g->gtif, GTModelTypeGeoKey,     TYPE_SHORT, 1, (short)1);
            GTIFKeySet(g->gtif, ProjectedCSTypeGeoKey, TYPE_SHORT, 1, (short)g->epsg);
            GTIFWriteKeys(g->gtif);
        }
    }

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

    int nbands;
    if (planar == PLANARCONFIG_CONTIG) {
        /* All bands in one IFD, interleaved: nbands = spp */
        nbands = (int)spp;
    } else {
        /* Separate planes: count non-overview IFDs */
        nbands = 0;
        tdir_t d = 0;
        do {
            uint32_t subtype = 0;
            TIFFGetFieldDefaulted(tif, TIFFTAG_SUBFILETYPE, &subtype);
            if (!(subtype & 0x1)) /* not a reduced-resolution image */
                nbands++;
            d++;
        } while (TIFFReadDirectory(tif));
        if (nbands == 0) nbands = 1;
        TIFFSetDirectory(tif, 0);
    }

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
