/*
 * backend_hdf5.c — HDF5 read backend using serial libhdf5.
 *
 * Auto-probes for a radiance or reflectance dataset under any
 * "Data Fields" group in the file hierarchy.  Override with:
 *   $RAS3D_HDF5_DATASET  — absolute HDF5 path, e.g.
 *       /HDFEOS/SWATHS/HYP/Data Fields/toa_radiance
 *
 * Dataset assumed to have shape [bands][rows][cols].
 * Geo-location from Lat/Lon arrays in adjacent "Geolocation Fields" group,
 * or from HDF-EOS attribute metadata as fallback.
 *
 * Output is always via the GeoTIFF backend; HDF5 is read-only here.
 */
/* HDF5 before ras3d headers — avoids any macro/enum conflicts */
#include <hdf5.h>
#include <hdf5_hl.h>

#include "ras3d/ras3d.h"
#include "ras3d/ras3d_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>

/* ── private backend context ─────────────────────────────────────────────── */
typedef struct {
    hid_t  file_id;
    hid_t  dset_id;
    int    nbands, nrows, ncols;
    double north, south, east, west;
    double ns_res, ew_res;
    char   path[4096];
} hdf5_ctx_t;

/* ── dataset search ───────────────────────────────────────────────────────── */

typedef struct {
    char path[4096];   /* first matching dataset path */
    int  found;
    hid_t file_id;
    int   ndims;
    hsize_t dims[8];
} probe_ctx_t;

/* Callback for H5Lvisit — find radiance/reflectance datasets. */
static herr_t probe_cb(hid_t loc_id, const char *name,
                        const H5L_info_t *info, void *op_data)
{
    (void)info;
    probe_ctx_t *ctx = op_data;
    if (ctx->found) return 1; /* stop after first match */

    /* HDF5 1.12+ uses H5O_info2_t and requires a fields bitmask */
    H5O_info2_t oinfo;
    if (H5Oget_info_by_name(loc_id, name, &oinfo,
                            H5O_INFO_BASIC, H5P_DEFAULT) < 0) return 0;
    if (oinfo.type != H5O_TYPE_DATASET) return 0;

    /* Match name containing "radiance" or "reflectance" (case-insensitive) */
    char lower[512];
    size_t nl = strlen(name);
    if (nl >= sizeof(lower)) nl = sizeof(lower) - 1;
    for (size_t i = 0; i < nl; i++)
        lower[i] = (char)((name[i] >= 'A' && name[i] <= 'Z')
                          ? name[i] + 32 : name[i]);
    lower[nl] = '\0';

    int match = strstr(lower, "radiance")    != NULL
             || strstr(lower, "reflectance") != NULL
             || strstr(lower, "toa_rad")     != NULL
             || strstr(lower, "boa_ref")     != NULL
             || strstr(lower, "sr_")         != NULL;

    if (!match) return 0;

    /* Check it has 3 dimensions */
    hid_t dset = H5Dopen2(loc_id, name, H5P_DEFAULT);
    if (dset < 0) return 0;
    hid_t space = H5Dget_space(dset);
    int ndims = H5Sget_simple_extent_ndims(space);
    if (ndims == 3) {
        H5Sget_simple_extent_dims(space, ctx->dims, NULL);
        /* Build absolute path: loc_id is the file root here */
        snprintf(ctx->path, sizeof(ctx->path), "/%s", name);
        ctx->found = 1;
        ctx->ndims = ndims;
    }
    H5Sclose(space);
    H5Dclose(dset);
    return ctx->found ? 1 : 0;
}

/* Find the target dataset path, respecting $RAS3D_HDF5_DATASET override. */
static int find_dataset(hid_t file_id, hdf5_ctx_t *ctx)
{
    const char *override = getenv("RAS3D_HDF5_DATASET");
    if (override) {
        hid_t dset = H5Dopen2(file_id, override, H5P_DEFAULT);
        if (dset < 0) {
            G_warning("RAS3D_HDF5_DATASET='%s' not found in HDF5 file", override);
            return 0;
        }
        hid_t space = H5Dget_space(dset);
        int nd = H5Sget_simple_extent_ndims(space);
        hsize_t dims[8];
        H5Sget_simple_extent_dims(space, dims, NULL);
        H5Sclose(space);
        H5Dclose(dset);
        if (nd != 3) {
            G_warning("Dataset '%s' has %d dims, expected 3", override, nd);
            return 0;
        }
        snprintf(ctx->path, sizeof(ctx->path), "%s", override);
        ctx->nbands = (int)dims[0];
        ctx->nrows  = (int)dims[1];
        ctx->ncols  = (int)dims[2];
        return 1;
    }

    /* Auto-probe */
    probe_ctx_t pctx = { .found = 0, .file_id = file_id };
    H5Lvisit(file_id, H5_INDEX_NAME, H5_ITER_NATIVE, probe_cb, &pctx);
    if (!pctx.found) {
        G_warning("No radiance/reflectance dataset found; "
                  "set RAS3D_HDF5_DATASET to override");
        return 0;
    }
    snprintf(ctx->path, sizeof(ctx->path), "%s", pctx.path);
    ctx->nbands = (int)pctx.dims[0];
    ctx->nrows  = (int)pctx.dims[1];
    ctx->ncols  = (int)pctx.dims[2];
    return 1;
}

/* ── geo-location from lat/lon arrays ────────────────────────────────────── */

/* Iterator context for finding a geo field by name substring */
typedef struct { const char *want; hid_t found; } geo_iter_t;

static herr_t geo_iter_cb(hid_t grp, const char *name,
                           const H5L_info_t *linfo, void *op_data)
{
    (void)linfo;
    geo_iter_t *ctx = op_data;
    if (ctx->found != H5I_INVALID_HID) return 1;

    char lower[256];
    size_t nl = strlen(name);
    if (nl >= sizeof(lower)) nl = sizeof(lower) - 1;
    for (size_t k = 0; k < nl; k++)
        lower[k] = (char)((name[k] >= 'A' && name[k] <= 'Z') ? name[k]+32 : name[k]);
    lower[nl] = '\0';

    if (strstr(lower, ctx->want))
        ctx->found = H5Dopen2(grp, name, H5P_DEFAULT);
    return ctx->found != H5I_INVALID_HID ? 1 : 0;
}

/* Try to load Lat or Lon from a sibling "Geolocation Fields" group.
 * Looks for a dataset whose name contains "lat" or "lon". */
static hid_t open_geo_field(hid_t file_id, const char *data_path,
                             const char *want)
{
    /* data_path example: /HDFEOS/SWATHS/HYP/Data Fields/toa_radiance
     * sibling:           /HDFEOS/SWATHS/HYP/Geolocation Fields/<lat|lon> */
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", data_path);
    /* Go up two levels (strip "Data Fields/<name>") */
    char *p = strrchr(parent, '/'); if (p) *p = '\0';
    p = strrchr(parent, '/'); if (p) *p = '\0';

    char geo_group[4096];
    snprintf(geo_group, sizeof(geo_group), "%s/Geolocation Fields", parent);

    if (H5Lexists(file_id, geo_group, H5P_DEFAULT) <= 0)
        return H5I_INVALID_HID;

    hid_t grp = H5Gopen2(file_id, geo_group, H5P_DEFAULT);
    if (grp < 0) return H5I_INVALID_HID;

    geo_iter_t ctx = { .want = want, .found = H5I_INVALID_HID };
    hsize_t idx = 0;
    H5Literate(grp, H5_INDEX_NAME, H5_ITER_NATIVE, &idx, geo_iter_cb, &ctx);
    H5Gclose(grp);
    return ctx.found;
}

static void compute_geo_from_latlon(hid_t file_id, const char *dset_path,
                                    int nrows, int ncols,
                                    hdf5_ctx_t *ctx)
{
    hid_t lat_dset = open_geo_field(file_id, dset_path, "lat");
    hid_t lon_dset = open_geo_field(file_id, dset_path, "lon");

    if (lat_dset == H5I_INVALID_HID || lon_dset == H5I_INVALID_HID) {
        G_warning("HDF5: cannot find Lat/Lon arrays; using unit geo-transform");
        ctx->north  = (double)nrows;
        ctx->south  = 0.0;
        ctx->west   = 0.0;
        ctx->east   = (double)ncols;
        ctx->ns_res = 1.0;
        ctx->ew_res = 1.0;
        ras3d_current_epsg = 0;
        ras3d_current_geographic = 0;
        if (lat_dset != H5I_INVALID_HID) H5Dclose(lat_dset);
        if (lon_dset != H5I_INVALID_HID) H5Dclose(lon_dset);
        return;
    }

    /* Read corner values only (first/last row and col) for bounding box */
    hsize_t start[2] = {0, 0}, count[2] = {1, 1};
    hsize_t mem_dims[2] = {1, 1};
    hid_t memsp  = H5Screate_simple(2, mem_dims, NULL);
    hid_t sp_lat = H5Dget_space(lat_dset);
    hid_t sp_lon = H5Dget_space(lon_dset);

    double lat_n, lat_s, lon_w, lon_e;

    /* top-left */
    start[0] = 0; start[1] = 0;
    H5Sselect_hyperslab(sp_lat, H5S_SELECT_SET, start, NULL, count, NULL);
    H5Dread(lat_dset, H5T_NATIVE_DOUBLE, memsp, sp_lat, H5P_DEFAULT, &lat_n);
    H5Sselect_hyperslab(sp_lon, H5S_SELECT_SET, start, NULL, count, NULL);
    H5Dread(lon_dset, H5T_NATIVE_DOUBLE, memsp, sp_lon, H5P_DEFAULT, &lon_w);

    /* bottom-right */
    start[0] = (hsize_t)(nrows - 1); start[1] = (hsize_t)(ncols - 1);
    H5Sselect_hyperslab(sp_lat, H5S_SELECT_SET, start, NULL, count, NULL);
    H5Dread(lat_dset, H5T_NATIVE_DOUBLE, memsp, sp_lat, H5P_DEFAULT, &lat_s);
    H5Sselect_hyperslab(sp_lon, H5S_SELECT_SET, start, NULL, count, NULL);
    H5Dread(lon_dset, H5T_NATIVE_DOUBLE, memsp, sp_lon, H5P_DEFAULT, &lon_e);

    H5Sclose(sp_lat); H5Sclose(sp_lon); H5Sclose(memsp);
    H5Dclose(lat_dset); H5Dclose(lon_dset);

    ctx->north  = lat_n;
    ctx->south  = lat_s;
    ctx->west   = lon_w;
    ctx->east   = lon_e;
    ctx->ns_res = (lat_n - lat_s) / (double)nrows;
    ctx->ew_res = (lon_e - lon_w) / (double)ncols;
    /* Extent taken from Lat/Lon arrays: WGS84 geographic. */
    ras3d_current_epsg = 4326;
    ras3d_current_geographic = 1;
}

/* ── read backend ─────────────────────────────────────────────────────────── */

static int hdf5_read_block(void *ctx,
                           int x0, int y0, int z0,
                           int nx, int ny, int nz,
                           void *buf, int type)
{
    hdf5_ctx_t *h = ctx;
    (void)x0; (void)y0;

    /* Read one band at a time (serial HDF5); process in parallel */
    int npix_per_band = nx * ny;
    DCELL *dcell_buf = G_malloc((size_t)npix_per_band * nz * sizeof(DCELL));

    for (int dz = 0; dz < nz; dz++) {
        int band = z0 + dz;

        /* Hyperslab: [band][row_start:row_start+ny][col_start:col_start+nx] */
        hsize_t start[3]  = { (hsize_t)band, (hsize_t)y0, (hsize_t)x0 };
        hsize_t count[3]  = { 1, (hsize_t)ny, (hsize_t)nx };
        hsize_t mem_dims[2] = { (hsize_t)ny, (hsize_t)nx };

        hid_t file_space = H5Dget_space(h->dset_id);
        H5Sselect_hyperslab(file_space, H5S_SELECT_SET,
                            start, NULL, count, NULL);
        hid_t mem_space = H5Screate_simple(2, mem_dims, NULL);

        /* Always read as native float, then promote */
        float *float_row = G_malloc((size_t)npix_per_band * sizeof(float));
        herr_t err = H5Dread(h->dset_id, H5T_NATIVE_FLOAT,
                             mem_space, file_space, H5P_DEFAULT, float_row);
        H5Sclose(mem_space);
        H5Sclose(file_space);

        if (err < 0) {
            G_warning("HDF5: H5Dread failed for band %d", band);
            G_free(float_row); G_free(dcell_buf);
            return 0;
        }

        DCELL *dst = dcell_buf + dz * npix_per_band;
#pragma omp parallel for simd schedule(static)
        for (int i = 0; i < npix_per_band; i++)
            dst[i] = (DCELL)float_row[i];

        G_free(float_row);
    }

    if (type == DCELL_TYPE) {
        memcpy(buf, dcell_buf, (size_t)npix_per_band * nz * sizeof(DCELL));
    } else {
        float *fbuf = buf;
        int ntotal = npix_per_band * nz;
#pragma omp parallel for simd schedule(static)
        for (int i = 0; i < ntotal; i++) fbuf[i] = (float)dcell_buf[i];
    }
    G_free(dcell_buf);
    return 1;
}

static int hdf5_close(void *ctx)
{
    hdf5_ctx_t *h = ctx;
    H5Dclose(h->dset_id);
    H5Fclose(h->file_id);
    free(h);
    return 1;
}

/* ── public constructor ───────────────────────────────────────────────────── */

int ras3d_open_hdf5_read(const char *path, RASTER3D_Map *map)
{
    hid_t file_id = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0) {
        G_warning("ras3d_open_hdf5_read: cannot open <%s>", path);
        return 0;
    }

    hdf5_ctx_t *h = calloc(1, sizeof(*h));
    h->file_id = file_id;

    if (!find_dataset(file_id, h)) {
        H5Fclose(file_id);
        free(h);
        return 0;
    }

    h->dset_id = H5Dopen2(file_id, h->path, H5P_DEFAULT);
    if (h->dset_id < 0) {
        G_warning("ras3d_open_hdf5_read: H5Dopen2 failed for '%s'", h->path);
        H5Fclose(file_id);
        free(h);
        return 0;
    }

    compute_geo_from_latlon(file_id, h->path, h->nrows, h->ncols, h);

    /* Fill map region */
    map->region.rows   = h->nrows;
    map->region.cols   = h->ncols;
    map->region.depths = h->nbands;
    map->region.north  = h->north;
    map->region.south  = h->south;
    map->region.east   = h->east;
    map->region.west   = h->west;
    map->region.ns_res = h->ns_res;
    map->region.ew_res = h->ew_res;
    map->region.top    = (double)h->nbands;
    map->region.bottom = 0.0;
    map->region.tb_res = 1.0;
    map->window   = map->region;
    map->type     = FCELL_TYPE;
    map->typeIntern = DCELL_TYPE;
    map->operation  = RASTER3D_READ_DATA;

    map->backend.read_block  = hdf5_read_block;
    map->backend.write_value = NULL;
    map->backend.close       = hdf5_close;
    map->backend.ctx         = h;
    map->out_buf             = NULL;
    return 1;
}
