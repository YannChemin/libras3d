/*
 * region.c — RASTER3D_Region helpers.
 */
#include <string.h>
#include <stdlib.h>
#include "ras3d/ras3d.h"
#include "ras3d/ras3d_internal.h"

void Rast3d_get_window(RASTER3D_Region *region)
{
    if (ras3d_window_valid) {
        *region = ras3d_current_window;
        return;
    }
    /* Minimal zero-fill; will be overwritten after opening the first map. */
    memset(region, 0, sizeof(*region));
}

void Rast3d_set_window(RASTER3D_Region *region)
{
    ras3d_current_window = *region;
    ras3d_window_valid   = 1;
}

void Rast3d_get_region_struct_map(RASTER3D_Map *map, RASTER3D_Region *region)
{
    *region = map->region;
    /* Also update global window so later Rast3d_get_window calls are sane. */
    if (!ras3d_window_valid) {
        ras3d_current_window = map->region;
        ras3d_window_valid   = 1;
    }
}
