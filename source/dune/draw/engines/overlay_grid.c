#include "draw_render.h"

#include "types_camera.h"
#include "types_screen.h"

#include "dgraph_query.h"

#include "ed_image.h"
#include "ed_view3d.h"

#include "uiDefIconTextBtn_resources.h"

#include "overlay_private.h"

enum {
  SHOW_AXIS_X = (1 << 0),
  SHOW_AXIS_Y = (1 << 1),
  SHOW_AXIS_Z = (1 << 2),
  SHOW_GRID = (1 << 3),
  PLANE_XY = (1 << 4),
  PLANE_XZ = (1 << 5),
  PLANE_YZ = (1 << 6),
  CLIP_ZPOS = (1 << 7),
  CLIP_ZNEG = (1 << 8),
  GRID_BACK = (1 << 9),
  GRID_CAMERA = (1 << 10),
  PLANE_IMAGE = (1 << 11),
  CUSTOM_GRID = (1 << 12),
};

void overlay_grid_init(OverlayData *vedata)
{
  OverlayPrivateData *pd = vedata->stl->pd;
  OverlayShadingData *shd = &pd->shdata;
  const DrawCtxState *draw_ctx = draw_ctx_state_get();

  shd->grid_flag = 0;
  shd->zneg_flag = 0;
  shd->zpos_flag = 0;
  shd->grid_line_size = max_ff(0.0f, U.pixelsize - 1.0f) * 0.5f;

  if (pd->space_type == SPACE_IMAGE) {
    SpaceImage *sima = (SpaceImage *)draw_ctx->space_data;
    View2D *v2d = &draw_ctx->region->v2d;
    if (sima->mode == SI_MODE_UV || !ed_space_image_has_buffer(sima)) {
      shd->grid_flag = GRID_BACK | PLANE_IMAGE | SHOW_GRID;
    }
    else {
      shd->grid_flag = 0;
    }

    if (sima->flag & SI_CUSTOM_GRID) {
      shd->grid_flag |= CUSTOM_GRID;
    }

    shd->grid_distance = 1.0f;
    copy_v3_fl3(shd->grid_size, 1.0f, 1.0f, 1.0f);
    if (sima->mode == SI_MODE_UV) {
      shd->grid_size[0] = (float)sima->tile_grid_shape[0];
      shd->grid_size[1] = (float)sima->tile_grid_shape[1];
    }

    const int grid_size = SI_GRID_STEPS_LEN;
    shd->zoom_factor = ed_space_image_zoom_level(v2d, grid_size);
    ed_space_image_grid_steps(sima, shd->grid_steps, grid_size);

    return;
  }

  View3D *v3d = draw_ctx->v3d;
  Scene *scene = draw_ctx->scene;
  RegionView3D *rv3d = draw_ctx->rv3d;

  const bool show_axis_x = (pd->v3d_gridflag & V3D_SHOW_X) != 0;
  const bool show_axis_y = (pd->v3d_gridflag & V3D_SHOW_Y) != 0;
  const bool show_axis_z = (pd->v3d_gridflag & V3D_SHOW_Z) != 0;
  const bool show_floor = (pd->v3d_gridflag & V3D_SHOW_FLOOR) != 0;
  const bool show_ortho_grid = (pd->v3d_gridflag & V3D_SHOW_ORTHO_GRID) != 0;

  if (pd->hide_overlays || !(pd->v3d_gridflag & (V3D_SHOW_X | V3D_SHOW_Y | V3D_SHOW_Z |
                                                 V3D_SHOW_FLOOR | V3D_SHOW_ORTHO_GRID))) {
    return;
  }

  float viewinv[4][4], wininv[4][4];
  float viewmat[4][4], winmat[4][4];
  draw_view_winmat_get(NULL, winmat, false);
  draw_view_winmat_get(NULL, wininv, true);
  draw_view_viewmat_get(NULL, viewmat, false);
  draw_view_viewmat_get(NULL, viewinv, true);

  /* If perspective view or non-axis aligned view. */
  if (winmat[3][3] == 0.0f || rv3d->view == RV3D_VIEW_USER) {
    if (show_axis_x) {
      shd->grid_flag |= PLANE_XY | SHOW_AXIS_X;
    }
    if (show_axis_y) {
      shd->grid_flag |= PLANE_XY | SHOW_AXIS_Y;
    }
    if (show_floor) {
      shd->grid_flag |= PLANE_XY | SHOW_GRID;
    }
  }
  else {
    if (show_ortho_grid && ELEM(rv3d->view, RV3D_VIEW_RIGHT, RV3D_VIEW_LEFT)) {
      shd->grid_flag = PLANE_YZ | SHOW_AXIS_Y | SHOW_AXIS_Z | SHOW_GRID | GRID_BACK;
    }
    else if (show_ortho_grid && ELEM(rv3d->view, RV3D_VIEW_TOP, RV3D_VIEW_BOTTOM)) {
      shd->grid_flag = PLANE_XY | SHOW_AXIS_X | SHOW_AXIS_Y | SHOW_GRID | GRID_BACK;
    }
    else if (show_ortho_grid && ELEM(rv3d->view, RV3D_VIEW_FRONT, RV3D_VIEW_BACK)) {
      shd->grid_flag = PLANE_XZ | SHOW_AXIS_X | SHOW_AXIS_Z | SHOW_GRID | GRID_BACK;
    }
  }

  shd->grid_axes[0] = (float)((shd->grid_flag & (PLANE_XZ | PLANE_XY)) != 0);
  shd->grid_axes[1] = (float)((shd->grid_flag & (PLANE_YZ | PLANE_XY)) != 0);
  shd->grid_axes[2] = (float)((shd->grid_flag & (PLANE_YZ | PLANE_XZ)) != 0);

  /* Z axis if needed */
  if (((rv3d->view == RV3D_VIEW_USER) || (rv3d->persp != RV3D_ORTHO)) && show_axis_z) {
    shd->zpos_flag = SHOW_AXIS_Z;

    float zvec[3], campos[3];
    negate_v3_v3(zvec, viewinv[2]);
    copy_v3_v3(campos, viewinv[3]);

    /* z axis : chose the most facing plane */
    if (fabsf(zvec[0]) < fabsf(zvec[1])) {
      shd->zpos_flag |= PLANE_XZ;
    }
    else {
      shd->zpos_flag |= PLANE_YZ;
    }

    shd->zneg_flag = shd->zpos_flag;

    /* Persp : If camera is below floor plane, we switch clipping
     * Ortho : If eye vector is looking up, we switch clipping */
    if (((winmat[3][3] == 0.0f) && (campos[2] > 0.0f)) ||
        ((winmat[3][3] != 0.0f) && (zvec[2] < 0.0f))) {
      shd->zpos_flag |= CLIP_ZPOS;
      shd->zneg_flag |= CLIP_ZNEG;
    }
    else {
      shd->zpos_flag |= CLIP_ZNEG;
      shd->zneg_flag |= CLIP_ZPOS;
    }

    shd->zplane_axes[0] = (float)((shd->zpos_flag & (PLANE_XZ | PLANE_XY)) != 0);
    shd->zplane_axes[1] = (float)((shd->zpos_flag & (PLANE_YZ | PLANE_XY)) != 0);
    shd->zplane_axes[2] = (float)((shd->zpos_flag & (PLANE_YZ | PLANE_XZ)) != 0);
  }
  else {
    shd->zneg_flag = shd->zpos_flag = CLIP_ZNEG | CLIP_ZPOS;
  }

  float dist;
  if (rv3d->persp == RV3D_CAMOB && v3d->camera && v3d->camera->type == OB_CAMERA) {
    Object *camera_object = DEG_get_evaluated_object(draw_ctx->dgraph, v3d->camera);
    dist = ((Camera *)(camera_object->data))->clip_end;
    shd->grid_flag |= GRID_CAMERA;
    shd->zneg_flag |= GRID_CAMERA;
    shd->zpos_flag |= GRID_CAMERA;
  }
  else {
    dist = v3d->clip_end;
  }

  if (winmat[3][3] == 0.0f) {
    copy_v3_fl(shd->grid_size, dist);
  }
  else {
    float viewdist = 1.0f / min_ff(fabsf(winmat[0][0]), fabsf(winmat[1][1]));
    copy_v3_fl(shd->grid_size, viewdist * dist);
  }

  shd->grid_distance = dist / 2.0f;

  ed_view3d_grid_steps(scene, v3d, rv3d, shd->grid_steps);

  if ((v3d->flag & (V3D_XR_SESSION_SURFACE | V3D_XR_SESSION_MIRROR)) != 0) {
    /* The calculations for the grid parameters assume that the view matrix has no scale component,
     * which may not be correct if the user is "shrunk" or "enlarged" by zooming in or out.
     * Therefore, we need to compensate the values here. */
    float viewinvscale = len_v3(
        viewinv[0]); /* Assumption is uniform scaling (all column vectors are of same length). */
    shd->grid_distance *= viewinvscale;
  }
}

void overlay_grid_cache_init(OverlayData *vedata)
{
  OverlayStorageList *stl = vedata->stl;
  OverlayPrivateData *pd = stl->pd;
  OverlayShadingData *shd = &pd->shdata;

  OverlayPassList *psl = vedata->psl;
  DefaultTextureList *dtxl = draw_viewport_texture_list_get();

  psl->grid_ps = NULL;

  if ((shd->grid_flag == 0 && shd->zpos_flag == 0) || !draw_state_is_fbo()) {
    return;
  }

  DrawState state = DRAW_STATE_WRITE_COLOR | DRAW_STATE_BLEND_ALPHA;
  DRAW_PASS_CREATE(psl->grid_ps, state);
  DrawShadingGroup *grp;
  GPUShader *sh;
  struct GPUBatch *geom = draw_cache_grid_get();

  if (pd->space_type == SPACE_IMAGE) {
    float mat[4][4];

    /* add quad background */
    sh = overlay_shader_grid_background();
    grp = draw_shgroup_create(sh, psl->grid_ps);
    float color_back[4];
    interp_v4_v4v4(color_back, G_draw.block.colorBackground, G_draw.block.colorGrid, 0.5);
    draw_shgroup_uniform_vec4_copy(grp, "color", color_back);
    draw_shgroup_uniform_texture_ref(grp, "depthBuffer", &dtxl->depth);
    unit_m4(mat);
    mat[0][0] = shd->grid_size[0];
    mat[1][1] = shd->grid_size[1];
    mat[2][2] = shd->grid_size[2];
    draw_shgroup_call_obmat(grp, draw_cache_quad_get(), mat);
  }

  sh = overlay_shader_grid();

  /* Create 3 quads to render ordered transparency Z axis */
  grp = draw_shgroup_create(sh, psl->grid_ps);
  draw_shgroup_uniform_int(grp, "gridFlag", &shd->zneg_flag, 1);
  draw_shgroup_uniform_vec3(grp, "planeAxes", shd->zplane_axes, 1);
  draw_shgroup_uniform_float(grp, "gridDistance", &shd->grid_distance, 1);
  draw_shgroup_uniform_float_copy(grp, "lineKernel", shd->grid_line_size);
  draw_shgroup_uniform_vec3(grp, "gridSize", shd->grid_size, 1);
  draw_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
  draw_shgroup_uniform_texture_ref(grp, "depthBuffer", &dtxl->depth);
  if (shd->zneg_flag & SHOW_AXIS_Z) {
    draw_shgroup_call(grp, geom, NULL);
  }

  grp = draw_shgroup_create(sh, psl->grid_ps);
  draw_shgroup_uniform_int(grp, "gridFlag", &shd->grid_flag, 1);
  draw_shgroup_uniform_float_copy(grp, "zoomFactor", shd->zoom_factor);
  draw_shgroup_uniform_vec3(grp, "planeAxes", shd->grid_axes, 1);
  draw_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
  draw_shgroup_uniform_texture_ref(grp, "depthBuffer", &dtxl->depth);
  draw_shgroup_uniform_float(grp, "gridSteps", shd->grid_steps, ARRAY_SIZE(shd->grid_steps));
  if (shd->grid_flag) {
    draw_shgroup_call(grp, geom, NULL);
  }

  grp = draw_shgroup_create(sh, psl->grid_ps);
  draw_shgroup_uniform_int(grp, "gridFlag", &shd->zpos_flag, 1);
  draw_shgroup_uniform_vec3(grp, "planeAxes", shd->zplane_axes, 1);
  draw_shgroup_uniform_block(grp, "globalsBlock", G_draw.block_ubo);
  draw_shgroup_uniform_texture_ref(grp, "depthBuffer", &dtxl->depth);
  if (shd->zpos_flag & SHOW_AXIS_Z) {
    draw_shgroup_call(grp, geom, NULL);
  }

  if (pd->space_type == SPACE_IMAGE) {
    float theme_color[4];
    ui_GetThemeColorShade4fv(TH_BACK, 60, theme_color);
    srgb_to_linearrgb_v4(theme_color, theme_color);

    float mat[4][4];
    /* add wire border */
    sh = overlay_shader_grid_image();
    grp = draw_shgroup_create(sh, psl->grid_ps);
    draw_shgroup_uniform_vec4_copy(grp, "color", theme_color);
    unit_m4(mat);
    for (int x = 0; x < shd->grid_size[0]; x++) {
      mat[3][0] = x;
      for (int y = 0; y < shd->grid_size[1]; y++) {
        mat[3][1] = y;
        draw_shgroup_call_obmat(grp, draw_cache_quad_wires_get(), mat);
      }
    }
  }
}

void OVERLAY_grid_draw(OVERLAY_Data *vedata)
{
  OVERLAY_PassList *psl = vedata->psl;

  if (psl->grid_ps) {
    DRW_draw_pass(psl->grid_ps);
  }
}
