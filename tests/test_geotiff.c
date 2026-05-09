/*
 * test_geotiff.c — open the Wyvern hyperspectral GeoTIFF and verify
 * that ras3d reports the correct dimensions (23 bands, 6003×7825).
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ras3d/ras3d.h"

#define WYVERN_PATH \
    "/home/yann/RSDATA/Wyvern_Dragonette_001_20240808_agri/" \
    "wyvern_dragonette-001_20240808T073501_51b92993.tiff"

int main(void)
{
    int pass = 1;

    const char *mapset = G_find_raster3d(WYVERN_PATH, "");
    if (!mapset) {
        fprintf(stderr, "FAIL: G_find_raster3d returned NULL\n");
        return 1;
    }

    RASTER3D_Region region;
    Rast3d_get_window(&region);

    RASTER3D_Map *map = Rast3d_open_cell_old(
        WYVERN_PATH, mapset, &region,
        RASTER3D_TILE_SAME_AS_FILE, RASTER3D_NO_CACHE);

    if (!map) {
        fprintf(stderr, "FAIL: Rast3d_open_cell_old returned NULL\n");
        return 1;
    }

    Rast3d_get_region_struct_map(map, &region);

    printf("GeoTIFF dims: cols=%d rows=%d bands=%d\n",
           region.cols, region.rows, region.depths);
    printf("Extent: N=%.6f S=%.6f W=%.6f E=%.6f\n",
           region.north, region.south, region.west, region.east);
    printf("Resolution: ew=%.8f ns=%.8f\n", region.ew_res, region.ns_res);

    if (region.cols != 6003)   { fprintf(stderr, "FAIL: cols=%d (expected 6003)\n", region.cols);   pass=0; }
    if (region.rows != 7825)   { fprintf(stderr, "FAIL: rows=%d (expected 7825)\n", region.rows);   pass=0; }
    if (region.depths != 23)   { fprintf(stderr, "FAIL: depths=%d (expected 23)\n", region.depths); pass=0; }

    /* Read one z-slice (band 0) and check it's non-trivial */
    int ncols = region.cols, nrows = region.rows;
    DCELL *buf = G_malloc((size_t)ncols * nrows * sizeof(DCELL));
    Rast3d_get_block(map, 0, 0, 0, ncols, nrows, 1, buf, DCELL_TYPE);

    /* Find min/max of first band */
    double mn = buf[0], mx = buf[0];
    for (int i = 0; i < ncols * nrows; i++) {
        if (!isnan(buf[i]) && buf[i] < mn) mn = buf[i];
        if (!isnan(buf[i]) && buf[i] > mx) mx = buf[i];
    }
    printf("Band 0: min=%.4f max=%.4f\n", mn, mx);
    if (mn == mx) { fprintf(stderr, "FAIL: band 0 is constant\n"); pass=0; }

    G_free(buf);
    Rast3d_close(map);

    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
