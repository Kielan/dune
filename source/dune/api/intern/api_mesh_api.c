#include <stdio.h>
#include <stdlib.h>

#include "api_define.h"

#include "types_customdata.h"

#include "lib_sys_types.h"

#include "lib_utildefines.h"

#include "api_internal.h" /* own include */

#ifdef API_RUNTIME

#  include "types_mesh.h"

#  include "dune_mesh.h"
#  include "dune_mesh_mapping.h"
#  include "dune_mesh_runtime.h"
#  include "dune_mesh_tangent.h"
#  include "ed_mesh.h"

static const char *api_Mesh_unit_test_compare(struct Mesh *mesh,
                                              struct Mesh *mesh2,
                                              float threshold)
{
  const char *ret = dune_mesh_cmp(mesh, mesh2, threshold);

  if (!ret) {
    ret = "Same";
  }

  return ret;
}

static void api_Mesh_create_normals_split(Mesh *mesh)
{
  if (!CustomData_has_layer(&mesh->ldata, CD_NORMAL)) {
    CustomData_add_layer(&mesh->ldata, CD_NORMAL, CD_CALLOC, NULL, mesh->totloop);
    CustomData_set_layer_flag(&mesh->ldata, CD_NORMAL, CD_FLAG_TEMPORARY);
  }
}

static void api_Mesh_free_normals_split(Mesh *mesh)
{
  CustomData_free_layers(&mesh->ldata, CD_NORMAL, mesh->totloop);
}

static void api_Mesh_calc_tangents(Mesh *mesh, ReportList *reports, const char *uvmap)
{
  float(*r_looptangents)[4];

  if (CustomData_has_layer(&mesh->ldata, CD_MLOOPTANGENT)) {
    r_looptangents = CustomData_get_layer(&mesh->ldata, CD_MLOOPTANGENT);
    memset(r_looptangents, 0, sizeof(float[4]) * mesh->totloop);
  }
  else {
    r_looptangents = CustomData_add_layer(
        &mesh->ldata, CD_MLOOPTANGENT, CD_CALLOC, NULL, mesh->totloop);
    CustomData_set_layer_flag(&mesh->ldata, CD_MLOOPTANGENT, CD_FLAG_TEMPORARY);
  }

  /* Compute loop normals if needed. */
  if (!CustomData_has_layer(&mesh->ldata, CD_NORMAL)) {
    dune_mesh_calc_normals_split(mesh);
  }

  dune_mesh_calc_loop_tangent_single(mesh, uvmap, r_looptangents, reports);
}

static void api_Mesh_free_tangents(Mesh *mesh)
{
  CustomData_free_layers(&mesh->ldata, CD_MLOOPTANGENT, mesh->totloop);
}

static void api_Mesh_calc_looptri(Mesh *mesh)
{
  dune_mesh_runtime_looptri_ensure(mesh);
}

static void api_Mesh_calc_smooth_groups(
    Mesh *mesh, bool use_bitflags, int *r_poly_group_len, int **r_poly_group, int *r_group_total)
{
  *r_poly_group_len = mesh->totpoly;
  *r_poly_group = dune_mesh_calc_smoothgroups(mesh->medge,
                                             mesh->totedge,
                                             mesh->mpoly,
                                             mesh->totpoly,
                                             mesh->mloop,
                                             mesh->totloop,
                                             r_group_total,
                                             use_bitflags);
}

static void api_Mesh_normals_split_custom_do(Mesh *mesh,
                                             float (*custom_loopnors)[3],
                                             const bool use_vertices)
{
  if (use_vertices) {
    dune_mesh_set_custom_normals_from_vertices(mesh, custom_loopnors);
  } else {
    dune_mesh_set_custom_normals(mesh, custom_loopnors);
  }
}

static void api_Mesh_normals_split_custom_set(Mesh *mesh,
                                              ReportList *reports,
                                              int normals_len,
                                              float *normals)
{
  float(*loopnors)[3] = (float(*)[3])normals;
  const int numloops = mesh->totloop;

  if (normals_len != numloops * 3) {
    dune_reportf(reports,
                RPT_ERROR,
                "Number of custom normals is not number of loops (%f / %d)",
                (float)normals_len / 3.0f,
                numloops);
    return;
  }

  api_Mesh_normals_split_custom_do(mesh, loopnors, false);

  graph_id_tag_update(&mesh->id, 0);
}

static void api_Mesh_normals_split_custom_set_from_vertices(Mesh *mesh,
                                                            ReportList *reports,
                                                            int normals_len,
                                                            float *normals)
{
  float(*vertnors)[3] = (float(*)[3])normals;
  const int numverts = mesh->totvert;

  if (normals_len != numverts * 3) {
    dune_reportf(reports,
                RPT_ERROR,
                "Number of custom normals is not number of vertices (%f / %d)",
                (float)normals_len / 3.0f,
                numverts);
    return;
  }

  api_Mesh_normals_split_custom_do(mesh, vertnors, true);

  graph_id_tag_update(&mesh->id, 0);
}

static void api_Mesh_transform(Mesh *mesh, float mat[16], bool shape_keys)
{
  dune_mesh_transform(mesh, (float(*)[4])mat, shape_keys);

  graph_id_tag_update(&mesh->id, 0);
}

static void rna_Mesh_flip_normals(Mesh *mesh)
{
  BKE_mesh_polygons_flip(mesh->mpoly, mesh->mloop, &mesh->ldata, mesh->totpoly);
  BKE_mesh_tessface_clear(mesh);
  BKE_mesh_normals_tag_dirty(mesh);
  BKE_mesh_runtime_clear_geometry(mesh);

  DEG_id_tag_update(&mesh->id, 0);
}

static void rna_Mesh_split_faces(Mesh *mesh, bool free_loop_normals)
{
  BKE_mesh_split_faces(mesh, free_loop_normals != 0);
}

static void rna_Mesh_update_gpu_tag(Mesh *mesh)
{
  BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);
}

static void rna_Mesh_count_selected_items(Mesh *mesh, int r_count[3])
{
  BKE_mesh_count_selected_items(mesh, r_count);
}

static void rna_Mesh_clear_geometry(Mesh *mesh)
{
  BKE_mesh_clear_geometry(mesh);

  DEG_id_tag_update(&mesh->id, ID_RECALC_GEOMETRY_ALL_MODES);
  WM_main_add_notifier(NC_GEOM | ND_DATA, mesh);
}

#else

void RNA_api_mesh(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;
  const int normals_array_dim[] = {1, 3};

  func = RNA_def_function(srna, "transform", "rna_Mesh_transform");
  RNA_def_function_ui_description(func,
                                  "Transform mesh vertices by a matrix "
                                  "(Warning: inverts normals if matrix is negative)");
  parm = RNA_def_float_matrix(func, "matrix", 4, 4, NULL, 0.0f, 0.0f, "", "Matrix", 0.0f, 0.0f);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_boolean(func, "shape_keys", 0, "", "Transform Shape Keys");

  func = RNA_def_function(srna, "flip_normals", "rna_Mesh_flip_normals");
  RNA_def_function_ui_description(func,
                                  "Invert winding of all polygons "
                                  "(clears tessellation, does not handle custom normals)");

  func = RNA_def_function(srna, "calc_normals", "BKE_mesh_calc_normals");
  RNA_def_function_ui_description(func, "Calculate vertex normals");

  func = RNA_def_function(srna, "create_normals_split", "rna_Mesh_create_normals_split");
  RNA_def_function_ui_description(func, "Empty split vertex normals");

  func = RNA_def_function(srna, "calc_normals_split", "BKE_mesh_calc_normals_split");
  RNA_def_function_ui_description(func,
                                  "Calculate split vertex normals, which preserve sharp edges");

  func = RNA_def_function(srna, "free_normals_split", "rna_Mesh_free_normals_split");
  RNA_def_function_ui_description(func, "Free split vertex normals");

  func = RNA_def_function(srna, "split_faces", "rna_Mesh_split_faces");
  RNA_def_function_ui_description(func, "Split faces based on the edge angle");
  RNA_def_boolean(
      func, "free_loop_normals", 1, "Free Loop Normals", "Free loop normals custom data layer");

  func = RNA_def_function(srna, "calc_tangents", "rna_Mesh_calc_tangents");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_function_ui_description(
      func,
      "Compute tangents and bitangent signs, to be used together with the split normals "
      "to get a complete tangent space for normal mapping "
      "(split normals are also computed if not yet present)");
  parm = RNA_def_string(func,
                        "uvmap",
                        NULL,
                        MAX_CUSTOMDATA_LAYER_NAME,
                        "",
                        "Name of the UV map to use for tangent space computation");

  func = RNA_def_function(srna, "free_tangents", "rna_Mesh_free_tangents");
  RNA_def_function_ui_description(func, "Free tangents");

  func = RNA_def_function(srna, "calc_loop_triangles", "rna_Mesh_calc_looptri");
  RNA_def_function_ui_description(func,
                                  "Calculate loop triangle tessellation (supports editmode too)");

  func = RNA_def_function(srna, "calc_smooth_groups", "rna_Mesh_calc_smooth_groups");
  RNA_def_function_ui_description(func, "Calculate smooth groups from sharp edges");
  RNA_def_boolean(
      func, "use_bitflags", false, "", "Produce bitflags groups instead of simple numeric values");
  /* return values */
  parm = RNA_def_int_array(func, "poly_groups", 1, NULL, 0, 0, "", "Smooth Groups", 0, 0);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_OUTPUT);
  parm = RNA_def_int(
      func, "groups", 0, 0, INT_MAX, "groups", "Total number of groups", 0, INT_MAX);
  RNA_def_parameter_flags(parm, 0, PARM_OUTPUT);

  func = RNA_def_function(srna, "normals_split_custom_set", "rna_Mesh_normals_split_custom_set");
  RNA_def_function_ui_description(func,
                                  "Define custom split normals of this mesh "
                                  "(use zero-vectors to keep auto ones)");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  /* TODO: see how array size of 0 works, this shouldn't be used. */
  parm = RNA_def_float_array(func, "normals", 1, NULL, -1.0f, 1.0f, "", "Normals", 0.0f, 0.0f);
  RNA_def_property_multi_array(parm, 2, normals_array_dim);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_REQUIRED);

  func = RNA_def_function(srna,
                          "normals_split_custom_set_from_vertices",
                          "rna_Mesh_normals_split_custom_set_from_vertices");
  RNA_def_function_ui_description(
      func,
      "Define custom split normals of this mesh, from vertices' normals "
      "(use zero-vectors to keep auto ones)");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  /* TODO: see how array size of 0 works, this shouldn't be used. */
  parm = RNA_def_float_array(func, "normals", 1, NULL, -1.0f, 1.0f, "", "Normals", 0.0f, 0.0f);
  RNA_def_property_multi_array(parm, 2, normals_array_dim);
  RNA_def_parameter_flags(parm, PROP_DYNAMIC, PARM_REQUIRED);

  func = RNA_def_function(srna, "update", "ED_mesh_update");
  RNA_def_boolean(func, "calc_edges", 0, "Calculate Edges", "Force recalculation of edges");
  RNA_def_boolean(func,
                  "calc_edges_loose",
                  0,
                  "Calculate Loose Edges",
                  "Calculate the loose state of each edge");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);

  RNA_def_function(srna, "update_gpu_tag", "rna_Mesh_update_gpu_tag");

  func = RNA_def_function(srna, "unit_test_compare", "rna_Mesh_unit_test_compare");
  RNA_def_pointer(func, "mesh", "Mesh", "", "Mesh to compare to");
  RNA_def_float_factor(func,
                       "threshold",
                       FLT_EPSILON * 60,
                       0.0f,
                       FLT_MAX,
                       "Threshold",
                       "Comparison tolerance threshold",
                       0.0f,
                       FLT_MAX);
  /* return value */
  parm = RNA_def_string(
      func, "result", "nothing", 64, "Return value", "String description of result of comparison");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "clear_geometry", "rna_Mesh_clear_geometry");
  RNA_def_function_ui_description(
      func,
      "Remove all geometry from the mesh. Note that this does not free shape keys or materials");

  func = apo_def_fn(sapi, "validate", "BKE_mesh_validate");
  RNA_def_fn_ui_description(func,
                                  "Validate geometry, return True when the mesh has had "
                                  "invalid geometry corrected/removed");
  api_def_bool(fn, "verbose", false, "Verbose", "Output information about the errors found");
  api_def_bool(fn, "clean_customdata",
               true,
               "Clean Custom Data",
               "Remove temp/cached custom-data layers, like e.g. normals...");
  parm = api_def_bool(fn, "result", 0, "Result", "");
  api_def_fn_return(fn, parm);

  fn = api_def_function(srna, "validate_material_indices", "BKE_mesh_validate_material_indices");
  api_def_function_ui_description(
      func,
      "Validate material indices of polygons, return True when the mesh has had "
      "invalid indices corrected (to default 0)");
  parm = api_def_bool(fn, "result", 0, "Result", "");
  api_def_fn_return(func, parm);

  fn = api_def_fn(sapi, "count_selected_items", "rna_Mesh_count_selected_items ");
  api_def_fn_ui_description(func, "Return the number of selected items (vert, edge, face)");
  parm = RNA_def_int_vector(func, "result", 3, NULL, 0, INT_MAX, "Result", NULL, 0, INT_MAX);
  RNA_def_function_output(func, parm);
}

#endif
