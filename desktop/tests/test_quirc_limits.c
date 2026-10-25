// Exercise the internal search with a degenerate transform, without a camera or
// a fragile image fixture. This used to perform over a billion region lookups.
#include "identify.c"

int main(void)
{
    struct quirc *q = quirc_new();
    if (!q || quirc_resize(q, 32, 32) != 0)
        return 1;
    if (QUIRC_PIXEL_ALIAS_IMAGE)
        q->pixels = q->image;
    memset(q->pixels, 0, 32 * 32 * sizeof(*q->pixels));
    q->grids[0].caps[0] = 0;
    q->grids[0].caps[2] = 2;
    q->grids[0].align.x = 16;
    q->grids[0].align.y = 16;
    q->grids[0].align_region = -1;
    q->capstones[0].c[0] = q->capstones[0].c[4] = 10000;
    q->capstones[2].c[0] = q->capstones[2].c[4] = 10000;
    find_alignment_pattern(q, 0);
    const int failed = q->grids[0].align_region != -1;
    quirc_destroy(q);
    return failed;
}
