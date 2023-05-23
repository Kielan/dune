#include <float.h>
#include <limits.h>
#include <stdlib.h>

#include "types_pen_legacy.h"
#include "types_object.h"
#include "types_scene.h"
#include "types_shader_fx.h"

#include "mem_guardedalloc.h"

#include "lib_math.h"

#include "lang_translation.h"

#include "dune_animsys.h"

#include "api_access.h"
#include "api_define.h"
#include "api_enum_types.h"

#include "api_internal.h"

#include "wm_api.h"
#include "wm_types.h"

const EnumPropItem api_enum_object_shaderfx_type_items[] = {
    {eShaderFxType_Blur, "FX_BLUR", ICON_SHADERFX, "Blur", "Apply Gaussian Blur to object"},
    {eShaderFxType_Colorize,
     "FX_COLORIZE",
     ICON_SHADERFX,
     "Colorize",
     "Apply different tint effects"},
    {eShaderFxType_Flip, "FX_FLIP", ICON_SHADERFX, "Flip", "Flip image"},
    {eShaderFxType_Glow, "FX_GLOW", ICON_SHADERFX, "Glow", "Create a glow effect"},
    {eShaderFxType_Pixel, "FX_PIXEL", ICON_SHADERFX, "Pixelate", "Pixelate image"},
    {eShaderFxType_Rim, "FX_RIM", ICON_SHADERFX, "Rim", "Add a rim to the image"},
    {eShaderFxType_Shadow, "FX_SHADOW", ICON_SHADERFX, "Shadow", "Create a shadow effect"},
    {eShaderFxType_Swirl, "FX_SWIRL", ICON_SHADERFX, "Swirl", "Create a rotation distortion"},
    {eShaderFxType_Wave,
     "FX_WAVE",
     ICON_SHADERFX,
     "Wave Distortion",
     "Apply sinusoidal deformation"},
    {0, NULL, 0, NULL, NULL},
};

static const EnumPropItem api_enum_shaderfx_rim_modes_items[] = {
    {eShaderFxRimMode_Normal, "NORMAL", 0, "Regular", ""},
    {eShaderFxRimMode_Overlay, "OVERLAY", 0, "Overlay", ""},
    {eShaderFxRimMode_Add, "ADD", 0, "Add", ""},
    {eShaderFxRimMode_Subtract, "SUBTRACT", 0, "Subtract", ""},
    {eShaderFxRimMode_Multiply, "MULTIPLY", 0, "Multiply", ""},
    {eShaderFxRimMode_Divide, "DIVIDE", 0, "Divide", ""},
    {0, NULL, 0, NULL, NULL}};

static const EnumPropItem api_enum_shaderfx_glow_modes_items[] = {
    {eShaderFxGlowMode_Luminance, "LUMINANCE", 0, "Luminance", ""},
    {eShaderFxGlowMode_Color, "COLOR", 0, "Color", ""},
    {0, NULL, 0, NULL, NULL}};

static const EnumPropItem api_enum_shaderfx_colorize_modes_items[] = {
    {eShaderFxColorizeMode_GrayScale, "GRAYSCALE", 0, "Gray Scale", ""},
    {eShaderFxColorizeMode_Sepia, "SEPIA", 0, "Sepia", ""},
    {eShaderFxColorizeMode_Duotone, "DUOTONE", 0, "Duotone", ""},
    {eShaderFxColorizeMode_Transparent, "TRANSPARENT", 0, "Transparent", ""},
    {eShaderFxColorizeMode_Custom, "CUSTOM", 0, "Custom", ""},
    {0, NULL, 0, NULL, NULL}};

static const EnumPropItem api_enum_glow_blend_modes_items[] = {
    {eGplBlendMode_Regular, "REGULAR", 0, "Regular", ""},
    {eGplBlendMode_Add, "ADD", 0, "Add", ""},
    {eGplBlendMode_Subtract, "SUBTRACT", 0, "Subtract", ""},
    {eGplBlendMode_Multiply, "MULTIPLY", 0, "Multiply", ""},
    {eGplBlendMode_Divide, "DIVIDE", 0, "Divide", ""},
    {0, NULL, 0, NULL, NULL}};

#ifdef API_RUNTIME

#  include "dune_shader_fx.h"

#  include "graph.h"
#  include "graph_build.h"

static ApiStruct *api_ShaderFx_refine(struct ApiPtr *ptr)
{
  ShaderFxData *md = (ShaderFxData *)ptr->data;

  switch ((ShaderFxType)md->type) {
    case eShaderFxType_Blur:
      return &Api_ShaderFxBlur;
    case eShaderFxType_Colorize:
      return &Api_ShaderFxColorize;
    case eShaderFxType_Wave:
      return &Api_ShaderFxWave;
    case eShaderFxType_Pixel:
      return &Api_ShaderFxPixel;
    case eShaderFxType_Rim:
      return &Api_ShaderFxRim;
    case eShaderFxType_Shadow:
      return &Api_ShaderFxShadow;
    case eShaderFxType_Swirl:
      return &Api_ShaderFxSwirl;
    case eShaderFxType_Flip:
      return &Api_ShaderFxFlip;
    case eShaderFxType_Glow:
      return &Api_ShaderFxGlow;
      /* Default */
    case eShaderFxType_None:
    case NUM_SHADER_FX_TYPES:
    default:
      return &Api_ShaderFx;
  }

  return &Api_ShaderFx;
}

static void api_ShaderFx_name_set(ApiPtr *ptr, const char *value)
{
  ShaderFxData *gmd = ptr->data;
  char oldname[sizeof(gmd->name)];

  /* make a copy of the old name first */
  STRNCPY(oldname, gmd->name);

  /* copy the new name into the name slot */
  STRNCPY_UTF8(gmd->name, value);

  /* make sure the name is truly unique */
  if (ptr->owner_id) {
    Object *ob = (Object *)ptr->owner_id;
    dune_shaderfx_unique_name(&ob->shader_fx, gmd);
  }

  /* fix all the animation data which may link to this */
  dune_animdata_fix_paths_rename_all(NULL, "shader_effects", oldname, gmd->name);
}

static char *api_ShaderFx_path(const ApiPtr *ptr)
{
  const ShaderFxData *gmd = ptr->data;
  char name_esc[sizeof(gmd->name) * 2];

  lib_str_escape(name_esc, gmd->name, sizeof(name_esc));
  return lib_sprintfn("shader_effects[\"%s\"]", name_esc);
}

static void api_ShaderFx_update(Main *UNUSED(main), Scene *UNUSED(scene), ApiPtr *ptr)
{
  graph_id_tag_update(ptr->owner_id, ID_RECALC_GEOMETRY);
  wm_main_add_notifier(NC_OBJECT | ND_SHADERFX, ptr->owner_id);
}

static void api_ShaderFx_dependency_update(Main *main, Scene *scene, PointerRNA *ptr)
{
  api_ShaderFx_update(main, scene, ptr);
  graph_relations_tag_update(main);
}

/* Objects */

static void shaderfx_object_set(Object *self, Object **ob_p, int type, PointerRNA value)
{
  Object *ob = value.data;

  if (!self || ob != self) {
    if (!ob || type == OB_EMPTY || ob->type == type) {
      id_lib_extern((Id *)ob);
      *ob_p = ob;
    }
  }
}

#  define API_FX_OBJECT_SET(_type, _prop, _obtype) \
    static void api_##_type##ShaderFx_##_prop##_set( \
        ApiPtr *ptr, ApiPtr value, struct ReportList *UNUSED(reports)) \
    { \
      _type##ShaderFxData *tmd = (_type##ShaderFxData *)ptr->data; \
      shaderfx_object_set((Object *)ptr->owner_id, &tmd->_prop, _obtype, value); \
    }

API_FX_OBJECT_SET(Shadow, object, OB_EMPTY);
API_FX_OBJECT_SET(Swirl, object, OB_EMPTY);

#  undef API_FX_OBJECT_SET

#else

static void api_def_shader_fx_blur(DuneApi *dapi)
{
  ApiStruct *sapi;
  ApiProp *prop;

  sapi = api_def_struct(dapi, "ShaderFxBlur", "ShaderFx");
  api_def_struct_ui_text(sapi, "Gaussian Blur Effect", "Gaussian Blur effect");
  api_def_struct_stype(sapi, "BlurShaderFxData");
  api_def_struct_ui_icon(sapi, ICON_SHADERFX);

  api_define_lib_overridable(true);

  prop = api_def_prop(sapi, "size", PROP_FLOAT, PROP_XYZ);
  api_def_prop_float_stype(prop, NULL, "radius");
  api_def_prop_range(prop, 0.0f, FLT_MAX);
  api_def_prop_ui_text(prop, "Size", "Factor of Blur");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "api_ShaderFx_update");

  prop = api_def_prop(sapi, "samples", PROP_INT, PROP_NONE);
  api_def_prop_int_stype(prop, NULL, "samples");
  api_def_prop_range(prop, 0, 32);
  api_def_prop_ui_range(prop, 0, 32, 2, -1);
  api_def_prop_int_default(prop, 4);
  api_def_prop_ui_text(prop, "Samples", "Number of Blur Samples (zero, disable blur)");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "api_ShaderFx_update");

  prop = api_def_prop(sapi, "rotation", PROP_FLOAT, PROP_ANGLE);
  api_def_prop_float_stype(prop, NULL, "rotation");
  api_def_prop_range(prop, -FLT_MAX, FLT_MAX);
  api_def_prop_ui_text(prop, "Rotation", "Rotation of the effect");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "api_ShaderFx_update"

  prop = api_def_prop(sapi, "use_dof_mode", PROP_BOOLEAN, PROP_NONE);
  api_def_prop_bool_stype(prop, NULL, "flag", FX_BLUR_DOF_MODE);
  api_def_prop_ui_text(prop, "Use as Depth Of Field", "Blur using camera depth of field");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "api_ShaderFx_update");

  RNA_define_lib_overridable(false);
}

static void rna_def_shader_fx_colorize(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "ShaderFxColorize", "ShaderFx");
  RNA_def_struct_ui_text(srna, "Colorize Effect", "Colorize effect");
  RNA_def_struct_sdna(srna, "ColorizeShaderFxData");
  RNA_def_struct_ui_icon(srna, ICON_SHADERFX);

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "factor", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "factor");
  RNA_def_property_range(prop, 0, 1.0);
  RNA_def_property_ui_text(prop, "Factor", "Mix factor");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = api_def_prop(sapi, "low_color", PROP_FLOAT, PROP_COLOR);
  api_def_prop_range(prop, 0.0, 1.0);
  api_def_prop_float_stype(prop, NULL, "low_color");
  api_def_prop_array(prop, 4);
  api_def_prop_ui_text(prop, "Low Color", "First color used for effect");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = api_def_prop(sapi, "high_color", PROP_FLOAT, PROP_COLOR);
  api_def_prop_range(prop, 0.0, 1.0);
  api_def_prop_float_stype(prop, NULL, "high_color");
  api_def_prop_array(prop, 4);
  api_def_prop_ui_text(prop, "High Color", "Second color used for effect");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = api_def_prop(sapi, "mode", PROP_ENUM, PROP_NONE);
  api_def_prop_enum_stype(prop, NULL, "mode");
  api_def_prop_enum_items(prop, api_enum_shaderfx_colorize_modes_items);
  api_def_prop_ui_text(prop, "Mode", "Effect mode");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  api_define_lib_overridable(false);
}

static void api_def_shader_fx_wave(DuneApi *dapi)
{
  ApiStruct *sapi;
  ApiProp *prop;

  static EnumPropItem prop_shaderfx_wave_type_items[] = {
      {0, "HORIZONTAL", 0, "Horizontal", ""},
      {1, "VERTICAL", 0, "Vertical", ""},
      {0, NULL, 0, NULL, NULL}};

  sapi = api_def_struct(dapi, "ShaderFxWave", "ShaderFx");
  api_def_struct_ui_text(sapi, "Wave Deformation Effect", "Wave Deformation effect");
  api_def_struct_stype(sapi, "WaveShaderFxData");
  api_def_struct_ui_icon(sapi, ICON_SHADERFX);

  api_define_lib_overridable(true);

  prop = api_def_prop(sapi, "orientation", PROP_ENUM, PROP_NONE);
  api_def_prop_enum_stype(prop, NULL, "orientation");
  api_def_prop_enum_items(prop, prop_shaderfx_wave_type_items);
  api_def_prop_ui_text(prop, "Orientation", "Direction of the wave");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = api_def_prop(sapi, "amplitude", PROP_FLOAT, PROP_NONE);
  api_def_prop_float_stype(prop, NULL, "amplitude");
  api_def_prop_range(prop, 0, FLT_MAX);
  api_def_prop_ui_text(prop, "Amplitude", "Amplitude of Wave");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = api_def_prop(sapi, "period", PROP_FLOAT, PROP_NONE);
  api_def_prop_float_stype(prop, NULL, "period");
  api_def_prop_range(prop, 0, FLT_MAX);
  api_def_propyl_ui_text(prop, "Period", "Period of Wave");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = api_def_prop(sapi, "phase", PROP_FLOAT, PROP_NONE);
  api_def_prop_float_stype(prop, NULL, "phase");
  api_def_prop_range(prop, -FLT_MAX, FLT_MAX);
  api_def_prop_ui_text(prop, "Phase", "Phase Shift of Wave");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  api_define_lib_overridable(false);
}

static void api_def_shader_fx_pixel(BlenderRNA *brna)
{
  ApiStruct *srna;
  ApiProp *prop;

  sapi = api_def_struct(dapi, "ShaderFxPixel", "ShaderFx");
  api_def_struct_ui_text(sapi, "Pixelate Effect", "Pixelate effect");
  api_def_struct_stype(sapi, "PixelShaderFxData");
  api_def_struct_ui_icon(sapi, ICON_SHADERFX);

  api_define_lib_overridable(true);

  prop = api_def_prop(sapi, "size", PROP_INT, PROP_PIXEL);
  api_def_prop_int_stype(prop, NULL, "size");
  api_def_prop_range(prop, 1, SHRT_MAX);
  api_def_prop_array(prop, 2)
  api_def_prop_ui_text(prop, "Size", "Pixel size");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "api_ShaderFx_update");

  prop = api_def_prop(sapi, "use_antialiasing", PROP_BOOL, PROP_NONE);
  api_def_prop_bool_negative_stype(prop, NULL, "flag", FX_PIXEL_FILTER_NEAREST);
  api_def_prop_ui_text(prop, "Antialiasing", "Antialias pixels");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  api_define_lib_overridable(false);
}

static void api_def_shader_fx_rim(DuneApi *dapi)
{
  ApiStruct *sapi;
  ApiProp *prop;

  sapi = api_def_struct(dapi, "ShaderFxRim", "ShaderFx");
  api_def_struct_ui_text(sapi, "Rim Effect", "Rim effect");
  api_def_struct_stype(sapi, "RimShaderFxData");
  api_def_struct_ui_icon(sapi, ICON_SHADERFX);

  api_define_lib_overridable(true);

  prop = api_def_property(srna, "offset", PROP_INT, PROP_PIXEL);
  api_def_prop_int_stype(prop, NULL, "offset");
  api_def_prop_range(prop, SHRT_MIN, SHRT_MAX);
  api_def_prop_ui_text(prop, "Offset", "Offset of the rim");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "api_ShaderFx_update");

  prop = api_def_prop(sapi, "rim_color", PROP_FLOAT, PROP_COLOR);
  api_def_prop_range(prop, 0.0, 1.0);
  api_def_prop_float_style(prop, NULL, "rim_rgb");
  api_def_prop_array(prop, 3);
  api_def_prop_ui_text(prop, "Rim Color", "Color used for Rim");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "api_ShaderFx_update");

  prop = api_def_prop(sapo, "mask_color", PROP_FLOAT, PROP_COLOR);
  api_def_prop_range(prop, 0.0, 1.0);
  api_def_prop_float_stype(prop, NULL, "mask_rgb");
  api_def_prop_array(prop, 3);
  api_def_prop_ui_text(prop, "Mask Color", "Color that must be kept");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = api_def_prop(sapi, "mode", PROP_ENUM, PROP_NONE);
  api_def_prop_enum_stype(prop, NULL, "mode");
  api_def_prop_enum_items(prop, rna_enum_shaderfx_rim_modes_items);
  api_def_prop_ui_text(prop, "Mode", "Blend mode");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = api_def_prop(sapi, "blur", PROP_INT, PROP_PIXEL);
  api_def_prop_int_stype(prop, NULL, "blur");
  api_def_prop_range(prop, 0, SHRT_MAX);
  api_def_prop_ui_text(
      prop, "Blur", "Number of pixels for blurring rim (set to 0 to disable)");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = api_def_prop(sapi, "samples", PROP_INT, PROP_NONE);
  api_def_prop_int_stype(prop, NULL, "samples");
  api_def_prop_range(prop, 0, 32);
  api_def_prop_ui_range(prop, 0, 32, 2, -1);
  api_def_prop_int_default(prop, 4);
  api_def_prop_ui_text(prop, "Samples", "Number of Blur Samples (zero, disable blur)");
  api_def_prop_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  api_define_lib_overridable(false);
}

static void api_def_shader_fx_shadow(BlenderRNA *brna)
{
  static EnumPropItem prop_shaderfx_shadow_type_items[] = {
      {0, "HORIZONTAL", 0, "Horizontal", ""},
      {1, "VERTICAL", 0, "Vertical", ""},
      {0, NULL, 0, NULL, NULL}};

  ApiStruct *sapi;
  ApiProp *prop;

  sapi = RNA_def_struct(brna, "ShaderFxShadow", "ShaderFx");
  api_def_struct_ui_text(srna, "Shadow Effect", "Shadow effect");
  api_def_struct_sdna(srna, "ShadowShaderFxData");
  api_def_struct_ui_icon(srna, ICON_SHADERFX);

  api_define_lib_overridable(true);

  prop = api_def_property(srna, "object", PROP_POINTER, PROP_NONE);
  api_def_prop_ui_text(prop, "Object", "Object to determine center of rotation");
  RNA_def_property_pointer_funcs(prop, NULL, "rna_ShadowShaderFx_object_set", NULL, NULL);
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_SELF_CHECK);
  RNA_def_property_update(prop, 0, "rna_ShaderFx_dependency_update");

  prop = RNA_def_property(srna, "offset", PROP_INT, PROP_PIXEL);
  RNA_def_property_int_sdna(prop, NULL, "offset");
  RNA_def_property_range(prop, SHRT_MIN, SHRT_MAX);
  RNA_def_property_ui_text(prop, "Offset", "Offset of the shadow");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "scale", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, NULL, "scale");
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
  RNA_def_property_ui_text(prop, "Scale", "Scale of the shadow");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "shadow_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, NULL, "shadow_rgba");
  RNA_def_property_array(prop, 4);
  RNA_def_property_ui_text(prop, "Shadow Color", "Color used for Shadow");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "orientation", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "orientation");
  RNA_def_property_enum_items(prop, prop_shaderfx_shadow_type_items);
  RNA_def_property_ui_text(prop, "Orientation", "Direction of the wave");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "amplitude", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "amplitude");
  RNA_def_property_range(prop, 0, FLT_MAX);
  RNA_def_property_ui_text(prop, "Amplitude", "Amplitude of Wave");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "period", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "period");
  RNA_def_property_range(prop, 0, FLT_MAX);
  RNA_def_property_ui_text(prop, "Period", "Period of Wave");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "phase", PROP_FLOAT, PROP_NONE);
  RNA_def_property_float_sdna(prop, NULL, "phase");
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
  RNA_def_property_ui_text(prop, "Phase", "Phase Shift of Wave");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "rotation", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "rotation");
  RNA_def_property_range(prop, DEG2RAD(-360), DEG2RAD(360));
  RNA_def_property_ui_range(prop, DEG2RAD(-360), DEG2RAD(360), 5, 2);
  RNA_def_property_ui_text(prop, "Rotation", "Rotation around center or object");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "blur", PROP_INT, PROP_PIXEL);
  RNA_def_property_int_sdna(prop, NULL, "blur");
  RNA_def_property_range(prop, 0, SHRT_MAX);
  RNA_def_property_ui_text(
      prop, "Blur", "Number of pixels for blurring shadow (set to 0 to disable)");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "samples", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "samples");
  RNA_def_property_range(prop, 0, 32);
  RNA_def_property_ui_range(prop, 0, 32, 2, -1);
  RNA_def_property_int_default(prop, 4);
  RNA_def_property_ui_text(prop, "Samples", "Number of Blur Samples (zero, disable blur)");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "use_object", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", FX_SHADOW_USE_OBJECT);
  RNA_def_property_ui_text(prop, "Use Object", "Use object as center of rotation");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "use_wave", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", FX_SHADOW_USE_WAVE);
  RNA_def_property_ui_text(prop, "Wave", "Use wave effect");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  RNA_define_lib_overridable(false);
}

static void rna_def_shader_fx_glow(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "ShaderFxGlow", "ShaderFx");
  RNA_def_struct_ui_text(srna, "Glow Effect", "Glow effect");
  RNA_def_struct_sdna(srna, "GlowShaderFxData");
  RNA_def_struct_ui_icon(srna, ICON_SHADERFX);

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "glow_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, NULL, "glow_color");
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Glow Color", "Color used for generated glow");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "opacity", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "glow_color[3]");
  RNA_def_property_range(prop, 0.0, 1.0f);
  RNA_def_property_ui_text(prop, "Opacity", "Effect Opacity");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "select_color", PROP_FLOAT, PROP_COLOR);
  RNA_def_property_range(prop, 0.0, 1.0);
  RNA_def_property_float_sdna(prop, NULL, "select_color");
  RNA_def_property_array(prop, 3);
  RNA_def_property_ui_text(prop, "Select Color", "Color selected to apply glow");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "mode");
  RNA_def_property_enum_items(prop, rna_enum_shaderfx_glow_modes_items);
  RNA_def_property_ui_text(prop, "Mode", "Glow mode");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "threshold", PROP_FLOAT, PROP_FACTOR);
  RNA_def_property_float_sdna(prop, NULL, "threshold");
  RNA_def_property_range(prop, 0.0f, 1.0f);
  RNA_def_property_ui_range(prop, 0.0f, 1.0f, 0.1f, 3);
  RNA_def_property_ui_text(prop, "Threshold", "Limit to select color for glow effect");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  /* Use blur fields to make compatible with blur filter */
  prop = RNA_def_property(srna, "size", PROP_FLOAT, PROP_XYZ);
  RNA_def_property_float_sdna(prop, NULL, "blur");
  RNA_def_property_range(prop, 0.0f, FLT_MAX);
  RNA_def_property_ui_text(prop, "Size", "Size of the effect");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "samples", PROP_INT, PROP_NONE);
  RNA_def_property_int_sdna(prop, NULL, "samples");
  RNA_def_property_range(prop, 1, 32);
  RNA_def_property_ui_range(prop, 1, 32, 2, -1);
  RNA_def_property_int_default(prop, 4);
  RNA_def_property_ui_text(prop, "Samples", "Number of Blur Samples");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "use_glow_under", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", FX_GLOW_USE_ALPHA);
  RNA_def_property_ui_text(
      prop, "Glow Under", "Glow only areas with alpha (not supported with Regular blend mode)");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "rotation", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "rotation");
  RNA_def_property_range(prop, -FLT_MAX, FLT_MAX);
  RNA_def_property_ui_text(prop, "Rotation", "Rotation of the effect");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  /* blend mode */
  prop = RNA_def_property(srna, "blend_mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_sdna(prop, NULL, "blend_mode");
  RNA_def_property_enum_items(prop, rna_enum_glow_blend_modes_items);
  RNA_def_property_ui_text(prop, "Blend Mode", "Blend mode");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  RNA_define_lib_overridable(false);
}

static void rna_def_shader_fx_swirl(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "ShaderFxSwirl", "ShaderFx");
  RNA_def_struct_ui_text(srna, "Swirl Effect", "Swirl effect");
  RNA_def_struct_sdna(srna, "SwirlShaderFxData");
  RNA_def_struct_ui_icon(srna, ICON_SHADERFX);

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "radius", PROP_INT, PROP_PIXEL);
  RNA_def_property_int_sdna(prop, NULL, "radius");
  RNA_def_property_range(prop, 0, SHRT_MAX);
  RNA_def_property_ui_text(prop, "Radius", "Radius to apply");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "angle", PROP_FLOAT, PROP_ANGLE);
  RNA_def_property_float_sdna(prop, NULL, "angle");
  RNA_def_property_range(prop, DEG2RAD(-5 * 360), DEG2RAD(5 * 360));
  RNA_def_property_ui_range(prop, DEG2RAD(-5 * 360), DEG2RAD(5 * 360), 5, 2);
  RNA_def_property_ui_text(prop, "Angle", "Angle of rotation");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "use_transparent", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", FX_SWIRL_MAKE_TRANSPARENT);
  RNA_def_property_ui_text(prop, "Transparent", "Make image transparent outside of radius");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "object", PROP_POINTER, PROP_NONE);
  RNA_def_property_ui_text(prop, "Object", "Object to determine center location");
  RNA_def_property_pointer_funcs(prop, NULL, "rna_SwirlShaderFx_object_set", NULL, NULL);
  RNA_def_property_flag(prop, PROP_EDITABLE | PROP_ID_SELF_CHECK);
  RNA_def_property_update(prop, 0, "rna_ShaderFx_dependency_update");

  RNA_define_lib_overridable(false);
}

static void rna_def_shader_fx_flip(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "ShaderFxFlip", "ShaderFx");
  RNA_def_struct_ui_text(srna, "Flip Effect", "Flip effect");
  RNA_def_struct_sdna(srna, "FlipShaderFxData");
  RNA_def_struct_ui_icon(srna, ICON_SHADERFX);

  RNA_define_lib_overridable(true);

  prop = RNA_def_property(srna, "use_flip_x", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", FX_FLIP_HORIZONTAL);
  RNA_def_property_ui_text(prop, "Horizontal", "Flip image horizontally");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  prop = RNA_def_property(srna, "use_flip_y", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "flag", FX_FLIP_VERTICAL);
  RNA_def_property_ui_text(prop, "Vertical", "Flip image vertically");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");

  RNA_define_lib_overridable(false);
}

void RNA_def_shader_fx(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  /* data */
  srna = RNA_def_struct(brna, "ShaderFx", NULL);
  RNA_def_struct_ui_text(srna, "ShaderFx", "Effect affecting the grease pencil object");
  RNA_def_struct_refine_func(srna, "rna_ShaderFx_refine");
  RNA_def_struct_path_func(srna, "rna_ShaderFx_path");
  RNA_def_struct_sdna(srna, "ShaderFxData");

  /* strings */
  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop, NULL, NULL, "rna_ShaderFx_name_set");
  RNA_def_property_ui_text(prop, "Name", "Effect name");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX | NA_RENAME, NULL);
  RNA_def_struct_name_property(srna, prop);

  /* enums */
  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_enum_sdna(prop, NULL, "type");
  RNA_def_property_enum_items(prop, rna_enum_object_shaderfx_type_items);
  RNA_def_property_ui_text(prop, "Type", "");
  RNA_def_property_translation_context(prop, BLT_I18NCONTEXT_ID_ID); /* Abused, for "Light"... */

  /* flags */
  prop = RNA_def_property(srna, "show_viewport", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "mode", eShaderFxMode_Realtime);
  RNA_def_property_ui_text(prop, "Realtime", "Display effect in viewport");
  RNA_def_property_flag(prop, PROP_LIB_EXCEPTION);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");
  RNA_def_property_ui_icon(prop, ICON_RESTRICT_VIEW_ON, 1);

  prop = RNA_def_property(srna, "show_render", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "mode", eShaderFxMode_Render);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Render", "Use effect during render");
  RNA_def_property_ui_icon(prop, ICON_RESTRICT_RENDER_ON, 1);
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, NULL);

  prop = RNA_def_property(srna, "show_in_editmode", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_sdna(prop, NULL, "mode", eShaderFxMode_Editmode);
  RNA_def_property_ui_text(prop, "Edit Mode", "Display effect in Edit mode");
  RNA_def_property_update(prop, NC_OBJECT | ND_SHADERFX, "rna_ShaderFx_update");
  RNA_def_property_ui_icon(prop, ICON_EDITMODE_HLT, 0);

  prop = RNA_def_property(srna, "show_expanded", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_flag(prop, PROP_NO_DEG_UPDATE);
  RNA_def_property_boolean_sdna(prop, NULL, "ui_expand_flag", 0);
  RNA_def_property_override_flag(prop, PROPOVERRIDE_OVERRIDABLE_LIBRARY);
  RNA_def_property_ui_text(prop, "Expanded", "Set effect expansion in the user interface");
  RNA_def_property_ui_icon(prop, ICON_DISCLOSURE_TRI_RIGHT, 1);

  /* types */
  rna_def_shader_fx_blur(brna);
  rna_def_shader_fx_colorize(brna);
  rna_def_shader_fx_wave(brna);
  rna_def_shader_fx_pixel(brna);
  rna_def_shader_fx_rim(brna);
  rna_def_shader_fx_shadow(brna);
  rna_def_shader_fx_glow(brna);
  rna_def_shader_fx_swirl(brna);
  rna_def_shader_fx_flip(brna);
}

#endif
