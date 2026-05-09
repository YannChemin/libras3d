/*
 * ras3d_internal.h — private types shared across ras3d source files.
 * NOT for inclusion by library users.
 *
 * Deliberately does not include libgeotiff or libhdf5 headers — those are
 * pulled in by each backend file before including this header to avoid the
 * TYPE_DOUBLE macro/enum clash between libgeotiff and ras3d.h.
 */
#ifndef RAS3D_INTERNAL_H
#define RAS3D_INTERNAL_H

#include <stddef.h>
#include "ras3d.h"

/* ── Backend abstraction ─────────────────────────────────────────────────── */
typedef struct {
    /* Read a block of nz z-slices into buf.  buf layout: [dz][row][col].
     * x0,y0 = origin (0,0 in all current callers); nx,ny,nz = block dims. */
    int (*read_block)(void *ctx, int x0, int y0, int z0, int nx, int ny, int nz,
                      void *buf, int type);
    /* Write one double value at (col, row, z) — output maps only. */
    int (*write_value)(void *ctx, int col, int row, int z, double val);
    /* Flush and release resources. */
    int (*close)(void *ctx);
    void *ctx;
} ras3d_backend_t;

/* ── 3-D map handle ──────────────────────────────────────────────────────── */
struct RASTER3D_Map {
    int    version;
    char  *fileName;
    char  *mapset;
    int    operation;   /* RASTER3D_READ_DATA or RASTER3D_WRITE_DATA */

    RASTER3D_Region region;
    RASTER3D_Region window;

    int tileX, tileY, tileZ;
    int nx, ny, nz;
    int type;        /* DCELL_TYPE or FCELL_TYPE (file storage) */
    int typeIntern;
    int precision;
    int compression;

    ras3d_backend_t backend;

    /* Convenience pointer into the geotiff write context's buffer, or NULL. */
    float *out_buf;
};

/* ── 2-D raster handle table (defined in grass_compat.c) ────────────────── */
#define RAS3D_MAX_2D 64

/* Forward-declared opaque — fully defined in grass_compat.c after tiffio.h */
typedef struct rast2d_handle rast2d_handle_t;

extern rast2d_handle_t *ras3d_rast2d;   /* points to static array */

/* ── Global parser state ─────────────────────────────────────────────────── */
#define RAS3D_MAX_OPTIONS 128
#define RAS3D_MAX_FLAGS    64

extern struct Option *ras3d_options[RAS3D_MAX_OPTIONS];
extern int            ras3d_noptions;
extern struct Flag   *ras3d_flags[RAS3D_MAX_FLAGS];
extern int            ras3d_nflags;

/* ── Global current window ───────────────────────────────────────────────── */
extern RASTER3D_Region ras3d_current_window;
extern int             ras3d_window_valid;

/* ── Internal helpers ────────────────────────────────────────────────────── */
RASTER3D_Map *ras3d_map_alloc(void);
void          ras3d_map_free(RASTER3D_Map *map);

/* Backend constructors */
int  ras3d_open_geotiff_read(const char *path, RASTER3D_Map *map);
int  ras3d_open_geotiff_write(const char *path, RASTER3D_Map *map,
                              int tile_x, int tile_y,
                              int compression, int precision);
int  ras3d_open_hdf5_read(const char *path, RASTER3D_Map *map);

#endif /* RAS3D_INTERNAL_H */
