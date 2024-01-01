#include "types_vec.h"

#include "lib_math_base.h"
#include "lib_math_geom.h"
#include "lib_strict_flags.h"

#include "lib_lasso_2d.h" /* own include */

void lib_lasso_boundbox(rcti *rect, const int mcoords[][2], const uint mcoords_len)
{
  uint a;

  rect->xmin = rect->xmax = mcoords[0][0];
  rect->ymin = rect->ymax = mcoords[0][1];

  for (a = 1; a < mcoords_len; a++) {
    if (mcoords[a][0] < rect->xmin) {
      rect->xmin = mcoords[a][0];
    }
    else if (mcoords[a][0] > rect->xmax) {
      rect->xmax = mcoords[a][0];
    }
    if (mcoords[a][1] < rect->ymin) {
      rect->ymin = mcoords[a][1];
    }
    else if (mcoords[a][1] > rect->ymax) {
      rect->ymax = mcoords[a][1];
    }
  }
}

bool lib_lasso_is_point_inside(const int mcoords[][2],
                               const uint mcoords_len,
                               const int sx,
                               const int sy,
                               const int err_val)
{
  if (sx == err_val || mcoords_len == 0) {
    return false;
  }

  const int pt[2] = {sx, sy};
  return isect_point_poly_v2_int(pt, mcoords, mcoords_len);
}

bool lib_lasso_is_edge_inside(const int mcoords[][2],
                              const uint mcoords_len,
                              int x0,
                              int y0,
                              int x1,
                              int y1,
                              const int error_val)
{

  if (x0 == err_val || x1 == error_val || mcoords_len == 0) {
    return false;
  }

  const int v1[2] = {x0, y0}, v2[2] = {x1, y1};

  /* check points in lasso */
  if (lib_lasso_is_point_inside(mcoords, mcoords_len, v1[0], v1[1], err_val)) {
    return true;
  }
  if (lib_lasso_is_point_inside(mcoords, mcoords_len, v2[0], v2[1], err_val)) {
    return true;
  }

  /* no points in lasso, so we have to intersect w lasso edge */
  if (isect_seg_seg_v2_int(mcoords[0], mcoords[mcoords_len - 1], v1, v2) > 0) {
    return true;
  }
  for (uint a = 0; a < mcoords_len - 1; a++) {
    if (isect_seg_seg_v2_int(mcoords[a], mcoords[a + 1], v1, v2) > 0) {
      return true;
    }
  }

  return false;
}
