/*
 * grass_compat.c — emulation of GRASS GIS API symbols.
 *
 * geotiff headers MUST be included before ras3d.h to avoid the
 * TYPE_DOUBLE macro/enum clash (libgeotiff declares TYPE_DOUBLE in an enum;
 * ras3d.h then #defines it to the GRASS integer value 2).
 */
#include <tiffio.h>
#include <geotiff.h>
#include <xtiffio.h>

/* Now safe to pull in ras3d types (TYPE_DOUBLE macro won't re-parse geotiff) */
#include "ras3d/ras3d.h"
#include "ras3d/ras3d_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

/* ── 2-D raster handle ────────────────────────────────────────────────────── */
struct rast2d_handle {
    int    in_use;
    TIFF  *tif;
    GTIF  *gtif;
    int    nrows, ncols;
    int    type;      /* CELL_TYPE, FCELL_TYPE, DCELL_TYPE */
    int    writing;
    int    cur_row;
    char  *path;
};

static struct rast2d_handle _rast2d_table[RAS3D_MAX_2D];
rast2d_handle_t *ras3d_rast2d = (rast2d_handle_t *)_rast2d_table;

/* ── Global parser state ──────────────────────────────────────────────────── */
struct Option *ras3d_options[RAS3D_MAX_OPTIONS];
int            ras3d_noptions = 0;
struct Flag   *ras3d_flags[RAS3D_MAX_FLAGS];
int            ras3d_nflags = 0;

/* ── Global window state ──────────────────────────────────────────────────── */
RASTER3D_Region ras3d_current_window;
int             ras3d_window_valid = 0;

/* ── memory ───────────────────────────────────────────────────────────────── */
void *G_malloc(size_t size)
{
    void *p = malloc(size);
    if (!p) { fprintf(stderr, "ras3d: G_malloc(%zu) failed\n", size); exit(1); }
    return p;
}

void *G_calloc(size_t nmemb, size_t size)
{
    void *p = calloc(nmemb, size);
    if (!p) { fprintf(stderr, "ras3d: G_calloc failed\n"); exit(1); }
    return p;
}

void *G_realloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (!p) { fprintf(stderr, "ras3d: G_realloc(%zu) failed\n", size); exit(1); }
    return p;
}

void G_free(void *ptr) { free(ptr); }

char *G_store(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (!d) { fprintf(stderr, "ras3d: G_store alloc failed\n"); exit(1); }
    memcpy(d, s, len);
    return d;
}

/* ── logging ──────────────────────────────────────────────────────────────── */
void G_fatal_error(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL: ");
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

void G_warning(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "WARNING: ");
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

void G_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

void G_verbose_message(const char *fmt, ...)
{
    if (G_verbose() < 2) return;
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

int G_verbose(void)
{
    const char *v = getenv("RAS3D_VERBOSE");
    return v ? atoi(v) : 1;
}

int G_verbose_std(void) { return 1; }

void G_percent(long done, long total, int d)
{
    (void)d;
    if (G_verbose() < 1 || total <= 0) return;
    fprintf(stderr, "\r%3ld%%", (done * 100L) / total);
    if (done >= total) fprintf(stderr, "\n");
    fflush(stderr);
}

void G_debug(int level, const char *fmt, ...)
{
    if (G_verbose() < level + 2) return;
    va_list ap;
    fprintf(stderr, "DEBUG: ");
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

void G__gisinit(const char *version, const char *pgm)
{
    (void)version; (void)pgm; /* no GRASS environment to init */
}

void G_add_keyword(const char *kw) { (void)kw; }

void G_get_window(struct Cell_head *win)
{
    memset(win, 0, sizeof(*win));
    if (ras3d_window_valid) {
        win->rows   = ras3d_current_window.rows;
        win->cols   = ras3d_current_window.cols;
        win->depths = ras3d_current_window.depths;
        win->north  = ras3d_current_window.north;
        win->south  = ras3d_current_window.south;
        win->east   = ras3d_current_window.east;
        win->west   = ras3d_current_window.west;
        win->ns_res = ras3d_current_window.ns_res;
        win->ew_res = ras3d_current_window.ew_res;
        win->top    = ras3d_current_window.top;
        win->bottom = ras3d_current_window.bottom;
        win->tb_res = ras3d_current_window.tb_res;
    }
}

/* ── environment/location emulation ──────────────────────────────────────── */
const char *G_gisdbase(void)
{
    const char *v = getenv("GISDBASE");
    return v ? v : ".";
}

const char *G_location(void)
{
    const char *v = getenv("LOCATION_NAME");
    return v ? v : "ras3d";
}

const char *G_mapset(void)
{
    const char *v = getenv("MAPSET");
    return v ? v : "PERMANENT";
}

/* ── file-finder helpers ──────────────────────────────────────────────────── */
static const char *probe_file(const char *name,
                              const char *const *exts, int n_ext,
                              char *out, size_t outsz)
{
    FILE *f = fopen(name, "rb");
    if (f) { fclose(f); if (out) { snprintf(out, outsz, "%s", name); } return "."; }

    for (int i = 0; i < n_ext; i++) {
        if (out) snprintf(out, outsz, "%s%s", name, exts[i]);
        char probe[4096];
        snprintf(probe, sizeof(probe), "%s%s", name, exts[i]);
        f = fopen(probe, "rb");
        if (f) { fclose(f); if (out) snprintf(out, outsz, "%s", probe); return "."; }
    }
    const char *search = getenv("RAS3D_PATH");
    if (search) {
        char probe[4096];
        snprintf(probe, sizeof(probe), "%s/%s", search, name);
        f = fopen(probe, "rb");
        if (f) { fclose(f); if (out) snprintf(out, outsz, "%s", probe); return search; }
        for (int i = 0; i < n_ext; i++) {
            snprintf(probe, sizeof(probe), "%s/%s%s", search, name, exts[i]);
            f = fopen(probe, "rb");
            if (f) { fclose(f); if (out) snprintf(out, outsz, "%s", probe); return search; }
        }
    }
    return NULL;
}

static const char *raster2d_exts[] = { ".tif", ".tiff", ".TIF", ".TIFF" };
static const char *raster3d_exts[] = { ".tif", ".tiff", ".h5", ".hdf5",
                                        ".he5", ".TIF",  ".TIFF" };

const char *G_find_raster(const char *name, const char *mapset)
{
    (void)mapset;
    return probe_file(name, raster2d_exts,
                      (int)(sizeof(raster2d_exts)/sizeof(*raster2d_exts)),
                      NULL, 0);
}

const char *G_find_raster3d(const char *name, const char *mapset)
{
    (void)mapset;
    return probe_file(name, raster3d_exts,
                      (int)(sizeof(raster3d_exts)/sizeof(*raster3d_exts)),
                      NULL, 0);
}

/* ── G_parser family ──────────────────────────────────────────────────────── */
struct GModule *G_define_module(void)
{
    struct GModule *m = calloc(1, sizeof(*m));
    if (!m) { fprintf(stderr, "ras3d: G_define_module alloc failed\n"); exit(1); }
    return m;
}

struct Option *G_define_option(void)
{
    struct Option *o = calloc(1, sizeof(*o));
    if (!o) { fprintf(stderr, "ras3d: G_define_option alloc failed\n"); exit(1); }
    if (ras3d_noptions < RAS3D_MAX_OPTIONS)
        ras3d_options[ras3d_noptions++] = o;
    return o;
}

struct Option *G_define_standard_option(StandardOption sopt)
{
    struct Option *o = G_define_option();
    switch (sopt) {
    case G_OPT_R3_INPUT:
        o->key = "input";  o->type = TYPE_STRING; o->required = YES;
        o->description = "Input 3-D raster map"; break;
    case G_OPT_R3_OUTPUT:
        o->key = "output"; o->type = TYPE_STRING; o->required = YES;
        o->description = "Output 3-D raster map"; break;
    case G_OPT_R_INPUT:
        o->key = "input";  o->type = TYPE_STRING; o->required = YES;
        o->description = "Input raster map"; break;
    case G_OPT_R_OUTPUT:
        o->key = "output"; o->type = TYPE_STRING; o->required = YES;
        o->description = "Output raster map"; break;
    default:
        o->key = "";  o->type = TYPE_STRING; o->required = NO; break;
    }
    return o;
}

struct Flag *G_define_flag(void)
{
    struct Flag *f = calloc(1, sizeof(*f));
    if (!f) { fprintf(stderr, "ras3d: G_define_flag alloc failed\n"); exit(1); }
    if (ras3d_nflags < RAS3D_MAX_FLAGS)
        ras3d_flags[ras3d_nflags++] = f;
    return f;
}

int G_parser(int argc, char **argv)
{
    int i, j;

    /* --help / -h */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            for (j = 0; j < ras3d_noptions; j++) {
                struct Option *o = ras3d_options[j];
                fprintf(stderr, "  %s=<%s>  %s%s\n",
                        o->key ? o->key : "?",
                        o->key_desc ? o->key_desc : "value",
                        o->description ? o->description : "",
                        o->required ? " [required]" : "");
            }
            for (j = 0; j < ras3d_nflags; j++) {
                struct Flag *f = ras3d_flags[j];
                fprintf(stderr, "  -%c  %s\n", f->key,
                        f->description ? f->description : "");
            }
            exit(0);
        }
    }

    /* Apply defaults */
    for (j = 0; j < ras3d_noptions; j++) {
        struct Option *o = ras3d_options[j];
        if (o->def && !o->answer)
            o->answer = (char *)o->def;
    }

    /* Parse */
    for (i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (arg[0] == '-' && arg[1] != '\0' && arg[1] != '-') {
            for (char *c = arg + 1; *c; c++) {
                int found = 0;
                for (j = 0; j < ras3d_nflags; j++) {
                    if (ras3d_flags[j]->key == *c) {
                        ras3d_flags[j]->answer = 1;
                        found = 1; break;
                    }
                }
                if (!found)
                    fprintf(stderr, "WARNING: unknown flag -%c\n", *c);
            }
            continue;
        }

        char *eq = strchr(arg, '=');
        if (!eq) { fprintf(stderr, "WARNING: unrecognised argument: %s\n", arg); continue; }

        size_t klen = (size_t)(eq - arg);
        char   key[256];
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, arg, klen); key[klen] = '\0';
        char *val = eq + 1;

        int found = 0;
        for (j = 0; j < ras3d_noptions; j++) {
            struct Option *o = ras3d_options[j];
            if (o->key && strcmp(o->key, key) == 0) {
                size_t vlen = strlen(val) + 1;
                char *dup = malloc(vlen);
                memcpy(dup, val, vlen);
                o->answer = dup;
                if (o->multiple) {
                    int n = 1;
                    for (char *p = val; *p; p++) if (*p == ',') n++;
                    o->answers = calloc((size_t)(n + 1), sizeof(char *));
                    char *tmp  = malloc(vlen);
                    memcpy(tmp, val, vlen);
                    char *tok  = strtok(tmp, ",");
                    int k = 0;
                    while (tok) {
                        size_t tl = strlen(tok) + 1;
                        o->answers[k] = malloc(tl);
                        memcpy(o->answers[k], tok, tl);
                        k++;
                        tok = strtok(NULL, ",");
                    }
                    o->answers[k] = NULL;
                    free(tmp);
                }
                found = 1; break;
            }
        }
        if (!found)
            fprintf(stderr, "WARNING: unknown option: %s\n", key);
    }

    int ok = 1;
    for (j = 0; j < ras3d_noptions; j++) {
        struct Option *o = ras3d_options[j];
        if (o->required && (!o->answer || o->answer[0] == '\0')) {
            fprintf(stderr, "ERROR: required option <%s> not set\n",
                    o->key ? o->key : "?");
            ok = 0;
        }
    }
    return ok ? 0 : 1;
}

/* ── 2-D raster I/O ───────────────────────────────────────────────────────── */

static struct rast2d_handle *alloc_handle(void)
{
    for (int i = 0; i < RAS3D_MAX_2D; i++)
        if (!_rast2d_table[i].in_use) return &_rast2d_table[i];
    G_fatal_error("ras3d: too many open 2-D raster files");
}

static int handle_fd(const struct rast2d_handle *h)
{
    return (int)(h - _rast2d_table);
}

static int resolve_path2d(const char *name, char *out, size_t outsz)
{
    FILE *f = fopen(name, "rb");
    if (f) { fclose(f); snprintf(out, outsz, "%s", name); return 1; }
    const char *exts[] = { ".tif", ".tiff", ".TIF", ".TIFF" };
    for (int i = 0; i < 4; i++) {
        snprintf(out, outsz, "%s%s", name, exts[i]);
        f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
    }
    const char *search = getenv("RAS3D_PATH");
    if (search) {
        snprintf(out, outsz, "%s/%s", search, name);
        f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
        for (int i = 0; i < 4; i++) {
            snprintf(out, outsz, "%s/%s%s", search, name, exts[i]);
            f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
        }
    }
    return 0;
}

int Rast_open_old(const char *name, const char *mapset)
{
    (void)mapset;
    char path[4096];
    if (!resolve_path2d(name, path, sizeof(path)))
        G_fatal_error("Rast_open_old: cannot find 2-D raster <%s>", name);

    TIFF *tif = XTIFFOpen(path, "r");
    if (!tif) G_fatal_error("Rast_open_old: XTIFFOpen failed for <%s>", path);

    struct rast2d_handle *h = alloc_handle();
    h->in_use  = 1;
    h->tif     = tif;
    h->gtif    = GTIFNew(tif);
    h->writing = 0;
    h->cur_row = 0;
    size_t plen = strlen(path) + 1;
    h->path = malloc(plen); memcpy(h->path, path, plen);

    uint32_t w = 0, ht = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH,  &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &ht);
    h->ncols = (int)w;
    h->nrows = (int)ht;

    uint16_t sf = SAMPLEFORMAT_IEEEFP, bps = 32;
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sf);
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    if (sf == SAMPLEFORMAT_INT || sf == SAMPLEFORMAT_UINT)
        h->type = CELL_TYPE;
    else if (bps == 32)
        h->type = FCELL_TYPE;
    else
        h->type = DCELL_TYPE;

    return handle_fd(h);
}

int Rast_open_new(const char *name, int type)
{
    char path[4096];
    size_t nlen = strlen(name);
    int has_ext = (nlen > 4 && (strcmp(name + nlen - 4, ".tif")  == 0 ||
                                strcmp(name + nlen - 5, ".tiff") == 0));
    if (has_ext)
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "%s.tif", name);

    const char *outdir = getenv("RAS3D_OUTDIR");
    if (outdir) {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s/%s", outdir, base);
        snprintf(path, sizeof(path), "%s", tmp);
    }

    TIFF *tif = XTIFFOpen(path, "w");
    if (!tif) G_fatal_error("Rast_open_new: XTIFFOpen failed for <%s>", path);

    struct rast2d_handle *h = alloc_handle();
    h->in_use  = 1;
    h->tif     = tif;
    h->gtif    = GTIFNew(tif);
    h->writing = 1;
    h->cur_row = 0;
    h->type    = type;
    h->nrows   = 0;
    h->ncols   = 0;
    size_t plen = strlen(path) + 1;
    h->path = malloc(plen); memcpy(h->path, path, plen);

    return handle_fd(h);
}

void Rast_close(int fd)
{
    if (fd < 0 || fd >= RAS3D_MAX_2D || !_rast2d_table[fd].in_use) return;
    struct rast2d_handle *h = &_rast2d_table[fd];
    if (h->writing) TIFFWriteDirectory(h->tif);
    if (h->gtif) GTIFFree(h->gtif);
    XTIFFClose(h->tif);
    free(h->path);
    memset(h, 0, sizeof(*h));
}

static int ncols_for_alloc(void)
{
    for (int i = 0; i < RAS3D_MAX_2D; i++)
        if (_rast2d_table[i].in_use && !_rast2d_table[i].writing)
            return _rast2d_table[i].ncols;
    if (ras3d_window_valid) return ras3d_current_window.cols;
    return 8192;
}

DCELL *Rast_allocate_d_buf(void) { return G_malloc((size_t)ncols_for_alloc() * sizeof(DCELL)); }
FCELL *Rast_allocate_f_buf(void) { return G_malloc((size_t)ncols_for_alloc() * sizeof(FCELL)); }
CELL  *Rast_allocate_c_buf(void) { return G_malloc((size_t)ncols_for_alloc() * sizeof(CELL));  }

void Rast_get_d_row(int fd, DCELL *buf, int row)
{
    struct rast2d_handle *h = &_rast2d_table[fd];
    tmsize_t scanline = TIFFScanlineSize(h->tif);
    void *tmp = G_malloc((size_t)scanline);
    TIFFReadScanline(h->tif, tmp, (uint32_t)row, 0);

    uint16_t sf = SAMPLEFORMAT_IEEEFP, bps = 32;
    TIFFGetFieldDefaulted(h->tif, TIFFTAG_SAMPLEFORMAT, &sf);
    TIFFGetFieldDefaulted(h->tif, TIFFTAG_BITSPERSAMPLE, &bps);

    for (int c = 0; c < h->ncols; c++) {
        double v;
        if (sf == SAMPLEFORMAT_IEEEFP)
            v = (bps == 64) ? ((double *)tmp)[c] : (double)((float *)tmp)[c];
        else if (sf == SAMPLEFORMAT_INT)
            v = (bps == 32) ? (double)((int32_t *)tmp)[c]
              : (bps == 16) ? (double)((int16_t *)tmp)[c]
                            : (double)((int8_t  *)tmp)[c];
        else
            v = (bps == 32) ? (double)((uint32_t *)tmp)[c]
              : (bps == 16) ? (double)((uint16_t *)tmp)[c]
                            : (double)((uint8_t  *)tmp)[c];
        buf[c] = v;
    }
    G_free(tmp);
}

static void setup_write_tags(struct rast2d_handle *h, int ncols)
{
    if (h->ncols) return;
    h->ncols = ncols;
    TIFFSetField(h->tif, TIFFTAG_IMAGEWIDTH,        (uint32_t)ncols);
    TIFFSetField(h->tif, TIFFTAG_PLANARCONFIG,      PLANARCONFIG_CONTIG);
    TIFFSetField(h->tif, TIFFTAG_SAMPLESPERPIXEL,   (uint16_t)1);
    TIFFSetField(h->tif, TIFFTAG_ROWSPERSTRIP,      (uint32_t)1);
    TIFFSetField(h->tif, TIFFTAG_COMPRESSION,       COMPRESSION_LZW);
    TIFFSetField(h->tif, TIFFTAG_PREDICTOR,         PREDICTOR_FLOATINGPOINT);
    if (h->type == CELL_TYPE) {
        TIFFSetField(h->tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)32);
        TIFFSetField(h->tif, TIFFTAG_SAMPLEFORMAT,  SAMPLEFORMAT_INT);
    } else if (h->type == FCELL_TYPE) {
        TIFFSetField(h->tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)32);
        TIFFSetField(h->tif, TIFFTAG_SAMPLEFORMAT,  SAMPLEFORMAT_IEEEFP);
    } else {
        TIFFSetField(h->tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)64);
        TIFFSetField(h->tif, TIFFTAG_SAMPLEFORMAT,  SAMPLEFORMAT_IEEEFP);
    }
}

void Rast_put_f_row(int fd, FCELL *buf)
{
    struct rast2d_handle *h = &_rast2d_table[fd];
    int ncols = ras3d_window_valid ? ras3d_current_window.cols : ncols_for_alloc();
    setup_write_tags(h, ncols);
    float *tmp;
    if (h->type == FCELL_TYPE) {
        tmp = buf;
    } else {
        tmp = G_malloc((size_t)ncols * sizeof(float));
        for (int c = 0; c < ncols; c++) tmp[c] = buf[c];
    }
    TIFFWriteScanline(h->tif, tmp, (uint32_t)h->cur_row, 0);
    if (h->type != FCELL_TYPE) G_free(tmp);
    h->cur_row++;
    h->nrows = h->cur_row;
}

void Rast_put_c_row(int fd, CELL *buf)
{
    struct rast2d_handle *h = &_rast2d_table[fd];
    int ncols = ras3d_window_valid ? ras3d_current_window.cols : ncols_for_alloc();
    setup_write_tags(h, ncols);
    TIFFWriteScanline(h->tif, buf, (uint32_t)h->cur_row, 0);
    h->cur_row++;
    h->nrows = h->cur_row;
}

/* ── null-value helpers ───────────────────────────────────────────────────── */
int Rast_is_d_null_value(const DCELL *v) { return isnan(*v) || *v == -9999.0; }

void Rast_set_f_null_value(FCELL *buf, int n) { for (int i=0;i<n;i++) buf[i]=(FCELL)NAN; }
void Rast_set_d_null_value(DCELL *buf, int n) { for (int i=0;i<n;i++) buf[i]=NAN; }
void Rast_set_c_null_value(CELL  *buf, int n) { for (int i=0;i<n;i++) buf[i]=(CELL)(-2147483648); }
