#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "lib_utildefines.h"

#include "api_define.h"
#include "api_enum_types.h"

#include "types_screen.h"
#include "types_space.h"
#include "types_windowmanager.h"

#include "ui.h"

#include "wm_cursors.h"
#include "wm_event_types.h"

#include "wm_api.h"
#include "wm_types.h"

#include "api_internal.h" /* own include */

/* confusing 2 enums mixed up here */
const EnumPropItem api_enum_window_cursor_items[] = {
    {WM_CURSOR_DEFAULT, "DEFAULT", 0, "Default", ""},
    {WM_CURSOR_NONE, "NONE", 0, "None", ""},
    {WM_CURSOR_WAIT, "WAIT", 0, "Wait", ""},
    {WM_CURSOR_EDIT, "CROSSHAIR", 0, "Crosshair", ""},
    {WM_CURSOR_X_MOVE, "MOVE_X", 0, "Move-X", ""},
    {WM_CURSOR_Y_MOVE, "MOVE_Y", 0, "Move-Y", ""},

    /* new */
    {WM_CURSOR_KNIFE, "KNIFE", 0, "Knife", ""},
    {WM_CURSOR_TEXT_EDIT, "TEXT", 0, "Text", ""},
    {WM_CURSOR_PAINT_BRUSH, "PAINT_BRUSH", 0, "Paint Brush", ""},
    {WM_CURSOR_PAINT, "PAINT_CROSS", 0, "Paint Cross", ""},
    {WM_CURSOR_DOT, "DOT", 0, "Dot Cursor", ""},
    {WM_CURSOR_ERASER, "ERASER", 0, "Eraser", ""},
    {WM_CURSOR_HAND, "HAND", 0, "Hand", ""},
    {WM_CURSOR_EW_SCROLL, "SCROLL_X", 0, "Scroll-X", ""},
    {WM_CURSOR_NS_SCROLL, "SCROLL_Y", 0, "Scroll-Y", ""},
    {WM_CURSOR_NSEW_SCROLL, "SCROLL_XY", 0, "Scroll-XY", ""},
    {WM_CURSOR_EYEDROPPER, "EYEDROPPER", 0, "Eyedropper", ""},
    {WM_CURSOR_PICK_AREA, "PICK_AREA", 0, "Pick Area", ""},
    {WM_CURSOR_STOP, "STOP", 0, "Stop", ""},
    {WM_CURSOR_COPY, "COPY", 0, "Copy", ""},
    {WM_CURSOR_CROSS, "CROSS", 0, "Cross", ""},
    {WM_CURSOR_MUTE, "MUTE", 0, "Mute", ""},
    {WM_CURSOR_ZOOM_IN, "ZOOM_IN", 0, "Zoom In", ""},
    {WM_CURSOR_ZOOM_OUT, "ZOOM_OUT", 0, "Zoom Out", ""},
    {0, NULL, 0, NULL, NULL},
};

#ifdef RNA_RUNTIME

#  include "dune_ctx.h"
#  include "dune_undo_system.h"

#  include "wm_types.h"

/* Needed since api doesn't use `const` in function signatures. */
static bool api_KeyMapItem_compare(struct wmKeyMapItem *k1, struct wmKeyMapItem *k2)
{
  return wm_keymap_item_compare(k1, k2);
}

static void api_KeyMapItem_to_string(wmKeyMapItem *kmi, bool compact, char *result)
{
  wm_keymap_item_to_string(kmi, compact, result, UI_MAX_SHORTCUT_STR);
}

static wmKeyMap *api_keymap_active(wmKeyMap *km, Ctx *C)
{
  wmWindowManager *wm = ctx_wm_manager(C);
  return wm_keymap_active(wm, km);
}

static void api_keymap_restore_to_default(wmKeyMap *km, bContext *C)
{
  wm_keymap_restore_to_default(km, ctx_wm_manager(C));
}

static void api_keymap_restore_item_to_default(wmKeyMap *km, Ctx *C, wmKeyMapItem *kmi)
{
  wm_keymap_item_restore_to_default(ctx_wm_manager(C), km, kmi);
}

static void api_op_report(wmOp *op, int type, const char *msg)
{
  dune_report(op->reports, type, msg);
}

static bool api_op_is_repeat(wmOp *op, Ctx *C)
{
  return wm_op_is_repeat(C, op);
}

/* since event isn't needed... */
static void api_op_enum_search_invoke(Ctx *C, wmOp *op)
{
  w_enum_search_invoke(C, op, NULL);
}

static bool api_event_modal_handler_add(struct Ctx *C, struct wmOp *op)
{
  return wm_event_add_modal_handler(C, op) != NULL;
}

/* XXX, need a way for python to know event types, 0x0110 is hard coded */
static wmTimer *api_event_timer_add(struct wmWindowManager *wm, float time_step, wmWindow *win)
{
  return wm_event_add_timer(wm, win, 0x0110, time_step);
}

static void api_event_timer_remove(struct wmWindowManager *wm, wmTimer *timer)
{
  wm_event_remove_timer(wm, timer->win, timer);
}

static wmGizmoGroupType *wm_gizmogrouptype_find_for_add_remove(ReportList *reports,
                                                               const char *idname)
{
  wmGizmoGroupType *gzgt = wm_gizmogrouptype_find(idname, true);
  if (gzgt == NULL) {
    dune_reportf(reports, RPT_ERROR, "Gizmo group type '%s' not found!", idname);
    return NULL;
  }
  if (gzgt->flag & WM_GIZMOGROUPTYPE_PERSISTENT) {
    dube_reportf(reports, RPT_ERROR, "Gizmo group '%s' has 'PERSISTENT' option set!", idname);
    return NULL;
  }
  return gzgt;
}

static void api_gizmo_group_type_ensure(ReportList *reports, const char *idname)
{
  wmGizmoGroupType *gzgt = wm_gizmogrouptype_find_for_add_remove(reports, idname);
  if (gzgt != NULL) {
    wm_gizmo_group_type_ensure_ptr(gzgt);
  }
}

static void api_gizmo_group_type_unlink_delayed(ReportList *reports, const char *idname)
{
  wmGizmoGroupType *gzgt = wm_gizmogrouptype_find_for_add_remove(reports, idname);
  if (gzgt != NULL) {
    WM_gizmo_group_type_unlink_delayed_ptr(gzgt);
  }
}

/* Placeholder data for final implementation of a true progress-bar. */
static struct wmStaticProgress {
  float min;
  float max;
  bool is_valid;
} wm_progress_state = {0, 0, false};

static void api_progress_begin(struct wmWindowManager *UNUSED(wm), float min, float max)
{
  float range = max - min;
  if (range != 0) {
    wm_progress_state.min = min;
    wm_progress_state.max = max;
    wm_progress_state.is_valid = true;
  }
  else {
    wm_progress_state.is_valid = false;
  }
}

static void api_progress_update(struct wmWindowManager *wm, float value)
{
  if (wm_progress_state.is_valid) {
    /* Map to cursor_time range [0,9999] */
    wmWindow *win = wm->winactive;
    if (win) {
      int val = (int)(10000 * (value - wm_progress_state.min) /
                      (wm_progress_state.max - wm_progress_state.min));
      wm_cursor_time(win, val);
    }
  }
}

static void api_progress_end(struct wmWindowManager *wm)
{
  if (wm_progress_state.is_valid) {
    wmWindow *win = wm->winactive;
    if (win) {
      wm_cursor_modal_restore(win);
      wm_progress_state.is_valid = false;
    }
  }
}

/* wrap these because of 'const wmEvent *' */
static int api_op_confirm(Ctx *C, wmOp *op, wmEvent *event)
{
  return wm_op_confirm(C, op, event);
}
static int api_op_props_popup(Ctx *C, wmOp *op, wmEvent *event)
{
  return wm_op_props_popup(C, op, event);
}

static int keymap_item_mod_flag_from_args(bool any, int shift, int ctrl, int alt, int oskey)
{
  int mod = 0;
  if (any) {
    mod = KM_ANY;
  }
  else {
    if (shift == KM_MOD_HELD) {
      mod |= KM_SHIFT;
    }
    else if (shift == KM_ANY) {
      mod |= KM_SHIFT_ANY;
    }

    if (ctrl == KM_MOD_HELD) {
      mod |= KM_CTRL;
    }
    else if (ctrl == KM_ANY) {
      mod |= KM_CTRL_ANY;
    }

    if (alt == KM_MOD_HELD) {
      mod |= KM_ALT;
    }
    else if (alt == KM_ANY) {
      mod |= KM_ALT_ANY;
    }

    if (oskey == KM_MOD_HELD) {
      mod |= KM_OSKEY;
    }
    else if (oskey == KM_ANY) {
      mod |= KM_OSKEY_ANY;
    }
  }
  return mod;
}

static wmKeyMapItem *api_KeyMap_item_new(wmKeyMap *km,
                                         ReportList *reports,
                                         const char *idname,
                                         int type,
                                         int value,
                                         bool any,
                                         int shift,
                                         int ctrl,
                                         int alt,
                                         int oskey,
                                         int keymod,
                                         int direction,
                                         bool repeat,
                                         bool head)
{
  /* only on non-modal maps */
  if (km->flag & KEYMAP_MODAL) {
    dune_report(reports, RPT_ERROR, "Not a non-modal keymap");
    return NULL;
  }

  // wmWindowManager *wm = CTX_wm_manager(C);
  wmKeyMapItem *kmi = NULL;
  char idname_bl[OP_MAX_TYPENAME];
  const int mod = keymap_item_mod_flag_from_args(any, shift, ctrl, alt, oskey);

  wm_op_bl_idname(idname_bl, idname);

  /* create keymap item */
  kmi = wm_keymap_add_item(km,
                           idname_bl,
                           &(const KeyMapItem_Params){
                               .type = type,
                               .value = value,
                               .mod = mod,
                               .keymod = keymod,
                               .direction = direction,
                           });

  if (!repeat) {
    kmi->flag |= KMI_REPEAT_IGNORE;
  }

  /* #32437 allow scripts to define hotkeys that get added to start of keymap
   *          so that they stand a chance against catch-all defines later on
   */
  if (head) {
    lib_remlink(&km->items, kmi);
    lib_addhead(&km->items, kmi);
  }

  return kmi;
}

static wmKeyMapItem *api_KeyMap_item_new_from_item(wmKeyMap *km,
                                                   ReportList *reports,
                                                   wmKeyMapItem *kmi_src,
                                                   bool head)
{
  // wmWindowManager *wm = CTX_wm_manager(C);

  if ((km->flag & KEYMAP_MODAL) == (kmi_src->idname[0] != '\0')) {
    dune_report(reports, RPT_ERROR, "Can not mix modal/non-modal items");
    return NULL;
  }

  /* create keymap item */
  wmKeyMapItem *kmi = em_keymap_add_item_copy(km, kmi_src);
  if (head) {
    lib_remlink(&km->items, kmi);
    lib_addhead(&km->items, kmi);
  }
  return kmi;
}

static wmKeyMapItem *api_KeyMap_item_new_modal(wmKeyMap *km,
                                               ReportList *reports,
                                               const char *propvalue_str,
                                               int type,
                                               int value,
                                               bool any,
                                               int shift,
                                               int ctrl,
                                               int alt,
                                               int oskey,
                                               int keymod,
                                               int direction,
                                               bool repeat)
{
  /* only modal maps */
  if ((km->flag & KEYMAP_MODAL) == 0) {
    dune_report(reports, RPT_ERROR, "Not a modal keymap");
    return NULL;
  }

  wmKeyMapItem *kmi = NULL;
  const int mod = keymap_item_mod_flag_from_args(any, shift, ctrl, alt, oskey);
  int propvalue = 0;

  KeyMapItem_Params params = {
      .type = type,
      .value = value,
      .mod = mod,
      .keymod = keymod,
      .direction = direction,
  };

  /* not initialized yet, do delayed lookup */
  if (!km->modal_items) {
    kmi = wm_modalkeymap_add_item_str(km, &params, propvalue_str);
  }
  else {
    if (api_enum_value_from_id(km->modal_items, propvalue_str, &propvalue) == 0) {
      dune_report(reports, RPT_WARNING, "Property value not in enumeration");
    }
    kmi = wm_modalkeymap_add_item(km, &params, propvalue);
  }

  if (!repeat) {
    kmi->flag |= KMI_REPEAT_IGNORE;
  }

  return kmi;
}

static void api_KeyMap_item_remove(wmKeyMap *km, ReportList *reports, ApiPtr *kmi_ptr)
{
  wmKeyMapItem *kmi = kmi_ptr->data;

  if (wm_keymap_remove_item(km, kmi) == false)
    dune_reportf(reports,
                RPT_ERROR,
                "KeyMapItem '%s' cannot be removed from '%s'",
                kmi->idname,
                km->idname);
    return;
  }

  API_PTR_INVALIDATE(kmi_ptr);
}

static ApiPtr api_KeyMap_item_find_from_op(Id *id,
                                           wmKeyMap *km,
                                           const char *idname,
                                           ApiPtr *props,
                                           int include_mask,
                                           int exclude_mask)
{
  char idname_bl[OP_MAX_TYPENAME];
  wn_op_bl_idname(idname_bl, idname);

  wmKeyMapItem *kmi = wm_key_event_op_from_keymap(
      km, idname_bl, props->data, include_mask, exclude_mask);
  ApiPtr kmi_ptr;
  api_ptr_create(id, &ApiKeyMapItem, kmi, &kmi_ptr);
  return kmi_ptr;
}

static ApiPtr api_KeyMap_item_match_event(Id *id, wmKeyMap *km, bContext *C, wmEvent *event)
{
  wmKeyMapItem *kmi = wm_event_match_keymap_item(C, km, event);
  PointerRNA kmi_ptr;
  api_ptr_create(id, &ApiKeyMapItem, kmi, &kmi_ptr);
  return kmi_ptr;
}

static wmKeyMap *api_keymap_new(wmKeyConfig *keyconf,
                                ReportList *reports,
                                const char *idname,
                                int spaceid,
                                int regionid,
                                bool modal,
                                bool tool)
{
  if (modal) {
    /* Sanity check: Don't allow add-ons to override internal modal key-maps
     * because this isn't supported, the restriction can be removed when
     * add-ons can define modal key-maps.
     * Currently this is only useful for add-ons to override built-in modal keymaps
     * which is not the intended use for add-on keymaps. */
    wmWindowManager *wm = G_MAIN->wm.first;
    if (keyconf == wm->addonconf) {
      dune_reportf(reports, RPT_ERROR, "Modal key-maps not supported for add-on key-config");
      return NULL;
    }
  }

  wmKeyMap *keymap;

  if (modal == 0) {
    keymap = wm_keymap_ensure(keyconf, idname, spaceid, regionid);
  }
  else {
    keymap = wm_modalkeymap_ensure(keyconf, idname, NULL); /* items will be lazy init */
  }

  if (keymap && tool) {
    keymap->flag |= KEYMAP_TOOL;
  }

  return keymap;
}

static wmKeyMap *api_keymap_find(wmKeyConfig *keyconf,
                                 const char *idname,
                                 int spaceid,
                                 int regionid)
{
  return wm_keymap_list_find(&keyconf->keymaps, idname, spaceid, regionid);
}

static wmKeyMap *api_keymap_find_modal(wmKeyConfig *UNUSED(keyconf), const char *idname)
{
  wmOpType *ot = wm_optype_find(idname, 0);

  if (!ot) {
    return NULL;
  }
  else {
    return ot->modalkeymap;
  }
}

static void api_KeyMap_remove(wmKeyConfig *keyconfig, ReportList *reports, ApiPtr *keymap_ptr)
{
  wmKeyMap *keymap = keymap_ptr->data;

  if (wm_keymap_remove(keyconfig, keymap) == false) {
    dune_reportf(reports, RPT_ERROR, "KeyConfig '%s' cannot be removed", keymap->idname);
    return;
  }

  API_PTR_INVALIDATE(keymap_ptr);
}

static void api_KeyConfig_remove(wmWindowManager *wm, ReportList *reports, ApiPtr *keyconf_ptr)
{
  wmKeyConfig *keyconf = keyconf_ptr->data;

  if (wm_keyconfig_remove(wm, keyconf) == false) {
    dune_reportf(reports, RPT_ERROR, "KeyConfig '%s' cannot be removed", keyconf->idname);
    return;
  }

  API_PTR_INVALIDATE(keyconf_ptr);
}

static ApiPtr api_KeyConfig_find_item_from_op(wmWindowManager *wm,
                                              Ctx *C,
                                              const char *idname,
                                              int opctx,
                                              ApiPtr *props,
                                              int include_mask,
                                              int exclude_mask,
                                              ApiPtr *km_ptr)
{
  char idname_bl[OP_MAX_TYPENAME];
  wm_op_bl_idname(idname_bl, idname);

  wmKeyMap *km = NULL;
  wmKeyMapItem *kmi = wm_key_event_op(
      C, idname_bl, opctx, props->data, include_mask, exclude_mask, &km);
  ApiPtr kmi_ptr;
  api_ptr_create(&wm->id, &ApiKeyMap, km, km_ptr);
  api_ptr_create(&wm->id, &ApiKeyMapItem, kmi, &kmi_ptr);
  return kmi_ptr;
}

static void api_KeyConfig_update(wmWindowManager *wm)
{
  wm_keyconfig_update(wm);
}

/* popup menu wrapper */
static ApiPtr api_PopMenuBegin(Ctx *C, const char *title, int icon)
{
  ApiPtr r_ptr;
  void *data;

  data = (void *)ui_popup_menu_begin(C, title, icon);

  api_ptr_create(NULL, &ApiUIPopupMenu, data, &r_ptr);

  return r_ptr;
}

static void api_PopMenuEnd(Ctx *C, ApiPtr *handle)
{
  ui_popup_menu_end(C, handle->data);
}

/* popover wrapper */
static ApiPtr api_PopoverBegin(Ctx *C, int ui_units_x, bool from_active_btn)
{
  ApiPtr r_ptr;
  void *data;

  data = (void *)ui_popover_begin(C, U.widget_unit * ui_units_x, from_active_button);

  api_ptr_create(NULL, &ApiUIPopover, data, &r_ptr);

  return r_ptr;
}

static void api_PopoverEnd(Ctx *C, ApiPtr *handle, wmKeyMap *keymap)
{
  ui_popover_end(C, handle->data, keymap);
}

/* pie menu wrapper */
static ApiPtr api_PieMenuBegin(Ctx *C, const char *title, int icon, ApiPtr *event)
{
  ApiPtr r_ptr;
  void *data;

  data = (void *)ui_pie_menu_begin(C, title, icon, event->data);

  api_ptr_create(NULL, &ApiUIPieMenu, data, &r_ptr);

  return r_ptr;
}

static void api_PieMenuEnd(Ctx *C, ApiPtr *handler)
{
  ui_pie_menu_end(C, handle->data);
}

static void api_WindowManager_print_undo_steps(wmWindowManager *wm)
{
  dune_undosys_print(wm->undo_stack);
}

static void api_WindowManager_tag_script_reload(void)
{
  wm_script_tag_reload();
  wm_main_add_notifier(NC_WINDOW, NULL);
}

static ApiPtr api_WindoManager_oper_props_last(const char *idname)
{
  wmOpType *ot = wm_optype_find(idname, true);

  if (ot != NULL) {
    ApiPtr ptr;
    wm_op_last_props_ensure(ot, &ptr);
    return ptr;
  }
  return ApiPtr_NULL;
}

static wmEvent *api_Window_event_add_simulate(wmWindow *win,
                                              ReportList *reports,
                                              int type,
                                              int value,
                                              const char *unicode,
                                              int x,
                                              int y,
                                              bool shift,
                                              bool ctrl,
                                              bool alt,
                                              bool oskey)
{
  if ((G.f & G_FLAG_EVENT_SIMULATE) == 0) {
    dune_report(reports, RPT_ERROR, "Not running with '--enable-event-simulate' enabled");
    return NULL;
  }

  if (!ELEM(value, KM_PRESS, KM_RELEASE, KM_NOTHING)) {
    dune_report(reports, RPT_ERROR, "Value: only 'PRESS/RELEASE/NOTHING' are supported");
    return NULL;
  }
  if (ISKEYBOARD(type) || ISMOUSE_BUTTON(type)) {
    if (!ELEM(value, KM_PRESS, KM_RELEASE)) {
      dune_report(reports, RPT_ERROR, "Value: must be 'PRESS/RELEASE' for keyboard/buttons");
      return NULL;
    }
  }
  if (ISMOUSE_MOTION(type)) {
    if (value != KM_NOTHING) {
      BKE_report(reports, RPT_ERROR, "Value: must be 'NOTHING' for motion");
      return NULL;
    }
  }
  if (unicode != NULL) {
    if (value != KM_PRESS) {
      dune_report(reports, RPT_ERROR, "Value: must be 'PRESS' when unicode is set");
      return NULL;
    }
  }
  /* TODO: validate NDOF. */

  if (unicode != NULL) {
    int len = lib_str_utf8_size(unicode);
    if (len == -1 || unicode[len] != '\0') {
      dune_report(reports, RPT_ERROR, "Only a single character supported");
      return NULL;
    }
  }

  wmEvent e = *win->eventstate;
  e.type = type;
  e.val = value;
  e.flag = 0;
  e.xy[0] = x;
  e.xy[1] = y;

  e.mod = 0;
  if (shift) {
    e.mod |= KM_SHIFT;
  }
  if (ctrl) {
    e.mod |= KM_CTRL;
  }
  if (alt) {
    e.mod |= KM_ALT;
  }
  if (oskey) {
    e.mod |= KM_OSKEY;
  }

  e.utf8_buf[0] = '\0';
  if (unicode != NULL) {
    STRNCPY(e.utf8_buf, unicode);
  }

  /* Until we expose setting tablet values here. */
  wm_event_tablet_data_default_set(&e.tablet);

  return wm_event_add_simulate(win, &e);
}

#else

#  define WM_GEN_INVOKE_EVENT (1 << 0)
#  define WM_GEN_INVOKE_SIZE (1 << 1)
#  define WM_GEN_INVOKE_RETURN (1 << 2)

static void api_generic_op_invoke(ApiFn *fn, int flag)
{
  ApiProp *parm;

  api_def_fn_flag(fn, FN_NO_SELF | FN_USE_CTX);
  parm = api_def_ptr(fn, "op", "Op", "", "Op to call");
  api_def_param_flags(parm, 0, PARM_REQUIRED);

  if (flag & WM_GEN_INVOKE_EVENT) {
    parm = api_def_ptr(fn, "event", "Event", "", "Event");
    api_def_param_flags(parm, 0, PARM_REQUIRED);
  }

  if (flag & WM_GEN_INVOKE_SIZE) {
    api_def_int(fn, "width", 300, 0, INT_MAX, "", "Width of the popup", 0, INT_MAX);
  }

  if (flag & WM_GEN_INVOKE_RETURN) {
    parm = api_def_enum_flag(
        fn, "result", api_enum_op_return_items, OP_FINISHED, "result", "");
    api_def_fn_return(fn, parm);
  }
}

void api_window(ApiStruct *sapi)
{
  ApiFn *fn;
  ApiProp *parm;

  fn = api_def_fn(sapi, "cursor_warp", "wm_cursor_warp");
  parm = api_def_int(fn, "x", 0, INT_MIN, INT_MAX, "", "", INT_MIN, INT_MAX);
  api_def_param_flags(parm, 0, PARM_REQUIRED);
  parm = api_def_int(fn, "y", 0, INT_MIN, INT_MAX, "", "", INT_MIN, INT_MAX);
  api_def_param_flags(parm, 0, PARM_REQUIRED);
  api_def_fn_ui_description(fn, "Set the cursor position");

  fn = api_def_fn(sapi, "cursor_set", "wm_cursor_set");
  parm = api_def_prop(fn, "cursor", PROP_ENUM, PROP_NONE);
  api_def_prop_enum_items(parm, api_enum_window_cursor_items);
  api_def_param_flags(parm, 0, PARM_REQUIRED);
  pi_def_fn_ui_description(func, "Set the cursor");

  fn = api_def_fn(sapi, "cursor_modal_set", "wm_cursor_modal_set");
  parm = api_def_prop(fn, "cursor", PROP_ENUM, PROP_NONE);
  api_def_prop_enum_items(parm, api_enum_window_cursor_items);
  api_def_param_flags(parm, 0, PARM_REQUIRED);
  api_def_fn_ui_description(fn, "Set the cursor, so the previous cursor can be restored");

  api_def_fn(sapi, "cursor_modal_restore", "WM_cursor_modal_restore");
  api_def_fn_ui_description(
      fn, "Restore the previous cursor after calling ``cursor_modal_set``");

  /* Arguments match 'rna_KeyMap_item_new'. */
  fn = api_def_fn(sapi, "event_simulate", "api_Window_event_add_simulate");
  api_def_fn_flag(fn, FN_USE_REPORTS);
  parm = api_def_enum(fn, "type", api_enum_event_type_items, 0, "Type", "");
  api_def_param_flags(parm, 0, PARM_REQUIRED);
  parm = api_def_enum(fn, "value", api_enum_event_value_items, 0, "Value", "");
  api_def_param_flags(parm, 0, PARM_REQUIRED);
  parm = api_def_string(fn, "unicode", NULL, 0, "", "");
  api_def_param_clear_flags(parm, PROP_NEVER_NULL, 0);

  api_def_int(fn, "x", 0, INT_MIN, INT_MAX, "", "", INT_MIN, INT_MAX);
  api_def_int(fn, "y", 0, INT_MIN, INT_MAX, "", "", INT_MIN, INT_MAX);

  api_def_bool(fn, "shift", 0, "Shift", "");
  api_def_bool(fn, "ctrl", 0, "Ctrl", "");
  api_def_bool(fn, "alt", 0, "Alt", "");
  api_def_bool(fn, "oskey", 0, "OS Key", "");
  parm = api_def_ptr(fn, "event", "Event", "Item", "Added key map item");
  api_def_fn_return(fn, parm);
}

void api_wm(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "fileselect_add", "WM_event_add_fileselect");
  RNA_def_function_ui_description(
      func,
      "Opens a file selector with an operator. "
      "The string properties 'filepath', 'filename', 'directory' and a 'files' "
      "collection are assigned when present in the operator");
  rna_generic_op_invoke(func, 0);

  func = RNA_def_function(srna, "modal_handler_add", "rna_event_modal_handler_add");
  RNA_def_function_ui_description(
      func,
      "Add a modal handler to the window manager, for the given modal operator "
      "(called by invoke() with self, just before returning {'RUNNING_MODAL'})");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_CONTEXT);
  parm = RNA_def_pointer(func, "operator", "Operator", "", "Operator to call");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_function_return(
      func, RNA_def_boolean(func, "handle", 1, "", "Whether adding the handler was successful"));

  func = RNA_def_function(srna, "event_timer_add", "rna_event_timer_add");
  RNA_def_function_ui_description(
      func, "Add a timer to the given window, to generate periodic 'TIMER' events");
  parm = RNA_def_property(func, "time_step", PROP_FLOAT, PROP_NONE);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_property_range(parm, 0.0, FLT_MAX);
  RNA_def_property_ui_text(parm, "Time Step", "Interval in seconds between timer events");
  RNA_def_pointer(func, "window", "Window", "", "Window to attach the timer to, or None");
  parm = RNA_def_pointer(func, "result", "Timer", "", "");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "event_timer_remove", "rna_event_timer_remove");
  parm = RNA_def_pointer(func, "timer", "Timer", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  func = RNA_def_function(srna, "gizmo_group_type_ensure", "rna_gizmo_group_type_ensure");
  RNA_def_function_ui_description(
      func, "Activate an existing widget group (when the persistent option isn't set)");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_REPORTS);
  parm = RNA_def_string(func, "identifier", NULL, 0, "", "Gizmo group type name");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);

  func = RNA_def_function(
      srna, "gizmo_group_type_unlink_delayed", "rna_gizmo_group_type_unlink_delayed");
  RNA_def_function_ui_description(func,
                                  "Unlink a widget group (when the persistent option is set)");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_REPORTS);
  parm = RNA_def_string(func, "identifier", NULL, 0, "", "Gizmo group type name");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);

  /* Progress bar interface */
  func = RNA_def_function(srna, "progress_begin", "rna_progress_begin");
  RNA_def_function_ui_description(func, "Start progress report");
  parm = RNA_def_property(func, "min", PROP_FLOAT, PROP_NONE);
  RNA_def_property_ui_text(parm, "min", "any value in range [0,9999]");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_property(func, "max", PROP_FLOAT, PROP_NONE);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_property_ui_text(parm, "max", "any value in range [min+1,9998]");

  func = RNA_def_function(srna, "progress_update", "rna_progress_update");
  RNA_def_function_ui_description(func, "Update the progress feedback");
  parm = RNA_def_property(func, "value", PROP_FLOAT, PROP_NONE);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_property_ui_text(
      parm, "value", "Any value between min and max as set in progress_begin()");

  func = RNA_def_function(srna, "progress_end", "rna_progress_end");
  RNA_def_function_ui_description(func, "Terminate progress report");

  /* invoke functions, for use with python */
  func = RNA_def_function(srna, "invoke_props_popup", "rna_Operator_props_popup");
  RNA_def_function_ui_description(
      func,
      "Operator popup invoke "
      "(show operator properties and execute it automatically on changes)");
  rna_generic_op_invoke(func, WM_GEN_INVOKE_EVENT | WM_GEN_INVOKE_RETURN);

  /* invoked dialog opens popup with OK button, does not auto-exec operator. */
  func = RNA_def_function(srna, "invoke_props_dialog", "WM_operator_props_dialog_popup");
  RNA_def_function_ui_description(
      func,
      "Operator dialog (non-autoexec popup) invoke "
      "(show operator properties and only execute it on click on OK button)");
  rna_generic_op_invoke(func, WM_GEN_INVOKE_SIZE | WM_GEN_INVOKE_RETURN);

  /* invoke enum */
  func = RNA_def_function(srna, "invoke_search_popup", "rna_Operator_enum_search_invoke");
  RNA_def_function_ui_description(
      func,
      "Operator search popup invoke which "
      "searches values of the operator's :class:`bpy.types.Operator.bl_property` "
      "(which must be an EnumProperty), executing it on confirmation");
  rna_generic_op_invoke(func, 0);

  /* invoke functions, for use with python */
  func = RNA_def_function(srna, "invoke_popup", "WM_operator_ui_popup");
  RNA_def_function_ui_description(func,
                                  "Operator popup invoke "
                                  "(only shows operator's properties, without executing it)");
  rna_generic_op_invoke(func, WM_GEN_INVOKE_SIZE | WM_GEN_INVOKE_RETURN);

  func = RNA_def_function(srna, "invoke_confirm", "rna_Operator_confirm");
  RNA_def_function_ui_description(
      func,
      "Operator confirmation popup "
      "(only to let user confirm the execution, no operator properties shown)");
  rna_generic_op_invoke(func, WM_GEN_INVOKE_EVENT | WM_GEN_INVOKE_RETURN);

  /* wrap UI_popup_menu_begin */
  func = RNA_def_function(srna, "popmenu_begin__internal", "rna_PopMenuBegin");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_CONTEXT);
  parm = RNA_def_string(func, "title", NULL, 0, "", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_property(func, "icon", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(parm, rna_enum_icon_items);
  /* return */
  parm = RNA_def_pointer(func, "menu", "UIPopupMenu", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_RNAPTR);
  RNA_def_function_return(func, parm);

  /* wrap UI_popup_menu_end */
  func = RNA_def_function(srna, "popmenu_end__internal", "rna_PopMenuEnd");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_CONTEXT);
  parm = RNA_def_pointer(func, "menu", "UIPopupMenu", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_RNAPTR | PARM_REQUIRED);

  /* wrap UI_popover_begin */
  func = RNA_def_function(srna, "popover_begin__internal", "rna_PopoverBegin");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_CONTEXT);
  RNA_def_property(func, "ui_units_x", PROP_INT, PROP_UNSIGNED);
  /* return */
  parm = RNA_def_pointer(func, "menu", "UIPopover", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_RNAPTR);
  RNA_def_function_return(func, parm);
  RNA_def_boolean(
      func, "from_active_button", 0, "Use Button", "Use the active button for positioning");

  /* wrap UI_popover_end */
  func = RNA_def_function(srna, "popover_end__internal", "rna_PopoverEnd");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_CONTEXT);
  parm = RNA_def_pointer(func, "menu", "UIPopover", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_RNAPTR | PARM_REQUIRED);
  RNA_def_pointer(func, "keymap", "KeyMap", "Key Map", "Active key map");

  /* wrap uiPieMenuBegin */
  func = RNA_def_function(srna, "piemenu_begin__internal", "rna_PieMenuBegin");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_CONTEXT);
  parm = RNA_def_string(func, "title", NULL, 0, "", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_property(func, "icon", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(parm, rna_enum_icon_items);
  parm = RNA_def_pointer(func, "event", "Event", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_RNAPTR);
  /* return */
  parm = RNA_def_pointer(func, "menu_pie", "UIPieMenu", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_RNAPTR);
  RNA_def_function_return(func, parm);

  /* wrap uiPieMenuEnd */
  func = RNA_def_function(srna, "piemenu_end__internal", "rna_PieMenuEnd");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_CONTEXT);
  parm = RNA_def_pointer(func, "menu", "UIPieMenu", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_RNAPTR | PARM_REQUIRED);

  /* access last operator options (optionally create). */
  func = RNA_def_function(
      srna, "operator_properties_last", "rna_WindoManager_operator_properties_last");
  RNA_def_function_flag(func, FUNC_NO_SELF);
  parm = RNA_def_string(func, "operator", NULL, 0, "", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  /* return */
  parm = RNA_def_pointer(func, "result", "OperatorProperties", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_RNAPTR);
  RNA_def_function_return(func, parm);

  RNA_def_function(srna, "print_undo_steps", "rna_WindowManager_print_undo_steps");

  /* Used by (#SCRIPT_OT_reload). */
  func = RNA_def_function(srna, "tag_script_reload", "rna_WindowManager_tag_script_reload");
  RNA_def_function_ui_description(
      func, "Tag for refreshing the interface after scripts have been reloaded");
  RNA_def_function_flag(func, FUNC_NO_SELF);

  parm = RNA_def_property(srna, "is_interface_locked", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_ui_text(
      parm,
      "Is Interface Locked",
      "If true, the interface is currently locked by a running job and data shouldn't be modified "
      "from application timers. Otherwise, the running job might conflict with the handler "
      "causing unexpected results or even crashes");
  RNA_def_property_clear_flag(parm, PROP_EDITABLE);
}

void RNA_api_operator(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  /* utility, not for registering */
  func = RNA_def_function(srna, "report", "rna_Operator_report");
  parm = RNA_def_enum_flag(func, "type", rna_enum_wm_report_items, 0, "Type", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_string(func, "message", NULL, 0, "Report Message", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);

  /* utility, not for registering */
  func = RNA_def_function(srna, "is_repeat", "rna_Operator_is_repeat");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);
  /* return */
  parm = RNA_def_boolean(func, "result", 0, "result", "");
  RNA_def_function_return(func, parm);

  /* Registration */

  /* poll */
  func = RNA_def_function(srna, "poll", NULL);
  RNA_def_function_ui_description(func, "Test if the operator can be called or not");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_REGISTER_OPTIONAL);
  RNA_def_function_return(func, RNA_def_boolean(func, "visible", 1, "", ""));
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* exec */
  func = RNA_def_function(srna, "execute", NULL);
  RNA_def_function_ui_description(func, "Execute the operator");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* better name? */
  parm = RNA_def_enum_flag(
      func, "result", rna_enum_operator_return_items, OPERATOR_FINISHED, "result", "");
  RNA_def_function_return(func, parm);

  /* check */
  func = RNA_def_function(srna, "check", NULL);
  RNA_def_function_ui_description(
      func, "Check the operator settings, return True to signal a change to redraw");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  parm = RNA_def_boolean(func, "result", 0, "result", ""); /* better name? */
  RNA_def_function_return(func, parm);

  /* invoke */
  func = RNA_def_function(srna, "invoke", NULL);
  RNA_def_function_ui_description(func, "Invoke the operator");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "event", "Event", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* better name? */
  parm = RNA_def_enum_flag(
      func, "result", rna_enum_operator_return_items, OPERATOR_FINISHED, "result", "");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "modal", NULL); /* same as invoke */
  RNA_def_function_ui_description(func, "Modal operator function");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "event", "Event", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* better name? */
  parm = RNA_def_enum_flag(
      func, "result", rna_enum_operator_return_items, OPERATOR_FINISHED, "result", "");
  RNA_def_function_return(func, parm);

  /* draw */
  func = RNA_def_function(srna, "draw", NULL);
  RNA_def_function_ui_description(func, "Draw function for the operator");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* cancel */
  func = RNA_def_function(srna, "cancel", NULL);
  RNA_def_function_ui_description(func, "Called when the operator is canceled");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL | FUNC_ALLOW_WRITE);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* description */
  fn = api_def_fn(sapi, "description", NULL);
  api_def_fn_ui_description(fn, "Compute a description string that depends on parameters");
  api_def_fn_flag(fn, FN_NO_SELF | FN_REGISTER_OPTIONAL);
  parm = api_def_string(fn, "result", NULL, 4096, "result", "");
  api_def_param_clear_flags(parm, PROP_NEVER_NULL, 0);
  api_def_param_flags(parm, PROP_THICK_WRAP, 0);
  api_def_fn_output(func, parm);
  parm = api_def_ptr(func, "context", "Context", "", "");
  api_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  parm = api_def_ptr(fn, "properties", "OperatorProperties", "", "");
  api_def_par_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
}

void api_macro(ApiStruct *sapi)
{
  ApiFn *fn;
  ApiProp *parm;

  /* utility, not for registering */
  func = RNA_def_function(srna, "report", "rna_Operator_report");
  parm = RNA_def_enum_flag(func, "type", rna_enum_wm_report_items, 0, "Type", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_string(func, "message", NULL, 0, "Report Message", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);

  /* Registration */

  /* poll */
  func = RNA_def_function(srna, "poll", NULL);
  RNA_def_function_ui_description(func, "Test if the operator can be called or not");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_REGISTER_OPTIONAL);
  RNA_def_function_return(func, RNA_def_boolean(func, "visible", 1, "", ""));
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);

  /* draw */
  func = RNA_def_function(srna, "draw", NULL);
  RNA_def_function_ui_description(func, "Draw function for the operator");
  RNA_def_function_flag(func, FUNC_REGISTER_OPTIONAL);
  parm = RNA_def_pointer(func, "context", "Context", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
}

void RNA_api_keyconfig(StructRNA *UNUSED(srna))
{
  /* FunctionRNA *func; */
  /* PropertyRNA *parm; */
}

void RNA_api_keymap(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "active", "rna_keymap_active");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);
  parm = RNA_def_pointer(func, "keymap", "KeyMap", "Key Map", "Active key map");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "restore_to_default", "rna_keymap_restore_to_default");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);

  func = RNA_def_function(srna, "restore_item_to_default", "rna_keymap_restore_item_to_default");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);
  parm = RNA_def_pointer(func, "item", "KeyMapItem", "Item", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
}

void RNA_api_keymapitem(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = api_def_fn(srna, "compare", "rna_KeyMapItem_compare");
  parm = api_def_ptr(func, "item", "KeyMapItem", "Item", "");
  api_def_param_flags(parm, 0, PARM_REQUIRED);
  parm = api_def_bool(fn, "result", 0, "Comparison result", "");
  api_def_fn_return(fn, parm);

  func = api_def_fn(sapi, "to_string", "rna_KeyMapItem_to_string");
  api_def_bool(fn, "compact", false, "Compact", "");
  parm = api_def_string(fn, "result", NULL, UI_MAX_SHORTCUT_STR, "result", "");
  api_def_param_flags(parm, PROP_THICK_WRAP, 0);
  api_def_fn_output(fn, parm);
}

void RNA_api_keymapitems(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "new", "rna_KeyMap_item_new");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_string(func, "idname", NULL, 0, "Operator Identifier", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_enum(func, "type", rna_enum_event_type_items, 0, "Type", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_enum(func, "value", rna_enum_event_value_items, 0, "Value", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_boolean(func, "any", 0, "Any", "");
  RNA_def_int(func, "shift", KM_NOTHING, KM_ANY, KM_MOD_HELD, "Shift", "", KM_ANY, KM_MOD_HELD);
  RNA_def_int(func, "ctrl", KM_NOTHING, KM_ANY, KM_MOD_HELD, "Ctrl", "", KM_ANY, KM_MOD_HELD);
  RNA_def_int(func, "alt", KM_NOTHING, KM_ANY, KM_MOD_HELD, "Alt", "", KM_ANY, KM_MOD_HELD);
  RNA_def_int(func, "oskey", KM_NOTHING, KM_ANY, KM_MOD_HELD, "OS Key", "", KM_ANY, KM_MOD_HELD);
  RNA_def_enum(func, "key_modifier", rna_enum_event_type_items, 0, "Key Modifier", "");
  RNA_def_enum(func, "direction", rna_enum_event_direction_items, KM_ANY, "Direction", "");
  RNA_def_boolean(func, "repeat", false, "Repeat", "When set, accept key-repeat events");
  RNA_def_boolean(func,
                  "head",
                  0,
                  "At Head",
                  "Force item to be added at start (not end) of key map so that "
                  "it doesn't get blocked by an existing key map item");
  parm = RNA_def_pointer(func, "item", "KeyMapItem", "Item", "Added key map item");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "new_modal", "rna_KeyMap_item_new_modal");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_string(func, "propvalue", NULL, 0, "Property Value", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_enum(func, "type", rna_enum_event_type_items, 0, "Type", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_enum(func, "value", rna_enum_event_value_items, 0, "Value", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_boolean(func, "any", 0, "Any", "");
  RNA_def_int(func, "shift", KM_NOTHING, KM_ANY, KM_MOD_HELD, "Shift", "", KM_ANY, KM_MOD_HELD);
  RNA_def_int(func, "ctrl", KM_NOTHING, KM_ANY, KM_MOD_HELD, "Ctrl", "", KM_ANY, KM_MOD_HELD);
  RNA_def_int(func, "alt", KM_NOTHING, KM_ANY, KM_MOD_HELD, "Alt", "", KM_ANY, KM_MOD_HELD);
  RNA_def_int(func, "oskey", KM_NOTHING, KM_ANY, KM_MOD_HELD, "OS Key", "", KM_ANY, KM_MOD_HELD);
  RNA_def_enum(func, "key_modifier", rna_enum_event_type_items, 0, "Key Modifier", "");
  RNA_def_enum(func, "direction", rna_enum_event_direction_items, KM_ANY, "Direction", "");
  RNA_def_boolean(func, "repeat", false, "Repeat", "When set, accept key-repeat events");
  parm = RNA_def_pointer(func, "item", "KeyMapItem", "Item", "Added key map item");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "new_from_item", "rna_KeyMap_item_new_from_item");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "item", "KeyMapItem", "Item", "Item to use as a reference");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED);
  RNA_def_boolean(func, "head", 0, "At Head", "");
  parm = RNA_def_pointer(func, "result", "KeyMapItem", "Item", "Added key map item");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_KeyMap_item_remove");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "item", "KeyMapItem", "Item", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, 0);

  func = RNA_def_function(srna, "from_id", "WM_keymap_item_find_id");
  parm = RNA_def_property(func, "id", PROP_INT, PROP_NONE);
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_property_ui_text(parm, "id", "ID of the item");
  parm = RNA_def_pointer(func, "item", "KeyMapItem", "Item", "");
  RNA_def_function_return(func, parm);

  /* Keymap introspection
   * Args follow: KeyConfigs.find_item_from_operator */
  func = RNA_def_function(srna, "find_from_operator", "rna_KeyMap_item_find_from_operator");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID);
  parm = RNA_def_string(func, "idname", NULL, 0, "Operator Identifier", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "properties", "OperatorProperties", "", "");
  RNA_def_parameter_flags(parm, 0, PARM_RNAPTR);
  RNA_def_enum_flag(
      func, "include", rna_enum_event_type_mask_items, EVT_TYPE_MASK_ALL, "Include", "");
  RNA_def_enum_flag(func, "exclude", rna_enum_event_type_mask_items, 0, "Exclude", "");
  parm = RNA_def_pointer(func, "item", "KeyMapItem", "", "");
  RNA_def_parameter_flags(parm, 0, PARM_RNAPTR);
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "match_event", "rna_KeyMap_item_match_event");
  RNA_def_function_flag(func, FUNC_USE_SELF_ID | FUNC_USE_CONTEXT);
  parm = RNA_def_pointer(func, "event", "Event", "", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "item", "KeyMapItem", "", "");
  RNA_def_parameter_flags(parm, 0, PARM_RNAPTR);
  RNA_def_function_return(func, parm);
}

void RNA_api_keymaps(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "new", "rna_keymap_new"); /* add_keymap */
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_function_ui_description(
      func,
      "Ensure the keymap exists. This will return the one with the given name/space type/region "
      "type, or create a new one if it does not exist yet.");

  parm = RNA_def_string(func, "name", NULL, 0, "Name", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_enum(func, "space_type", rna_enum_space_type_items, SPACE_EMPTY, "Space Type", "");
  RNA_def_enum(
      func, "region_type", rna_enum_region_type_items, RGN_TYPE_WINDOW, "Region Type", "");
  RNA_def_boolean(func, "modal", 0, "Modal", "Keymap for modal operators");
  RNA_def_boolean(func, "tool", 0, "Tool", "Keymap for active tools");
  parm = RNA_def_pointer(func, "keymap", "KeyMap", "Key Map", "Added key map");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_KeyMap_remove"); /* remove_keymap */
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func, "keymap", "KeyMap", "Key Map", "Removed key map");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, 0);

  func = RNA_def_function(srna, "find", "rna_keymap_find"); /* find_keymap */
  parm = RNA_def_string(func, "name", NULL, 0, "Name", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  RNA_def_enum(func, "space_type", rna_enum_space_type_items, SPACE_EMPTY, "Space Type", "");
  RNA_def_enum(
      func, "region_type", rna_enum_region_type_items, RGN_TYPE_WINDOW, "Region Type", "");
  parm = RNA_def_pointer(func, "keymap", "KeyMap", "Key Map", "Corresponding key map");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "find_modal", "rna_keymap_find_modal"); /* find_keymap_modal */
  parm = RNA_def_string(func, "name", NULL, 0, "Operator Name", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_pointer(func, "keymap", "KeyMap", "Key Map", "Corresponding key map");
  RNA_def_function_return(func, parm);
}

void RNA_api_keyconfigs(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "new", "WM_keyconfig_new_user"); /* add_keyconfig */
  parm = RNA_def_string(func, "name", NULL, 0, "Name", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_pointer(
      func, "keyconfig", "KeyConfig", "Key Configuration", "Added key configuration");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_KeyConfig_remove"); /* remove_keyconfig */
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "keyconfig", "KeyConfig", "Key Configuration", "Removed key configuration");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, 0);

  /* Helper functions */

  /* Keymap introspection */
  func = RNA_def_function(
      srna, "find_item_from_operator", "rna_KeyConfig_find_item_from_operator");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);
  parm = RNA_def_string(func, "idname", NULL, 0, "Operator Identifier", "");
  RNA_def_parameter_flags(parm, 0, PARM_REQUIRED);
  parm = RNA_def_property(func, "context", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(parm, rna_enum_operator_context_items);
  parm = RNA_def_pointer(func, "properties", "OperatorProperties", "", "");
  RNA_def_parameter_flags(parm, 0, PARM_RNAPTR);
  RNA_def_enum_flag(
      func, "include", rna_enum_event_type_mask_items, EVT_TYPE_MASK_ALL, "Include", "");
  RNA_def_enum_flag(func, "exclude", rna_enum_event_type_mask_items, 0, "Exclude", "");
  parm = RNA_def_pointer(func, "keymap", "KeyMap", "", "");
  RNA_def_parameter_flags(parm, 0, PARM_RNAPTR | PARM_OUTPUT);
  parm = RNA_def_pointer(func, "item", "KeyMapItem", "", "");
  RNA_def_parameter_flags(parm, 0, PARM_RNAPTR);
  RNA_def_function_return(func, parm);

  RNA_def_function(srna, "update", "rna_KeyConfig_update"); /* WM_keyconfig_update */
}

#endif
