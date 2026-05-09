/*
 * api.c — public Rast3d_* API entry points.
 *
 * Format detection by file extension:
 *   .tif / .tiff        → GeoTIFF backend
 *   .h5 / .hdf5 / .he5  → HDF5 backend
 *
 * Name resolution: try literal path first, then add common extensions,
 * then search $RAS3D_PATH directory.  Matches G_find_raster3d behaviour.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "ras3d/ras3d.h"
#include "ras3d/ras3d_internal.h"

/* ── map allocation ───────────────────────────────────────────────────────── */

RASTER3D_Map *ras3d_map_alloc(void)
{
    RASTER3D_Map *m = calloc(1, sizeof(*m));
    if (!m) G_fatal_error("ras3d_map_alloc: out of memory");
    m->version = RASTER3D_MAP_VERSION;
    return m;
}

void ras3d_map_free(RASTER3D_Map *map)
{
    if (!map) return;
    free(map->fileName);
    free(map->mapset);
    free(map);
}

/* ── format detection ─────────────────────────────────────────────────────── */

typedef enum { FMT_UNKNOWN, FMT_GEOTIFF, FMT_HDF5 } file_fmt_t;

static file_fmt_t detect_format(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return FMT_UNKNOWN;
    /* lower-case compare */
    char lo[16]; size_t n = strlen(ext);
    if (n >= sizeof(lo)) n = sizeof(lo)-1;
    for (size_t i = 0; i < n; i++)
        lo[i] = (char)((ext[i] >= 'A' && ext[i] <= 'Z') ? ext[i]+32 : ext[i]);
    lo[n] = '\0';
    if (strcmp(lo, ".tif")  == 0 || strcmp(lo, ".tiff") == 0) return FMT_GEOTIFF;
    if (strcmp(lo, ".h5")   == 0 || strcmp(lo, ".hdf5") == 0 ||
        strcmp(lo, ".he5")  == 0 || strcmp(lo, ".hdf")  == 0) return FMT_HDF5;
    return FMT_UNKNOWN;
}

/* Resolve name to an actual file path.  Writes into out (size outsz).
 * Returns 1 on success, 0 if not found. */
static int resolve_path(const char *name, char *out, size_t outsz)
{
    FILE *f = fopen(name, "rb");
    if (f) { fclose(f); snprintf(out, outsz, "%s", name); return 1; }

    const char *exts[] = { ".tif", ".tiff", ".h5", ".hdf5", ".he5", NULL };
    for (int i = 0; exts[i]; i++) {
        snprintf(out, outsz, "%s%s", name, exts[i]);
        f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
    }
    const char *search = getenv("RAS3D_PATH");
    if (search) {
        snprintf(out, outsz, "%s/%s", search, name);
        f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
        for (int i = 0; exts[i]; i++) {
            snprintf(out, outsz, "%s/%s%s", search, name, exts[i]);
            f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
        }
    }
    return 0;
}

/* ── Rast3d_open_cell_old ─────────────────────────────────────────────────── */

RASTER3D_Map *Rast3d_open_cell_old(const char *name, const char *mapset,
                                    RASTER3D_Region *window,
                                    int tile_type, int cache_mode)
{
    (void)mapset; (void)tile_type; (void)cache_mode;

    char path[4096];
    if (!resolve_path(name, path, sizeof(path))) {
        G_warning("Rast3d_open_cell_old: cannot find <%s>", name);
        return NULL;
    }

    RASTER3D_Map *map = ras3d_map_alloc();
    map->fileName = strdup(path);
    map->mapset   = strdup(mapset ? mapset : ".");

    /* Seed region from window so backends can narrow if needed */
    if (window) map->region = *window;

    file_fmt_t fmt = detect_format(path);
    int ok = 0;
    switch (fmt) {
    case FMT_GEOTIFF: ok = ras3d_open_geotiff_read(path, map); break;
    case FMT_HDF5:    ok = ras3d_open_hdf5_read(path, map);    break;
    default:
        G_warning("Rast3d_open_cell_old: unknown format for <%s>", path);
        break;
    }

    if (!ok) { ras3d_map_free(map); return NULL; }

    /* Publish window */
    ras3d_current_window = map->region;
    ras3d_window_valid   = 1;
    return map;
}

/* ── Rast3d_init_defaults / Rast3d_min_unlocked ──────────────────────────── */

void Rast3d_init_defaults(void) { /* no-op: ras3d has no global defaults */ }
void Rast3d_min_unlocked(RASTER3D_Map *map, int mode) { (void)map; (void)mode; }

/* ── Rast3d_open_new_opt_tile_size ───────────────────────────────────────── */
/* Matches the actual GRASS signature: cache advisory ignored; maxSize_KB
 * ignored (output always uses 512×512 tiles). */

RASTER3D_Map *Rast3d_open_new_opt_tile_size(const char *name, int cache,
                                             RASTER3D_Region *region,
                                             int type, int maxSize)
{
    (void)cache; (void)maxSize;

    /* Build output path with .tif extension */
    char path[4096];
    size_t nlen = strlen(name);
    int has_ext = (nlen > 4 && (strcmp(name+nlen-4, ".tif")  == 0 ||
                                strcmp(name+nlen-5, ".tiff") == 0));
    if (has_ext)
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "%s.tif", name);

    /* Honour $RAS3D_OUTDIR */
    const char *outdir = getenv("RAS3D_OUTDIR");
    if (outdir) {
        const char *base = strrchr(path, '/');
        base = base ? base+1 : path;
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s/%s", outdir, base);
        snprintf(path, sizeof(path), "%s", tmp);
    }

    RASTER3D_Map *map = ras3d_map_alloc();
    map->fileName = strdup(path);
    map->mapset   = strdup(".");
    if (region) map->region = *region;
    map->type     = type;
    map->typeIntern = type;
    map->tileX = 512;
    map->tileY = 512;
    map->tileZ = 1;

    int ok = ras3d_open_geotiff_write(path, map, 512, 512,
                                      RASTER3D_COMPRESSION, RASTER3D_MAX_PRECISION);
    if (!ok) { ras3d_map_free(map); return NULL; }
    return map;
}

/* ── Rast3d_get_block ─────────────────────────────────────────────────────── */

void Rast3d_get_block(RASTER3D_Map *map,
                      int x0, int y0, int z0,
                      int nx, int ny, int nz,
                      void *buf, int type)
{
    if (!map || !map->backend.read_block)
        G_fatal_error("Rast3d_get_block: map not open for reading");
    map->backend.read_block(map->backend.ctx, x0, y0, z0, nx, ny, nz, buf, type);
}

/* ── Rast3d_put_double / Rast3d_put_float ────────────────────────────────── */

void Rast3d_put_double(RASTER3D_Map *map, int col, int row, int z, double val)
{
    if (!map || !map->backend.write_value)
        G_fatal_error("Rast3d_put_double: map not open for writing");
    map->backend.write_value(map->backend.ctx, col, row, z, val);
}

void Rast3d_put_float(RASTER3D_Map *map, int col, int row, int z, float val)
{
    Rast3d_put_double(map, col, row, z, (double)val);
}

/* ── Rast3d_close ─────────────────────────────────────────────────────────── */

int Rast3d_close(RASTER3D_Map *map)
{
    if (!map) return 0;
    int ok = 1;
    if (map->backend.close)
        ok = map->backend.close(map->backend.ctx);
    ras3d_map_free(map);
    return ok;
}

/* ── Wavelength metadata (sidecar .wl.json) ──────────────────────────────── */

int ras3d_read_wavelengths(const char *mapname, float **wl_out, int *n_out)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s.wl.json", mapname);
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Try stripping known extensions */
        char base[4096];
        snprintf(base, sizeof(base), "%s", mapname);
        char *dot = strrchr(base, '.');
        if (dot) {
            *dot = '\0';
            snprintf(path, sizeof(path), "%s.wl.json", base);
            f = fopen(path, "r");
        }
    }
    if (!f) return 0;

    /* Parse simple JSON array: [wl0, wl1, ...] */
    int capacity = 512, n = 0;
    float *wl = malloc((size_t)capacity * sizeof(float));
    char buf[65536];
    size_t sz = fread(buf, 1, sizeof(buf)-1, f);
    fclose(f);
    buf[sz] = '\0';

    char *p = strchr(buf, '[');
    if (!p) { free(wl); return 0; }
    p++;
    while (*p && *p != ']') {
        char *end;
        float v = strtof(p, &end);
        if (end == p) { p++; continue; }
        if (n == capacity) {
            capacity *= 2;
            wl = realloc(wl, (size_t)capacity * sizeof(float));
        }
        wl[n++] = v;
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r') p++;
    }
    *wl_out = wl;
    *n_out  = n;
    return 1;
}

int ras3d_write_wavelengths(const char *mapname, const float *wl, int n)
{
    char path[4096];
    /* Strip known extension and add .wl.json */
    char base[4096];
    snprintf(base, sizeof(base), "%s", mapname);
    char *dot = strrchr(base, '.');
    if (dot && (strcmp(dot, ".tif") == 0 || strcmp(dot, ".tiff") == 0))
        *dot = '\0';
    snprintf(path, sizeof(path), "%s.wl.json", base);

    FILE *f = fopen(path, "w");
    if (!f) { G_warning("ras3d_write_wavelengths: cannot write %s", path); return 0; }
    fprintf(f, "[");
    for (int i = 0; i < n; i++) fprintf(f, "%s%g", i ? "," : "", (double)wl[i]);
    fprintf(f, "]\n");
    fclose(f);
    return 1;
}

/* ── Stub history functions ───────────────────────────────────────────────── */

int  Rast3d_read_history(RASTER3D_Map *map, RASTER3D_History *hist)
{
    (void)map; (void)hist; return 0;
}

int  Rast3d_write_history(RASTER3D_Map *map, RASTER3D_History *hist)
{
    (void)map; (void)hist; return 0;
}

void Rast3d_init_history(RASTER3D_History *hist)
{
    if (hist) { hist->line = NULL; hist->nlines = 0; }
}

void Rast3d_free_history(RASTER3D_History *hist)
{
    if (hist) { free(hist->line); hist->nlines = 0; }
}
