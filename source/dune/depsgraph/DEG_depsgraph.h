/**
 * Public API for Depsgraph
 * ================
 *
 * The dependency graph tracks relations between various pieces of data in
 * a dune file, but mainly just those which make up scene data. It is used
 * to determine the set of operations need to ensure that all data has been
 * correctly evaluated in response to changes, based on dependencies and visibility
 * of affected data.
 * Evaluation Engine
 * =================
 *
 * The evaluation takes the operation-nodes the Depsgraph has tagged for updating,
 * and schedules them up for being evaluated/executed such that the all dependency
 * relationship constraints are satisfied.
 */

/* ************************************************* */
/* Forward-defined typedefs for core types
 * - These are used in all depsgraph code and by all callers of Depsgraph API...
 */

#pragma once

#include "DNA_ID.h"

/* Dependency Graph */
typedef struct Depsgraph Depsgraph;

/* ------------------------------------------------ */

struct Main;

struct Scene;
struct ViewLayer;

typedef enum eEvaluationMode {
  DAG_EVAL_VIEWPORT = 0, /* evaluate for OpenGL viewport */
  DAG_EVAL_RENDER = 1,   /* evaluate for render purposes */
} eEvaluationMode;

/* DagNode->eval_flags */
enum {
  /* Regardless to curve->path animation flag path is to be evaluated anyway,
   * to meet dependencies with such a things as curve modifier and other guys
   * who're using curve deform, where_on_path and so. */
  DAG_EVAL_NEED_CURVE_PATH = (1 << 0),
  /* A shrinkwrap modifier or constraint targeting this mesh needs information
   * about non-manifold boundary edges for the Target Normal Project mode. */
  DAG_EVAL_NEED_SHRINKWRAP_BOUNDARY = (1 << 1),
};

#ifdef __cplusplus
extern "C" {
#endif

/* ************************************************ */
/* Depsgraph API */

/* -------------------------------------------------------------------- */
/** CRUD **/

/* Get main depsgraph instance from context! */

/**
 * Create new Depsgraph instance.
 *
 * TODO: what arguments are needed here? What's the building-graph entry point?
 */
Depsgraph *deg_graph_new(struct Main *dmain,
                         struct Scene *scene,
                         struct ViewLayer *view_layer,
                         eEvaluationMode mode);

/**
 * Replace the "owner" pointers (currently Main/Scene/ViewLayer) of this depsgraph.
 * Used for:
 * - Undo steps when we do want to re-use the old depsgraph data as much as possible.
 * - Rendering where we want to re-use objects between different view layers.
 */
void deg_graph_replace_owners(struct Depsgraph *depsgraph,
                              struct Main *bmain,
                              struct Scene *scene,
                              struct ViewLayer *view_layer);

/** Free graph's contents and graph itself. */
void deg_graph_free(Depsgraph *graph);

/* -------------------------------------------------------------------- */
/** Node Types Registry **/

/** Register all node types. */
void deg_register_node_types(void);

/** Free node type registry on exit. */
void deg_free_node_types(void);

/* -------------------------------------------------------------------- */
/** Update Tagging **/

/** Tag dependency graph for updates when visible scenes/layers changes. */
void deg_graph_tag_on_visible_update(Depsgraph *depsgraph, bool do_time);

/** Tag all dependency graphs for update when visible scenes/layers changes. */
void deg_tag_on_visible_update(struct Main *bmain, bool do_time);

/**
 * note Will return NULL if the flag is not known, allowing to gracefully handle situations
 * when recalc flag has been removed.
 */
const char *deg_update_tag_as_string(IDRecalcFlag flag);

/** Tag given ID for an update in all the dependency graphs. */
void deg_id_tag_update(struct ID *id, int flag);
void deg_id_tag_update_ex(struct Main *dmain, struct ID *id, int flag);

void deg_graph_id_tag_update(struct Main *dmain,
                             struct Depsgraph *depsgraph,
                             struct ID *id,
                             int flag);

/** Tag all dependency graphs when time has changed. */
void deg_time_tag_update(struct Main *bmain);

/** Tag a dependency graph when time has changed. */
void deg_graph_time_tag_update(struct Depsgraph *depsgraph);

/**
 * Mark a particular data-block type as having changing.
 * This does not cause any updates but is used by external
 * render engines to detect if for example a data-block was removed.
 */
void deg_graph_id_type_tag(struct Depsgraph *depsgraph, short id_type);
void deg_id_type_tag(struct Main *bmain, short id_type);

/**
 * Set a depsgraph to flush updates to editors. This would be done
 * for viewport depsgraphs, but not render or export depsgraph for example.
 */
void deg_enable_editors_update(struct Depsgraph *depsgraph);

/** Check if something was changed in the database and inform editors about this. */
void deg_editors_update(struct Depsgraph *depsgraph, bool time);

/** Clear recalc flags after editors or renderers have handled updates. */
void deg_ids_clear_recalc(Depsgraph *depsgraph, bool backup);

/**
 * Restore recalc flags, backed up by a previous call to #DEG_ids_clear_recalc.
 * This also clears the backup.
 */
void deg_ids_restore_recalc(Depsgraph *depsgraph);

/* ************************************************ */
/* Evaluation Engine API */

/* -------------------------------------------------------------------- */
/** Graph Evaluation **/

/**
 * Frame changed recalculation entry point.
 *
 * The frame-change happened for root scene that graph belongs to.
 */
void deg_evaluate_on_framechange(Depsgraph *graph, float frame);

/**
 * Data changed recalculation entry point.
 * Evaluate all nodes tagged for updating.
 */
void deg_evaluate_on_refresh(Depsgraph *graph);

/* -------------------------------------------------------------------- */
/** Editors Integration
 *
 * Mechanism to allow editors to be informed of depsgraph updates,
 * to do their own updates based on changes.
 **/

typedef struct DEGEditorUpdateContext {
  struct Main *bmain;
  struct Depsgraph *depsgraph;
  struct Scene *scene;
  struct ViewLayer *view_layer;
} DEGEditorUpdateContext;

typedef void (*DEG_EditorUpdateIDCb)(const DEGEditorUpdateContext *update_ctx, struct ID *id);
typedef void (*DEG_EditorUpdateSceneCb)(const DEGEditorUpdateContext *update_ctx, bool updated);

/** Set callbacks which are being called when depsgraph changes. */
void deg_editors_set_update_cb(DEG_EditorUpdateIDCb id_func, DEG_EditorUpdateSceneCb scene_func);

/* -------------------------------------------------------------------- */
/** Evaluation */

bool deg_is_evaluating(const struct Depsgraph *depsgraph);

bool deg_is_active(const struct Depsgraph *depsgraph);
void deg_make_active(struct Depsgraph *depsgraph);
void deg_make_inactive(struct Depsgraph *depsgraph);

/* -------------------------------------------------------------------- */
/** Evaluation Debug **/

void deg_debug_print_begin(struct Depsgraph *depsgraph);

void deg_debug_print_eval(struct Depsgraph *depsgraph,
                          const char *function_name,
                          const char *object_name,
                          const void *object_address);

void deg_debug_print_eval_subdata(struct Depsgraph *depsgraph,
                                  const char *function_name,
                                  const char *object_name,
                                  const void *object_address,
                                  const char *subdata_comment,
                                  const char *subdata_name,
                                  const void *subdata_address);

void deg_debug_print_eval_subdata_index(struct Depsgraph *depsgraph,
                                        const char *function_name,
                                        const char *object_name,
                                        const void *object_address,
                                        const char *subdata_comment,
                                        const char *subdata_name,
                                        const void *subdata_address,
                                        int subdata_index);

void deg_debug_print_eval_parent_typed(struct Depsgraph *depsgraph,
                                       const char *function_name,
                                       const char *object_name,
                                       const void *object_address,
                                       const char *parent_comment,
                                       const char *parent_name,
                                       const void *parent_address);

void deg_debug_print_eval_time(struct Depsgraph *depsgraph,
                               const char *function_name,
                               const char *object_name,
                               const void *object_address,
                               float time);


#ifdef __cplusplus
} /* extern "C" */
#endif
