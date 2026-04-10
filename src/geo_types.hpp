#pragma once

namespace nasrbrowse
{
    struct geo_bbox
    {
        double lon_min;
        double lat_min;
        double lon_max;
        double lat_max;
    };

    struct airspace_point
    {
        double lat;
        double lon;
    };

} // namespace nasrbrowse
