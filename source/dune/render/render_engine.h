#pragma once

#include "types_listBase.h"
#include "types_node_types.h"
#include "types_scene_types.h"
#include "render_bake.h"
#include "api_types.h"

#include "lib_threads.h"

struct BakePixel;
struct Graph;
struct Main;
struct Object;
struct Render;
struct RenderData;
struct RenderEngine;
struct RenderEngineType;
struct RenderLayer;
struct RenderPass;
struct RenderResult;
struct ReportList;
struct Scene;
struct ViewLayer;
struct Node;
struct NodeTree;

#ifdef __cplusplus
extern "C" {
#endif

/* External Engine */

/* RenderEngineType.flag */
#define RENDER_INTERNAL 1
/* #define RENDER_FLAG_DEPRECATED   2 */
#define RENDER_USE_PREVIEW 4
#define RENDER_USE_POSTPROCESS 8
#define RENDER_USE_EEVEE_VIEWPORT 16
/* #define RENDER_USE_SAVE_BUFFERS_DEPRECATED 32 */
#define RENDER_USE_SHADING_NODES_CUSTOM 64
#define RENER_USE_SPHERICAL_STEREO 128
#define RENDER_USE_STEREO_VIEWPORT 256
#define RENDER_USE_GPU_CONTEXT 512
#define RENDER_USE_CUSTOM_FREESTYLE 1024
#define RENDER_USE_NO_IMAGE_SAVE 2048
#define RENDER_USE_ALEMBIC_PROCEDURAL 4096

/* RenderEngine.flag */
#define RENDER_ENGINE_ANIMATION 1
#define RENDER_ENGINE_PREVIEW 2
#define RENDER_ENGINE_DO_DRAW 4
#define RENDER_ENGINE_DO_UPDATE 8
#define RENDER_ENGINE_RENDERING 16
#define RENDER_ENGINE_HIGHLIGHT_TILES 32
#define RENDER_ENGINE_CAN_DRAW 64

extern ListBase R_engines;

typedef struct RenderEngineType {
  struct RenderEngineType *next, *prev;

  /* type info */
  char idname[64]; /* best keep the same size as BKE_ST_MAXNAME. */
  char name[64];
  int flag;

  void (*update)(struct RenderEngine *engine, struct Main *bmain, struct Depsgraph *depsgraph);

  void (*render)(struct RenderEngine *engine, struct Depsgraph *depsgraph);

  /* Offline rendering is finished - no more view layers will be rendered.
   *
   * All the pending data is to be communicated from the engine back to Blender. In a possibly
   * most memory-efficient manner (engine might free its database before making Blender to allocate
   * full-frame render result). */
  void (*render_frame_finish)(struct RenderEngine *engine);

  void (*draw)(struct RenderEngine *engine,
               const struct Ctx *ctx,
               struct Graph *graph);

  void (*bake)(struct RenderEngine *engine,
               struct Graph *graph,
               struct Object *object,
               int pass_type,
               int pass_filter,
               int width,
               int height);

  void (*view_update)(struct RenderEngine *engine,
                      const struct Ctx *ctx,
                      struct Graph *graph);
  void (*view_draw)(struct RenderEngine *engine,
                    const struct Ctx *ctx,
                    struct Graph *graph);

  void (*update_script_node)(struct RenderEngine *engine,
                             truct NodeTree *ntree,
                             struct Node *node);
  void (*update_render_passes)(struct RenderEngine *engine,
                               struct Scene *scene,
                               struct ViewLayer *view_layer);

  struct DrawEngineType *draw_engine;

  /* Api integration */
  ExtensionApi api_ext;
} RenderEngineType;

typedef void (*update_render_passes_cb_t)(void *userdata,
                                          struct Scene *scene,
                                          struct ViewLayer *view_layer,
                                          const char *name,
                                          int channels,
                                          const char *chanid,
                                          eNodeSocketDatatype type);

typedef struct RenderEngine {
  RenderEngineType *type;
  void *py_instance;

  int flag;
  struct Object *camera_override;
  unsigned int layer_override;

  struct Render *re;
  ListBase fullresult;
  char text[512]; /* IMA_MAX_RENDER_TEXT */

  int resolution_x, resolution_y;

  struct ReportList *reports;

  struct {
    const struct BakePixel *pixels;
    float *result;
    int width, height, depth;
    int object_id;
  } bake;

  /* Depsgraph */
  struct Graph *graph;
  bool has_pen;

  /* callback for render pass query */
  ThreadMutex update_render_passes_mutex;
  update_render_passes_cb_t update_render_passes_cb;
  void *update_render_passes_data;

  rctf last_viewplane;
  rcti last_disprect;
  float last_viewmat[4][4];
  int last_winx, last_winy;
} RenderEngine;

RenderEngine *render_engine_create(RenderEngineType *type);
void render_engine_free(RenderEngine *engine);

/**
 * Loads in image into a result, size must match
 * x/y offsets are only used on a partial copy when dimensions don't match.
 */
void render_layer_load_from_file(
    struct RenderLayer *layer, struct ReportList *reports, const char *filename, int x, int y);
void renderm_result_load_from_file(struct RenderResult *result,
                              struct ReportList *reports,
                              const char *filename);

struct RenderResult *render_engine_begin_result(
    RenderEngine *engine, int x, int y, int w, int h, const char *layername, const char *viewname);
void render_engine_update_result(RenderEngine *engine, struct RenderResult *result);
void render_engine_add_pass(RenderEngine *engine,
                            const char *name,
                            int channels,
                            const char *chan_id,
                            const char *layername);
void render_engine_end_result(RenderEngine *engine,
                              struct RenderResult *result,
                              bool cancel,
                              bool highlight,
                              bool merge_results);
struct RenderResult *render_engine_get_result(struct RenderEngine *engine);

struct RenderPass *render_engine_pass_by_index_get(struct RenderEngine *engine,
                                               const char *layer_name,
                                               int index);

const char *render_engine_active_view_get(RenderEngine *engine);
void render_engine_active_view_set(RenderEngine *engine, const char *viewname);
float render_engine_get_camera_shift_x(RenderEngine *engine,
                                   struct Object *camera,
                                   bool use_spherical_stereo);
void render_engine_get_camera_model_matrix(RenderEngine *engine,
                                           struct Object *camera,
                                           bool use_spherical_stereo,
                                           float r_modelmat[16]);
bool render_engine_get_spherical_stereo(RenderEngine *engine, struct Object *camera);

bool render_engine_test_break(RenderEngine *engine);
void render_engine_update_stats(RenderEngine *engine, const char *stats, const char *info);
void render_engine_update_progress(RenderEngine *engine, float progress);
void render_engine_update_memory_stats(RenderEngine *engine, float mem_used, float mem_peak);
void render_engine_report(RenderEngine *engine, int type, const char *msg);
void RE_engine_set_error_message(RenderEngine *engine, const char *msg);

bool render_engine_render(struct Render *re, bool do_all);

bool render_engine_is_external(const struct Render *re);

void render_engine_frame_set(struct RenderEngine *engine, int frame, float subframe);

void render_engine_update_render_passes(struct RenderEngine *engine,
                                    struct Scene *scene,
                                    struct ViewLayer *view_layer,
                                    update_render_passes_cb_t callback,
                                    void *callback_data);
void RE_engine_register_pass(struct RenderEngine *engine,
                             struct Scene *scene,
                             struct ViewLayer *view_layer,
                             const char *name,
                             int channels,
                             const char *chanid,
                             eNodeSocketDatatype type);

bool RE_engine_use_persistent_data(struct RenderEngine *engine);

struct RenderEngine *RE_engine_get(const struct Render *re);

/* Acquire render engine for drawing via its `draw()` callback.
 *
 * If drawing is not possible false is returned. If drawing is possible then the engine is
 * "acquired" so that it can not be freed by the render pipeline.
 *
 * Drawing is possible if the engine has the `draw()` callback and it is in its `render()`
 * callback. */
bool RE_engine_draw_acquire(struct Render *re);
void RE_engine_draw_release(struct Render *re);

/* NOTE: Only used for Cycles's BLenderGPUDisplay integration with the draw manager. A subject
 * for re-consideration. Do not use this functionality. */
bool RE_engine_has_render_context(struct RenderEngine *engine);
void RE_engine_render_context_enable(struct RenderEngine *engine);
void RE_engine_render_context_disable(struct RenderEngine *engine);

/* Engine Types */

void RE_engines_init(void);
void RE_engines_init_experimental(void);
void RE_engines_exit(void);
void RE_engines_register(RenderEngineType *render_type);

bool RE_engine_is_opengl(RenderEngineType *render_type);

/**
 * Return true if the RenderEngineType has native support for direct loading of Alembic data. For
 * Cycles, this also checks that the experimental feature set is enabled.
 */
bool RE_engine_supports_alembic_procedural(const RenderEngineType *render_type, Scene *scene);

RenderEngineType *RE_engines_find(const char *idname);

rcti *RE_engine_get_current_tiles(struct Render *re, int *r_total_tiles, bool *r_needs_free);
struct RenderData *RE_engine_get_render_data(struct Render *re);
void RE_bake_engine_set_engine_parameters(struct Render *re,
                                          struct Main *bmain,
                                          struct Scene *scene);

void RE_engine_free_blender_memory(struct RenderEngine *engine);

void RE_engine_tile_highlight_set(
    struct RenderEngine *engine, int x, int y, int width, int height, bool highlight);
void RE_engine_tile_highlight_clear_all(struct RenderEngine *engine);

#ifdef __cplusplus
}
#endif
