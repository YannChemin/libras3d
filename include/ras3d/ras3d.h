/*
 * ras3d.h — drop-in replacement for <grass/gis.h>, <grass/glocale.h>,
 *            <grass/raster.h>, and <grass/raster3d.h>.
 *
 * Include this single header instead of those four GRASS headers when
 * building i.hyper.* modules outside GRASS GIS.  All public symbols used
 * by i.hyper.atcorr are reproduced here with identical struct layouts.
 */
#ifndef RAS3D_H
#define RAS3D_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── glocale.h emulation ─────────────────────────────────────────────────── */
#ifndef _
#define _(x) (x)
#endif

/* ── M_PI (not in C11 without GNU extensions) ────────────────────────────── */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── raster.h type emulation ─────────────────────────────────────────────── */
typedef int    CELL;
typedef float  FCELL;
typedef double DCELL;

#define CELL_TYPE   0
#define FCELL_TYPE  1
#define DCELL_TYPE  2

/* ── gis.h constants ─────────────────────────────────────────────────────── */
#define YES  1
#define NO   0
#define REQUIRED YES
#define OPTIONAL NO

#define GPATH_MAX 4096

#define TYPE_INTEGER  1
#define TYPE_DOUBLE   2
#define TYPE_STRING   3

/* G_OPT_ enum — only the values used by i.hyper.* are listed */
typedef enum {
    G_OPT_R_INPUT = 0,
    G_OPT_R_INPUTS,
    G_OPT_R_OUTPUT,
    G_OPT_R_OUTPUTS,
    G_OPT_R_MAP,
    G_OPT_R_MAPS,
    G_OPT_R_BASE,
    G_OPT_R_COVER,
    G_OPT_R_ELEV,
    G_OPT_R_ELEVS,
    G_OPT_R_TYPE,
    G_OPT_R_INTERP_TYPE,
    G_OPT_R_BASENAME_INPUT,
    G_OPT_R_BASENAME_OUTPUT,
    G_OPT_R3_INPUT,
    G_OPT_R3_INPUTS,
    G_OPT_R3_OUTPUT,
    G_OPT_R3_MAP,
    G_OPT_R3_MAPS,
    G_OPT_R3_TYPE,
    G_OPT_R3_PRECISION,
    G_OPT_R3_TILE_DIMENSION,
    G_OPT_R3_COMPRESSION,
} StandardOption;

/* ── Cell_head — GRASS 2-D region descriptor (used by G_get_window) ─────── */
struct Cell_head {
    int    format;
    int    compressed;
    int    rows;
    int    rows3;
    int    cols;
    int    cols3;
    int    depths;
    int    proj;
    int    zone;
    double ew_res;
    double ew_res3;
    double ns_res;
    double ns_res3;
    double tb_res;
    double north;
    double south;
    double east;
    double west;
    double top;
    double bottom;
};

/* ── gis.h structs ───────────────────────────────────────────────────────── */
struct Option {
    const char *key;
    int         type;
    int         required;
    int         multiple;
    const char *options;
    const char **opts;
    const char *key_desc;
    const char *label;
    const char *description;
    const char *descriptions;
    const char **descs;
    char       *answer;
    const char *def;
    char      **answers;
    struct Option *next_opt;
    const char *gisprompt;
    const char *guisection;
    const char *guidependency;
    int (*checker)(const char *);
    int count;
};

struct Flag {
    char key;
    char answer;
    char suppress_required;
    char suppress_overwrite;
    const char *label;
    const char *description;
    const char *guisection;
    struct Flag *next_flag;
};

struct GModule {
    const char  *label;
    const char  *description;
    const char **keywords;
    int          overwrite;
    int          verbose;
};

/* ── raster3d.h constants ────────────────────────────────────────────────── */
#define RASTER3D_MAP_VERSION        2
#define RASTER3D_TILE_SAME_AS_FILE  2
#define RASTER3D_NO_COMPRESSION     0
#define RASTER3D_COMPRESSION        1
#define RASTER3D_MAX_PRECISION     -1
#define RASTER3D_NO_CACHE           0
#define RASTER3D_USE_CACHE_DEFAULT -1
#define RASTER3D_USE_CACHE_X       -2
#define RASTER3D_USE_CACHE_Y       -3
#define RASTER3D_USE_CACHE_Z       -4
#define RASTER3D_USE_CACHE_XY      -5
#define RASTER3D_USE_CACHE_XZ      -6
#define RASTER3D_USE_CACHE_YZ      -7
#define RASTER3D_USE_CACHE_XYZ     -8
#define RASTER3D_DEFAULT_WINDOW     0
#define RASTER3D_WRITE_DATA         1
#define RASTER3D_READ_DATA          0

/* ── raster3d.h types ────────────────────────────────────────────────────── */
typedef struct {
    double north, south;
    double east,  west;
    double top,   bottom;
    int    rows, cols, depths;
    double ns_res, ew_res, tb_res;
    int    proj;
    int    zone;
} RASTER3D_Region;

/* Opaque map handle — internal definition in ras3d_internal.h */
typedef struct RASTER3D_Map RASTER3D_Map;

/* ── gis.h function declarations ─────────────────────────────────────────── */
void        *G_malloc(size_t size);
void        *G_calloc(size_t nmemb, size_t size);
void        *G_realloc(void *ptr, size_t size);
void         G_free(void *ptr);
char        *G_store(const char *s);

void         G_fatal_error(const char *fmt, ...) __attribute__((noreturn, format(printf,1,2)));
void         G_warning(const char *fmt, ...) __attribute__((format(printf,1,2)));
void         G_message(const char *fmt, ...) __attribute__((format(printf,1,2)));
void         G_verbose_message(const char *fmt, ...) __attribute__((format(printf,1,2)));
int          G_verbose(void);
int          G_verbose_std(void);
void         G_percent(long done, long total, int d);
void         G_debug(int level, const char *fmt, ...) __attribute__((format(printf,2,3)));

const char  *G_gisdbase(void);
const char  *G_location(void);
const char  *G_mapset(void);

/* G_gisinit — GRASS init macro; in ras3d it is a no-op */
void         G__gisinit(const char *version, const char *pgm);
#define      G_gisinit(pgm) G__gisinit("", (pgm))

void         G_get_window(struct Cell_head *win);
void         G_add_keyword(const char *kw);

/* Find 2-D raster: returns directory (mapset) string or NULL */
const char  *G_find_raster(const char *name, const char *mapset);
/* Find 3-D raster: returns directory string or NULL */
const char  *G_find_raster3d(const char *name, const char *mapset);

/* Parser */
struct GModule *G_define_module(void);
struct Option  *G_define_option(void);
struct Option  *G_define_standard_option(StandardOption opt);
struct Flag    *G_define_flag(void);
int             G_parser(int argc, char **argv);

/* ── raster.h function declarations ──────────────────────────────────────── */
int     Rast_open_old(const char *name, const char *mapset);
int     Rast_open_new(const char *name, int type);
void    Rast_close(int fd);

DCELL  *Rast_allocate_d_buf(void);
FCELL  *Rast_allocate_f_buf(void);
CELL   *Rast_allocate_c_buf(void);

void    Rast_get_d_row(int fd, DCELL *buf, int row);
void    Rast_put_f_row(int fd, FCELL *buf);
void    Rast_put_c_row(int fd, CELL  *buf);

int     Rast_is_d_null_value(const DCELL *v);
void    Rast_set_f_null_value(FCELL *buf, int n);
void    Rast_set_d_null_value(DCELL *buf, int n);
void    Rast_set_c_null_value(CELL  *buf, int n);

/* ── raster3d.h function declarations ────────────────────────────────────── */
void          Rast3d_get_window(RASTER3D_Region *region);
void          Rast3d_set_window(RASTER3D_Region *region);
void          Rast3d_init_defaults(void);

RASTER3D_Map *Rast3d_open_cell_old(const char *name, const char *mapset,
                                    RASTER3D_Region *window,
                                    int tile_type, int cache_mode);

/* Matches the actual GRASS signature: (name, cache, region, type, maxSize_KB) */
RASTER3D_Map *Rast3d_open_new_opt_tile_size(const char *name, int cache,
                                             RASTER3D_Region *region,
                                             int type, int maxSize);

void  Rast3d_get_region_struct_map(RASTER3D_Map *map, RASTER3D_Region *region);

void  Rast3d_get_block(RASTER3D_Map *map,
                       int x0, int y0, int z0,
                       int nx, int ny, int nz,
                       void *buf, int type);

void  Rast3d_put_double(RASTER3D_Map *map, int col, int row, int z, double val);
void  Rast3d_put_float(RASTER3D_Map *map,  int col, int row, int z, float  val);

/* Cache advisory hint — no-op in ras3d (no tile cache) */
void  Rast3d_min_unlocked(RASTER3D_Map *map, int mode);

int   Rast3d_close(RASTER3D_Map *map);

/* ── History / metadata helpers used by atcorr wavelength reader ─────────── */
/* Minimal stubs — not a full GRASS history implementation */
typedef struct { char *line; int nlines; } RASTER3D_History;
int  Rast3d_read_history(RASTER3D_Map *map, RASTER3D_History *hist);
int  Rast3d_write_history(RASTER3D_Map *map, RASTER3D_History *hist);
void Rast3d_init_history(RASTER3D_History *hist);
void Rast3d_free_history(RASTER3D_History *hist);

/* Wavelength metadata — stored as a sidecar JSON file by ras3d */
int  ras3d_read_wavelengths(const char *mapname, float **wl_out, int *n_out);
int  ras3d_write_wavelengths(const char *mapname, const float *wl, int n);

#ifdef __cplusplus
}
#endif
#endif /* RAS3D_H */
