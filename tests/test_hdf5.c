/*
 * test_hdf5.c — open the Tanager HDF5 file and verify that ras3d
 * reports the correct dimensions (426 bands, 732×607).
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ras3d/ras3d.h"

#define TANAGER_PATH \
    "/home/yann/RSDATA/Tanager/Kanpur/" \
    "20250321_054913_40_4001_basic_radiance_hdf5.h5"

int main(void)
{
    int pass = 1;

    const char *mapset = G_find_raster3d(TANAGER_PATH, "");
    if (!mapset) {
        fprintf(stderr, "FAIL: G_find_raster3d returned NULL\n");
        return 1;
    }

    RASTER3D_Region region;
    Rast3d_get_window(&region);

    RASTER3D_Map *map = Rast3d_open_cell_old(
        TANAGER_PATH, mapset, &region,
        RASTER3D_TILE_SAME_AS_FILE, RASTER3D_NO_CACHE);

    if (!map) {
        fprintf(stderr, "FAIL: Rast3d_open_cell_old returned NULL\n");
        return 1;
    }

    Rast3d_get_region_struct_map(map, &region);

    printf("HDF5 dims: cols=%d rows=%d bands=%d\n",
           region.cols, region.rows, region.depths);
    printf("Extent: N=%.6f S=%.6f W=%.6f E=%.6f\n",
           region.north, region.south, region.west, region.east);

    if (region.depths != 426) { fprintf(stderr, "FAIL: bands=%d (expected 426)\n", region.depths); pass=0; }
    if (region.rows   != 732) { fprintf(stderr, "FAIL: rows=%d (expected 732)\n",  region.rows);   pass=0; }
    if (region.cols   != 607) { fprintf(stderr, "FAIL: cols=%d (expected 607)\n",  region.cols);   pass=0; }

    /* Read band 0 and check non-trivial */
    int ncols = region.cols, nrows = region.rows;
    DCELL *buf = G_malloc((size_t)ncols * nrows * sizeof(DCELL));
    Rast3d_get_block(map, 0, 0, 0, ncols, nrows, 1, buf, DCELL_TYPE);

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
