#include "api_define.h"
#include "api_enum_types.h"
#include "api_types.h"

#include "dune_workspace.h"

#include "ed_render.h"

#include "render_engine.h"

#include "wm_api.h"
#include "wm_types.h"

#include "api_internal.h"

#include "types_workspace.h"

#ifdef API_RUNTIME

#  include "lib_list.h"

#  include "dune_global.h"

#  include "types_object.h"
#  include "types_screen.h"
#  include "types_space.h"

#  include "ed_asset.h"
#  include "ed_paint.h"

#  include "api_access.h"

#  include "wm_toolsystem.h"

static void api_window_update_all(Main *UNUSED(main),
                                  Scene *UNUSED(scene),
                                  ApiPtr *UNUSED(ptr))
{
  wm_main_add_notifier(NC_WINDOW, NULL);
}

void api_workspace_screens_begin(CollectionPropIter *iter, ApiPtr *ptr)
{
  WorkSpace *workspace = (WorkSpace *)ptr->owner_id;
  api_iter_list_begin(iter, &workspace->layouts, NULL);
}

static ApiPtr api_workspace_screens_item_get(CollectionPropertyIterator *iter)
{
  WorkSpaceLayout *layout = rna_iterator_listbase_get(iter);
  Screen *screen = dune_workspace_layout_screen_get(layout);

  return api_ptr_inherit_refine(&iter->parent, &ApiScreen, screen);
}

/* workspace.owner_ids */

static wmOwnerID *api_WorkSpace_owner_ids_new(WorkSpace *workspace, const char *name)
{
  wmOwnerID *owner_id = mem_callocn(sizeof(*owner_id), __func__);
  lib_addtail(&workspace->owner_ids, owner_id);
  STRNCPY(owner_id->name, name);
  wm_main_add_notifier(NC_WINDOW, NULL);
  return owner_id;
}

static void api_WorkSpace_owner_ids_remove(WorkSpace *workspace,
                                           ReportList *reports,
                                           ApiPtr *wstag_ptr)
{
  wmOwnerId *owner_id = wstag_ptr->data;
  if (lib_remlink_safe(&workspace->owner_ids, owner_id) == false) {
    dune_reportf(reports,
                RPT_ERROR,
                "wmOwnerID '%s' not in workspace '%s'",
                owner_id->name,
                workspace->id.name + 2);
    return;
  }

  mem_freen(owner_id);
  API_PTR_INVALIDATE(wstag_ptr);

  wm_main_add_notifier(NC_WINDOW, NULL);
}

static void api_WorkSpace_owner_ids_clear(WorkSpace *workspace)
{
  lib_freelistn(&workspace->owner_ids);
  wm_main_add_notifier(NC_OBJECT | ND_MODIFIER | NA_REMOVED, workspace);
}

static int api_WorkSpace_asset_library_get(ApiPtr *ptr)
{
  const WorkSpace *workspace = ptr->data;
  return ed_asset_lib_ref_to_enum_value(&workspace->asset_lib_ref);
}

static void rna_WorkSpace_asset_library_set(ApiPtr *ptr, int value)
{
  WorkSpace *workspace = ptr->data;
  workspace->asset_lib_ref = ED_asset_libr_ref_from_enum_value(value);
}

static ToolRef *api_WorkSpace_tools_from_tkey(WorkSpace *workspace,
                                               const ToolKey *tkey,
                                               bool create)
{
  if (create) {
    ToolRef *tref;
    wm_toolsystem_ref_ensure(workspace, tkey, &tref);
    return tref;
  }
  return wm_toolsystem_ref_find(workspace, tkey);
}

static ToolRef *api_WorkSpace_tools_from_space_view3d_mode(WorkSpace *workspace,
                                                            int mode,
                                                            bool create)
{
  return api_WorkSpace_tools_from_tkey(workspace,
                                       &(ToolKey){
                                           .space_type = SPACE_VIEW3D,
                                           .mode = mode,
                                       },
                                       create);
}

static ToolRef *api_WorkSpace_tools_from_space_image_mode(WorkSpace *workspace,
                                                           int mode,
                                                           bool create)
{
  return api_WorkSpace_tools_from_tkey(workspace,
                                       &(bToolKey){
                                           .space_type = SPACE_IMAGE,
                                           .mode = mode,
                                       },
                                       create);
}

static ToolRef *api_WorkSpace_tools_from_space_node(WorkSpace *workspace, bool create)
{
  return api_WorkSpace_tools_from_tkey(workspace,
                                       &(bToolKey){
                                           .space_type = SPACE_NODE,
                                           .mode = 0,
                                       },
                                       create);
}
static bToolRef *api_WorkSpace_tools_from_space_sequencer(WorkSpace *workspace,
                                                          int mode,
                                                          bool create)
{
  return api_WorkSpace_tools_from_tkey(workspace,
                                       &(bToolKey){
                                           .space_type = SPACE_SEQ,
                                           .mode = mode,
                                       },
                                       create);
}
const EnumPropertyItem *rna_WorkSpace_tools_mode_itemf(bContext *UNUSED(C),
                                                       PointerRNA *ptr,
                                                       PropertyRNA *UNUSED(prop),
                                                       bool *UNUSED(r_free))
{
  bToolRef *tref = ptr->data;
  switch (tref->space_type) {
    case SPACE_VIEW3D:
      return rna_enum_context_mode_items;
    case SPACE_IMAGE:
      return rna_enum_space_image_mode_all_items;
    case SPACE_SEQ:
      return rna_enum_space_sequencer_view_type_items;
  }
  return DummyRNA_DEFAULT_items;
}

static bool rna_WorkSpaceTool_use_paint_canvas_get(PointerRNA *ptr)
{
  bToolRef *tref = ptr->data;
  return ED_paint_tool_use_canvas(NULL, tref);
}

static int rna_WorkSpaceTool_index_get(PointerRNA *ptr)
{
  bToolRef *tref = ptr->data;
  return (tref->runtime) ? tref->runtime->index : 0;
}

static bool rna_WorkSpaceTool_has_datablock_get(PointerRNA *ptr)
{
  bToolRef *tref = ptr->data;
  return (tref->runtime) ? (tref->runtime->data_block[0] != '\0') : false;
}

static void rna_WorkSpaceTool_widget_get(PointerRNA *ptr, char *value)
{
  bToolRef *tref = ptr->data;
  strcpy(value, tref->runtime ? tref->runtime->gizmo_group : "");
}

static int rna_WorkSpaceTool_widget_length(PointerRNA *ptr)
{
  bToolRef *tref = ptr->data;
  return tref->runtime ? strlen(tref->runtime->gizmo_group) : 0;
}

#else /* RNA_RUNTIME */

static void rna_def_workspace_owner(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "wmOwnerID", NULL);
  RNA_def_struct_sdna(srna, "wmOwnerID");
  RNA_def_struct_clear_flag(srna, STRUCT_UNDO);
  RNA_def_struct_ui_text(srna, "Work Space UI Tag", "");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_ui_text(prop, "Name", "");
  RNA_def_struct_name_property(srna, prop);
}

static void rna_def_workspace_owner_ids(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "wmOwnerIDs");
  srna = RNA_def_struct(brna, "wmOwnerIDs", NULL);
  RNA_def_struct_sdna(srna, "WorkSpace");
  RNA_def_struct_ui_text(srna, "WorkSpace UI Tags", "");

  /* add owner_id */
  fn = api_def_fn(sapi, "new", "api_WorkSpace_owner_ids_new");
  api_def_fn_ui_description(fn, "Add ui tag");
  parm = api_def_string(fn, "name", "Name", 0, "", "New name for the tag");
  api_def_param_flags(parm, 0, PARM_REQUIRED);
  /* return type */
  parm = api_def_ptr(fn, "owner_id", "wmOwnerID", "", "");
  api_def_fn_return(fn, parm);

  /* remove owner_id */
  fn = api_def_fn(sapi, "remove", "api_WorkSpace_owner_ids_remove");
  api_def_fn_flag(fn, FN_USE_REPORTS);
  api_def_fn_ui_description(fn, "Remove ui tag");
  /* owner_id to remove */
  parm = api_def_ptr(fn, "owner_id", "wmOwnerID", "", "Tag to remove");
  api_def_param_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  api_def_param_clear_flags(parm, PROP_THICK_WRAP, 0);

  /* clear all modifiers */
  func = RNA_def_function(srna, "clear", "rna_WorkSpace_owner_ids_clear");
  RNA_def_function_ui_description(func, "Remove all tags");
}

static void api_def_workspace_tool(BlenderRNA *brna)
{
  ApiStruct *sapi;
  ApiProp *prop;

  sapi = api_def_struct(sapi, "WorkSpaceTool", NULL);
  api_def_struct_stype(sapi, "bToolRef");m
  api_def_struct_clear_flag(sapi, STRUCT_UNDO);
  api_def_struct_ui_text(sapi, "Work Space Tool", "");

  prop = api_def_prop(sapi, "idname", PROP_STRING, PROP_NONE);
  api_def_property_ui_text(prop, "Identifier", "");
  api_def_struct_name_prop(sapi, prop);

  prop = api_def_prop(sapi, "idname_fallback", PROP_STRING, PROP_NONE);
  api_def_prop_ui_text(prop, "Identifier Fallback", "");

  prop = api_def_prop(sapi, "index", PROP_INT, PROP_NONE);
  api_def_prop_clear_flag(prop, PROP_EDITABLE);
  api_def_prop_ui_text(prop, "Index", "");
  api_def_prop_int_fns(prop, "api_WorkSpaceTool_index_get", NULL, NULL);

  prop = api_def_prop(sapi, "space_type", PROP_ENUM, PROP_NONE);
  api_def_prop_enum_stype(prop, NULL, "space_type");
  api_def_prop_clear_flag(prop, PROP_EDITABLE);
  api_def_prop_enum_items(prop, api_enum_space_type_items);
  api_def_prop_ui_text(prop, "Space Type", "");

  prop = api_def_prop(sapi, "mode", PROP_ENUM, PROP_NONE);
  api_def_prop_enum_stype(prop, NULL, "mode");
  api_def_prop_enum_items(prop, DummyRNA_DEFAULT_items);
  api_def_prop_enum_fns(prop, NULL, NULL, "rna_WorkSpace_tools_mode_itemf");
  api_def_prop_ui_text(prop, "Tool Mode", "");
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  prop = RNA_def_property(srna, "use_paint_canvas", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Index", "");
  RNA_def_property_boolean_funcs(prop, "rna_WorkSpaceTool_use_paint_canvas_get", NULL);
  RNA_def_property_ui_text(prop, "Use Paint Canvas", "Does this tool use a painting canvas");

  RNA_define_verify_sdna(0);
  prop = RNA_def_property(srna, "has_datablock", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Has Data-Block", "");
  RNA_def_property_boolean_funcs(prop, "rna_WorkSpaceTool_has_datablock_get", NULL);
  RNA_define_verify_sdna(1);

  prop = RNA_def_property(srna, "widget", PROP_STRING, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_ui_text(prop, "Widget", "");
  RNA_def_property_string_funcs(
      prop, "rna_WorkSpaceTool_widget_get", "rna_WorkSpaceTool_widget_length", NULL);

  RNA_api_workspace_tool(srna);
}

static void rna_def_workspace_tools(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "wmTools");
  srna = RNA_def_struct(brna, "wmTools", NULL);
  RNA_def_struct_sdna(srna, "WorkSpace");
  RNA_def_struct_ui_text(srna, "WorkSpace UI Tags", "");

  /* add owner_id */
  func = RNA_def_function(
      srna, "from_space_view3d_mode", "rna_WorkSpace_tools_from_space_view3d_mode");
  RNA_def_function_ui_description(func, "");
  parm = RNA_def_enum(func, "mode", rna_enum_context_mode_items, 0, "", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_boolean(func, "create", false, "Create", "");
  /* return type */
  parm = RNA_def_pointer(func, "result", "WorkSpaceTool", "", "");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(
      srna, "from_space_image_mode", "rna_WorkSpace_tools_from_space_image_mode");
  RNA_def_function_ui_description(func, "");
  parm = RNA_def_enum(func, "mode", rna_enum_space_image_mode_all_items, 0, "", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_boolean(func, "create", false, "Create", "");
  /* return type */
  parm = RNA_def_pointer(func, "result", "WorkSpaceTool", "", "");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "from_space_node", "rna_WorkSpace_tools_from_space_node");
  RNA_def_function_ui_description(func, "");
  RNA_def_boolean(func, "create", false, "Create", "");
  /* return type */
  parm = RNA_def_pointer(func, "result", "WorkSpaceTool", "", "");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(
      srna, "from_space_sequencer", "rna_WorkSpace_tools_from_space_sequencer");
  RNA_def_function_ui_description(func, "");
  parm = RNA_def_enum(func, "mode", rna_enum_space_sequencer_view_type_items, 0, "", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_boolean(func, "create", false, "Create", "");
  /* return type */
  parm = RNA_def_pointer(func, "result", "WorkSpaceTool", "", "");
  RNA_def_function_return(func, parm);
}

static void rna_def_workspace(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "WorkSpace", "ID");
  RNA_def_struct_sdna(srna, "WorkSpace");
  RNA_def_struct_ui_text(
      srna, "Workspace", "Workspace data-block, defining the working environment for the user");
  /* TODO: real icon, just to show something */
  RNA_def_struct_ui_icon(srna, ICON_WORKSPACE);

  prop = RNA_def_property(srna, "screens", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "layouts", NULL);
  RNA_def_property_struct_type(prop, "Screen");
  RNA_def_property_collection_funcs(prop,
                                    "rna_workspace_screens_begin",
                                    NULL,
                                    NULL,
                                    "rna_workspace_screens_item_get",
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL);
  RNA_def_property_ui_text(prop, "Screens", "Screen layouts of a workspace");

  prop = RNA_def_property(srna, "owner_ids", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "wmOwnerID");
  RNA_def_property_ui_text(prop, "UI Tags", "");
  rna_def_workspace_owner_ids(brna, prop);

  prop = RNA_def_property(srna, "tools", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_sdna(prop, NULL, "tools", NULL);
  RNA_def_property_struct_type(prop, "WorkSpaceTool");
  RNA_def_property_ui_text(prop, "Tools", "");
  rna_def_workspace_tools(brna, prop);

  prop = RNA_def_property(srna, "object_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, rna_enum_workspace_object_mode_items);
  RNA_def_property_ui_text(
      prop, "Object Mode", "Switch to this object mode when activating the workspace");

  prop = RNA_def_property(srna, "use_pin_scene", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", WORKSPACE_USE_PIN_SCENE);
  RNA_def_property_ui_text(prop,
                           "Pin Scene",
                           "Remember the last used scene for the workspace and switch to it "
                           "whenever this workspace is activated again");
  RNA_def_property_update(prop, NC_WORKSPACE, NULL);

  /* Flags */
  prop = RNA_def_property(srna, "use_filter_by_owner", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_ANIMATABLE);
  RNA_def_property_boolean_sdna(prop, NULL, "flags", WORKSPACE_USE_FILTER_BY_ORIGIN);
  RNA_def_property_ui_text(prop, "Use UI Tags", "Filter the UI by tags");
  RNA_def_property_update(prop, 0, "rna_window_update_all");

  prop = rna_def_asset_library_reference_common(
      srna, "rna_WorkSpace_asset_library_get", "rna_WorkSpace_asset_library_set");
  RNA_def_property_ui_text(prop,
                           "Asset Library",
                           "Active asset library to show in the UI, not used by the Asset Browser "
                           "(which has its own active asset library)");
  RNA_def_property_update(prop, NC_ASSET | ND_ASSET_LIST_READING, NULL);

  RNA_api_workspace(srna);
}

void RNA_def_workspace(BlenderRNA *brna)
{
  rna_def_workspace_owner(brna);
  rna_def_workspace_tool(brna);

  rna_def_workspace(brna);
}

#endif /* RNA_RUNTIME */
