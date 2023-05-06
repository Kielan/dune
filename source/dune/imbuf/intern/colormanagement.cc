#include "imbuf_colormanagement.h"
#include "imbuf_colormanagement_intern.h"

#include <math.h>
#include <string.h>

#include "types_color.h"
#include "types_image.h"
#include "types_movieclip.h"
#include "types_scene.h"
#include "types_space.h"

#include "imbuf_filetype.h"
#include "imbuf_filter.h"
#include "imbuf.h"
#include "imbuf_types.h"
#include "imbuf_metadata.h"
#include "imbuf_moviecache.h"

#include "mem_guardedalloc.h"

#include "lib_dunelib.h"
#include "lib_math.h"
#include "lib_math_color.h"
#include "lib_rect.h"
#include "lib_string.h"
#include "lib_task.h"
#include "lib_threads.h"

#include "dune_appdir.h"
#include "dune_colortools.h"
#include "dune_context.h"
#include "dune_image.h"
#include "dune_image_format.h"
#include "dune_main.h"

#include "api_define.h"

#include "seq_iterator.h"

#include <ocio_capi.h>

/* -------------------------------------------------------------------- */
/** Global declarations **/

#define DISPLAY_BUFFER_CHANNELS 4

/* ** list of all supported color spaces, displays and views */
static char global_role_data[MAX_COLORSPACE_NAME];
static char global_role_scene_linear[MAX_COLORSPACE_NAME];
static char global_role_color_picking[MAX_COLORSPACE_NAME];
static char global_role_texture_painting[MAX_COLORSPACE_NAME];
static char global_role_default_byte[MAX_COLORSPACE_NAME];
static char global_role_default_float[MAX_COLORSPACE_NAME];
static char global_role_default_sequencer[MAX_COLORSPACE_NAME];

static ListBase global_colorspaces = {nullptr, nullptr};
static ListBase global_displays = {nullptr, nullptr};
static ListBase global_views = {nullptr, nullptr};
static ListBase global_looks = {nullptr, nullptr};

static int global_tot_colorspace = 0;
static int global_tot_display = 0;
static int global_tot_view = 0;
static int global_tot_looks = 0;

/* Luma coefficients and XYZ to RGB to be initialized by OCIO. */

float imbuf_luma_coefficients[3] = {0.0f};
float imbuf_scene_linear_to_xyz[3][3] = {{0.0f}};
float imbuf_xyz_to_scene_linear[3][3] = {{0.0f}};
float imbuf_scene_linear_to_rec709[3][3] = {{0.0f}};
float imbuf_rec709_to_scene_linear[3][3] = {{0.0f}};
float imbuf_scene_linear_to_aces[3][3] = {{0.0f}};
float imbuf_aces_to_scene_linear[3][3] = {{0.0f}};

/* lock used by pre-cached processors getters, so processor wouldn't
 * be created several times
 * LOCK_COLORMANAGE can not be used since this mutex could be needed to
 * be locked before pre-cached processor are creating
 */
static pthread_mutex_t processor_lock = LIB_MUTEX_INITIALIZER;

typedef struct ColormanageProcessor {
  OCIO_ConstCPUProcessorRcPtr *cpu_processor;
  CurveMapping *curve_mapping;
  bool is_data_result;
} ColormanageProcessor;

static struct global_gpu_state {
  /* GPU shader currently bound. */
  bool gpu_shader_bound;

  /* Curve mapping. */
  CurveMapping *curve_mapping, *orig_curve_mapping;
  bool use_curve_mapping;
  int curve_mapping_timestamp;
  OCIO_CurveMappingSettings curve_mapping_settings;
} global_gpu_state = {false};

static struct global_color_picking_state {
  /* Cached processor for color picking conversion. */
  OCIO_ConstCPUProcessorRcPtr *cpu_processor_to;
  OCIO_ConstCPUProcessorRcPtr *cpu_processor_from;
  bool failed;
} global_color_picking_state = {nullptr};

/* -------------------------------------------------------------------- */
/** Color Managed Cache **/

/**
 * Cache Implementation Notes
 * ==========================
 *
 * All color management cache stuff is stored in two properties of
 * image buffers:
 *
 *   1. display_buffer_flags
 *
 *      This is a bit field which used to mark calculated transformations
 *      for particular image buffer. Index inside of this array means index
 *      of a color managed display. Element with given index matches view
 *      transformations applied for a given display. So if bit B of array
 *      element B is set to 1, this means display buffer with display index
 *      of A and view transform of B was ever calculated for this imbuf.
 *
 *      In contrast with indices in global lists of displays and views this
 *      indices are 0-based, not 1-based. This is needed to save some bytes
 *      of memory.
 *
 *   2. colormanage_cache
 *
 *      This is a pointer to a structure which holds all data which is
 *      needed for color management cache to work.
 *
 *      It contains two parts:
 *        - data
 *        - moviecache
 *
 *      Data field is used to store additional information about cached
 *      buffers which affects on whether cached buffer could be used.
 *      This data can't go to cache key because changes in this data
 *      shouldn't lead extra buffers adding to cache, it shall
 *      invalidate cached images.
 *
 *      Currently such a data contains only exposure and gamma, but
 *      would likely extended further.
 *
 *      data field is not null only for elements of cache, not used for
 *      original image buffers.
 *
 *      Color management cache is using generic MovieCache implementation
 *      to make it easier to deal with memory limitation.
 *
 *      Currently color management is using the same memory limitation
 *      pool as sequencer and clip editor are using which means color
 *      managed buffers would be removed from the cache as soon as new
 *      frames are loading for the movie clip and there's no space in
 *      cache.
 *
 *      Every image buffer has got own movie cache instance, which
 *      means keys for color managed buffers could be really simple
 *      and look up in this cache would be fast and independent from
 *      overall amount of color managed images.
 */

/* NOTE: ColormanageCacheViewSettings and ColormanageCacheDisplaySettings are
 *       quite the same as ColorManagedViewSettings and ColorManageDisplaySettings
 *       but they holds indexes of all transformations and color spaces, not
 *       their names.
 *
 *       This helps avoid extra colorspace / display / view lookup without
 *       requiring to pass all variables which affects on display buffer
 *       to color management cache system and keeps calls small and nice.
 */
typedef struct ColormanageCacheViewSettings {
  int flag;
  int look;
  int view;
  float exposure;
  float gamma;
  float dither;
  CurveMapping *curve_mapping;
} ColormanageCacheViewSettings;

typedef struct ColormanageCacheDisplaySettings {
  int display;
} ColormanageCacheDisplaySettings;

typedef struct ColormanageCacheKey {
  int view;    /* view transformation used for display buffer */
  int display; /* display device name */
} ColormanageCacheKey;

typedef struct ColormanageCacheData {
  int flag;                    /* view flags of cached buffer */
  int look;                    /* Additional artistic transform. */
  float exposure;              /* exposure value cached buffer is calculated with */
  float gamma;                 /* gamma value cached buffer is calculated with */
  float dither;                /* dither value cached buffer is calculated with */
  CurveMapping *curve_mapping; /* curve mapping used for cached buffer */
  int curve_mapping_timestamp; /* time stamp of curve mapping used for cached buffer */
} ColormanageCacheData;

typedef struct ColormanageCache {
  struct MovieCache *moviecache;
  ColormanageCacheData *data;
} ColormanageCache;

static struct MovieCache *colormanage_moviecache_get(const ImBuf *ibuf)
{
  if (!ibuf->colormanage_cache) {
    return nullptr;
  }

  return ibuf->colormanage_cache->moviecache;
}

static ColormanageCacheData *colormanage_cachedata_get(const ImBuf *ibuf)
{
  if (!ibuf->colormanage_cache) {
    return nullptr;
  }

  return ibuf->colormanage_cache->data;
}

static uint colormanage_hashhash(const void *key_v)
{
  const ColormanageCacheKey *key = static_cast<const ColormanageCacheKey *>(key_v);

  uint rval = (key->display << 16) | (key->view % 0xffff);

  return rval;
}

static bool colormanage_hashcmp(const void *av, const void *bv)
{
  const ColormanageCacheKey *a = static_cast<const ColormanageCacheKey *>(av);
  const ColormanageCacheKey *b = static_cast<const ColormanageCacheKey *>(bv);

  return ((a->view != b->view) || (a->display != b->display));
}

static struct MovieCache *colormanage_moviecache_ensure(ImBuf *ibuf)
{
  if (!ibuf->colormanage_cache) {
    ibuf->colormanage_cache = mem_cnew<ColormanageCache>("imbuf colormanage cache");
  }

  if (!ibuf->colormanage_cache->moviecache) {
    struct MovieCache *moviecache;

    moviecache = imbuf_moviecache_create("colormanage cache",
                                       sizeof(ColormanageCacheKey),
                                       colormanage_hashhash,
                                       colormanage_hashcmp);

    ibuf->colormanage_cache->moviecache = moviecache;
  }

  return ibuf->colormanage_cache->moviecache;
}

static void colormanage_cachedata_set(ImBuf *ibuf, ColormanageCacheData *data)
{
  if (!ibuf->colormanage_cache) {
    ibuf->colormanage_cache = mem_cnew<ColormanageCache>("imbuf colormanage cache");
  }

  ibuf->colormanage_cache->data = data;
}

static void colormanage_view_settings_to_cache(ImBuf *ibuf,
                                               ColormanageCacheViewSettings *cache_view_settings,
                                               const ColorManagedViewSettings *view_settings)
{
  int look = imbuf_colormanagement_look_get_named_index(view_settings->look);
  int view = imbuf_colormanagement_view_get_named_index(view_settings->view_transform);

  cache_view_settings->look = look;
  cache_view_settings->view = view;
  cache_view_settings->exposure = view_settings->exposure;
  cache_view_settings->gamma = view_settings->gamma;
  cache_view_settings->dither = ibuf->dither;
  cache_view_settings->flag = view_settings->flag;
  cache_view_settings->curve_mapping = view_settings->curve_mapping;
}

static void colormanage_display_settings_to_cache(
    ColormanageCacheDisplaySettings *cache_display_settings,
    const ColorManagedDisplaySettings *display_settings)
{
  int display = imbuf_colormanagement_display_get_named_index(display_settings->display_device);

  cache_display_settings->display = display;
}

static void colormanage_settings_to_key(ColormanageCacheKey *key,
                                        const ColormanageCacheViewSettings *view_settings,
                                        const ColormanageCacheDisplaySettings *display_settings)
{
  key->view = view_settings->view;
  key->display = display_settings->display;
}

static ImBuf *colormanage_cache_get_ibuf(ImBuf *ibuf,
                                         ColormanageCacheKey *key,
                                         void **cache_handle)
{
  ImBuf *cache_ibuf;
  struct MovieCache *moviecache = colormanage_moviecache_get(ibuf);

  if (!moviecache) {
    /* If there's no moviecache it means no color management was applied
     * on given image buffer before. */
    return nullptr;
  }

  *cache_handle = nullptr;

  cache_ibuf = imbuf_moviecache_get(moviecache, key, nullptr);

  *cache_handle = cache_ibuf;

  return cache_ibuf;
}

static uchar *colormanage_cache_get(ImBuf *ibuf,
                                    const ColormanageCacheViewSettings *view_settings,
                                    const ColormanageCacheDisplaySettings *display_settings,
                                    void **cache_handle)
{
  ColormanageCacheKey key;
  ImBuf *cache_ibuf;
  int view_flag = 1 << (view_settings->view - 1);
  CurveMapping *curve_mapping = view_settings->curve_mapping;
  int curve_mapping_timestamp = curve_mapping ? curve_mapping->changed_timestamp : 0;

  colormanage_settings_to_key(&key, view_settings, display_settings);

  /* check whether image was marked as dirty for requested transform */
  if ((ibuf->display_buffer_flags[display_settings->display - 1] & view_flag) == 0) {
    return nullptr;
  }

  cache_ibuf = colormanage_cache_get_ibuf(ibuf, &key, cache_handle);

  if (cache_ibuf) {
    ColormanageCacheData *cache_data;

    lib_assert(cache_ibuf->x == ibuf->x && cache_ibuf->y == ibuf->y);

    /* only buffers with different color space conversions are being stored
     * in cache separately. buffer which were used only different exposure/gamma
     * are re-suing the same cached buffer
     *
     * check here which exposure/gamma/curve was used for cached buffer and if they're
     * different from requested buffer should be re-generated
     */
    cache_data = colormanage_cachedata_get(cache_ibuf);

    if (cache_data->look != view_settings->look ||
        cache_data->exposure != view_settings->exposure ||
        cache_data->gamma != view_settings->gamma || cache_data->dither != view_settings->dither ||
        cache_data->flag != view_settings->flag || cache_data->curve_mapping != curve_mapping ||
        cache_data->curve_mapping_timestamp != curve_mapping_timestamp)
    {
      *cache_handle = nullptr;

      lib_freeImBuf(cache_ibuf);

      return nullptr;
    }

    return (uchar *)cache_ibuf->rect;
  }

  return nullptr;
}

static uint colormanage_hashhash(const void *key_v)
{
  const ColormanageCacheKey *key = static_cast<const ColormanageCacheKey *>(key_v);

  uint rval = (key->display << 16) | (key->view % 0xffff);

  return rval;
}

static bool colormanage_hashcmp(const void *av, const void *bv)
{
  const ColormanageCacheKey *a = static_cast<const ColormanageCacheKey *>(av);
  const ColormanageCacheKey *b = static_cast<const ColormanageCacheKey *>(bv);

  return ((a->view != b->view) || (a->display != b->display));
}

static struct MovieCache *colormanage_moviecache_ensure(ImBuf *ibuf)
{
  if (!ibuf->colormanage_cache) {
    ibuf->colormanage_cache = MEM_cnew<ColormanageCache>("imbuf colormanage cache");
  }

  if (!ibuf->colormanage_cache->moviecache) {
    struct MovieCache *moviecache;

    moviecache = imbuf_moviecache_create("colormanage cache",
                                       sizeof(ColormanageCacheKey),
                                       colormanage_hashhash,
                                       colormanage_hashcmp);

    ibuf->colormanage_cache->moviecache = moviecache;
  }

  return ibuf->colormanage_cache->moviecache;
}

static void colormanage_cachedata_set(ImBuf *ibuf, ColormanageCacheData *data)
{
  if (!ibuf->colormanage_cache) {
    ibuf->colormanage_cache = mem_cnew<ColormanageCache>("imbuf colormanage cache");
  }

  ibuf->colormanage_cache->data = data;
}

static void colormanage_cache_put(ImBuf *ibuf,
                                  const ColormanageCacheViewSettings *view_settings,
                                  const ColormanageCacheDisplaySettings *display_settings,
                                  uchar *display_buffer,
                                  void **cache_handle)
{
  ColormanageCacheKey key;
  ImBuf *cache_ibuf;
  ColormanageCacheData *cache_data;
  int view_flag = 1 << (view_settings->view - 1);
  struct MovieCache *moviecache = colormanage_moviecache_ensure(ibuf);
  CurveMapping *curve_mapping = view_settings->curve_mapping;
  int curve_mapping_timestamp = curve_mapping ? curve_mapping->changed_timestamp : 0;

  colormanage_settings_to_key(&key, view_settings, display_settings);

  /* mark display buffer as valid */
  ibuf->display_buffer_flags[display_settings->display - 1] |= view_flag;

  /* buffer itself */
  cache_ibuf = imbuf_allocImBuf(ibuf->x, ibuf->y, ibuf->planes, 0);
  cache_ibuf->rect = (uint *)display_buffer;

  cache_ibuf->mall |= IB_rect;
  cache_ibuf->flags |= IB_rect;

  /* Store data which is needed to check whether cached buffer
   * could be used for color managed display settings. */
  cache_data = mem_cnew<ColormanageCacheData>("color manage cache imbuf data");
  cache_data->look = view_settings->look;
  cache_data->exposure = view_settings->exposure;
  cache_data->gamma = view_settings->gamma;
  cache_data->dither = view_settings->dither;
  cache_data->flag = view_settings->flag;
  cache_data->curve_mapping = curve_mapping;
  cache_data->curve_mapping_timestamp = curve_mapping_timestamp;

  colormanage_cachedata_set(cache_ibuf, cache_data);

  *cache_handle = cache_ibuf;

  imbuf_moviecache_put(moviecache, &key, cache_ibuf);
}

static void colormanage_cache_handle_release(void *cache_handle)
{
  ImBuf *cache_ibuf = static_cast<ImBuf *>(cache_handle);

  imbuf_freeImBuf(cache_ibuf);
}

/* -------------------------------------------------------------------- */
/** Initialization / De-initialization **/

static void colormanage_role_color_space_name_get(OCIO_ConstConfigRcPtr *config,
                                                  char *colorspace_name,
                                                  const char *role,
                                                  const char *backup_role)
{
  OCIO_ConstColorSpaceRcPtr *ociocs;

  ociocs = OCIO_configGetColorSpace(config, role);

  if (!ociocs && backup_role) {
    ociocs = OCIO_configGetColorSpace(config, backup_role);
  }

  if (ociocs) {
    const char *name = OCIO_colorSpaceGetName(ociocs);

    /* assume function was called with buffer properly allocated to MAX_COLORSPACE_NAME chars */
    lib_strncpy(colorspace_name, name, MAX_COLORSPACE_NAME);
    OCIO_colorSpaceRelease(ociocs);
  }
  else {
    printf("Color management: Error could not find role %s role.\n", role);
  }
}

static void colormanage_load_config(OCIO_ConstConfigRcPtr *config)
{
  int tot_colorspace, tot_display, tot_display_view, tot_looks;
  int index, viewindex, viewindex2;
  const char *name;

  /* get roles */
  colormanage_role_color_space_name_get(config, global_role_data, OCIO_ROLE_DATA, nullptr);
  colormanage_role_color_space_name_get(
      config, global_role_scene_linear, OCIO_ROLE_SCENE_LINEAR, nullptr);
  colormanage_role_color_space_name_get(
      config, global_role_color_picking, OCIO_ROLE_COLOR_PICKING, nullptr);
  colormanage_role_color_space_name_get(
      config, global_role_texture_painting, OCIO_ROLE_TEXTURE_PAINT, nullptr);
  colormanage_role_color_space_name_get(
      config, global_role_default_sequencer, OCIO_ROLE_DEFAULT_SEQUENCER, OCIO_ROLE_SCENE_LINEAR);
  colormanage_role_color_space_name_get(
      config, global_role_default_byte, OCIO_ROLE_DEFAULT_BYTE, OCIO_ROLE_TEXTURE_PAINT);
  colormanage_role_color_space_name_get(
      config, global_role_default_float, OCIO_ROLE_DEFAULT_FLOAT, OCIO_ROLE_SCENE_LINEAR);

  /* load colorspaces */
  tot_colorspace = OCIO_configGetNumColorSpaces(config);
  for (index = 0; index < tot_colorspace; index++) {
    OCIO_ConstColorSpaceRcPtr *ocio_colorspace;
    const char *description;
    bool is_invertible, is_data;

    name = OCIO_configGetColorSpaceNameByIndex(config, index);

    ocio_colorspace = OCIO_configGetColorSpace(config, name);
    description = OCIO_colorSpaceGetDescription(ocio_colorspace);
    is_invertible = OCIO_colorSpaceIsInvertible(ocio_colorspace);
    is_data = OCIO_colorSpaceIsData(ocio_colorspace);

    ColorSpace *colorspace = colormanage_colorspace_add(name, description, is_invertible, is_data);

    colorspace->num_aliases = OCIO_colorSpaceGetNumAliases(ocio_colorspace);
    if (colorspace->num_aliases > 0) {
      colorspace->aliases = static_cast<char(*)[MAX_COLORSPACE_NAME]>(MEM_callocN(
          sizeof(*colorspace->aliases) * colorspace->num_aliases, "ColorSpace aliases"));
      for (int i = 0; i < colorspace->num_aliases; i++) {
        lib_strncpy(colorspace->aliases[i],
                    OCIO_colorSpaceGetAlias(ocio_colorspace, i),
                    MAX_COLORSPACE_NAME);
      }
    }

    OCIO_colorSpaceRelease(ocio_colorspace);
  }

  /* load displays */
  viewindex2 = 0;
  tot_display = OCIO_configGetNumDisplays(config);

  for (index = 0; index < tot_display; index++) {
    const char *displayname;
    ColorManagedDisplay *display;

    displayname = OCIO_configGetDisplay(config, index);

    display = colormanage_display_add(displayname);

    /* load views */
    tot_display_view = OCIO_configGetNumViews(config, displayname);
    for (viewindex = 0; viewindex < tot_display_view; viewindex++, viewindex2++) {
      const char *viewname;
      ColorManagedView *view;
      LinkData *display_view;

      viewname = OCIO_configGetView(config, displayname, viewindex);

      /* first check if view transform with given name was already loaded */
      view = colormanage_view_get_named(viewname);

      if (!view) {
        view = colormanage_view_add(viewname);
      }

      display_view = lib_genericNodeN(view);

      lib_addtail(&display->views, display_view);
    }
  }

  global_tot_display = tot_display;

  /* load looks */
  tot_looks = OCIO_configGetNumLooks(config);
  colormanage_look_add("None", "", true);
  for (index = 0; index < tot_looks; index++) {
    OCIO_ConstLookRcPtr *ocio_look;
    const char *process_space;

    name = OCIO_configGetLookNameByIndex(config, index);
    ocio_look = OCIO_configGetLook(config, name);
    process_space = OCIO_lookGetProcessSpace(ocio_look);
    OCIO_lookRelease(ocio_look);

    colormanage_look_add(name, process_space, false);
  }

  /* Load luminance coefficients. */
  OCIO_configGetDefaultLumaCoefs(config, imbuf_luma_coefficients);

  /* Load standard color spaces. */
  OCIO_configGetXYZtoSceneLinear(config, imbuf_xyz_to_scene_linear);
  invert_m3_m3(imbuf_scene_linear_to_xyz, imbuf_xyz_to_scene_linear);

  mul_m3_m3m3(imbuf_scene_linear_to_rec709, OCIO_XYZ_TO_REC709, imbuf_scene_linear_to_xyz);
  invert_m3_m3(imbuf_rec709_to_scene_linear, imbuf_scene_linear_to_rec709);

  mul_m3_m3m3(imbuf_aces_to_scene_linear, imbuf_xyz_to_scene_linear, OCIO_ACES_TO_XYZ);
  invert_m3_m3(imbuf_scene_linear_to_aces, imbuf_aces_to_scene_linear);
}

static void colormanage_free_config(void)
{
  ColorSpace *colorspace;
  ColorManagedDisplay *display;

  /* free color spaces */
  colorspace = static_cast<ColorSpace *>(global_colorspaces.first);
  while (colorspace) {
    ColorSpace *colorspace_next = colorspace->next;

    /* Free precomputed processors. */
    if (colorspace->to_scene_linear) {
      OCIO_cpuProcessorRelease((OCIO_ConstCPUProcessorRcPtr *)colorspace->to_scene_linear);
    }
    if (colorspace->from_scene_linear) {
      OCIO_cpuProcessorRelease((OCIO_ConstCPUProcessorRcPtr *)colorspace->from_scene_linear);
    }

    /* free color space itself */
    MEM_SAFE_FREE(colorspace->aliases);
    mem_freen(colorspace);

    colorspace = colorspace_next;
  }
  lib_listbase_clear(&global_colorspaces);
  global_tot_colorspace = 0;

  /* free displays */
  display = static_cast<ColorManagedDisplay *>(global_displays.first);
  while (display) {
    ColorManagedDisplay *display_next = display->next;

    /* free precomputer processors */
    if (display->to_scene_linear) {
      OCIO_cpuProcessorRelease((OCIO_ConstCPUProcessorRcPtr *)display->to_scene_linear);
    }
    if (display->from_scene_linear) {
      OCIO_cpuProcessorRelease((OCIO_ConstCPUProcessorRcPtr *)display->from_scene_linear);
    }

    /* free list of views */
    lib_freelistn(&display->views);

    mem_freen(display);
    display = display_next;
  }
  lib_listbase_clear(&global_displays);
  global_tot_display = 0;

  /* free views */
  lib_freelistn(&global_views);
  global_tot_view = 0;

  /* free looks */
  lib_freelistn(&global_looks);
  global_tot_looks = 0;

  OCIO_exit();
}

void colormanagement_init(void)
{
  const char *ocio_env;
  const char *configdir;
  char configfile[FILE_MAX];
  OCIO_ConstConfigRcPtr *config = nullptr;

  OCIO_init();

  ocio_env = lib_getenv("OCIO");

  if (ocio_env && ocio_env[0] != '\0') {
    config = OCIO_configCreateFromEnv();
    if (config != nullptr) {
      printf("Color management: Using %s as a configuration file\n", ocio_env);
    }
  }

  if (config == nullptr) {
    configdir = dune_appdir_folder_id(DUNE_DATAFILES, "colormanagement");

    if (configdir) {
      lib_path_join(configfile, sizeof(configfile), configdir, BCM_CONFIG_FILE);

      config = OCIO_configCreateFromFile(configfile);
    }
  }

  if (config == nullptr) {
    printf("Color management: using fallback mode for management\n");

    config = OCIO_configCreateFallback();
  }

  if (config) {
    OCIO_setCurrentConfig(config);

    colormanage_load_config(config);

    OCIO_configRelease(config);
  }

  /* If there are no valid display/views, use fallback mode. */
  if (global_tot_display == 0 || global_tot_view == 0) {
    printf("Color management: no displays/views in the config, using fallback mode instead\n");

    /* Free old config. */
    colormanage_free_config();

    /* Initialize fallback config. */
    config = OCIO_configCreateFallback();
    colormanage_load_config(config);
  }

  lib_init_srgb_conversion();
}
void colormanagement_exit(void)
{
  OCIO_gpuCacheFree();

  if (global_gpu_state.curve_mapping) {
    dune_curvemapping_free(global_gpu_state.curve_mapping);
  }

  if (global_gpu_state.curve_mapping_settings.lut) {
    mem_freen(global_gpu_state.curve_mapping_settings.lut);
  }

  if (global_color_picking_state.cpu_processor_to) {
    OCIO_cpuProcessorRelease(global_color_picking_state.cpu_processor_to);
  }

  if (global_color_picking_state.cpu_processor_from) {
    OCIO_cpuProcessorRelease(global_color_picking_state.cpu_processor_from);
  }

  memset(&global_gpu_state, 0, sizeof(global_gpu_state));
  memset(&global_color_picking_state, 0, sizeof(global_color_picking_state));

  colormanage_free_config();
}

/* -------------------------------------------------------------------- */
/** Internal functions **/

static bool colormanage_compatible_look(ColorManagedLook *look, const char *view_name)
{
  if (look->is_noop) {
    return true;
  }

  /* Skip looks only relevant to specific view transforms. */
  return (look->view[0] == 0 || (view_name && STREQ(look->view, view_name)));
}

static bool colormanage_use_look(const char *look, const char *view_name)
{
  ColorManagedLook *look_descr = colormanage_look_get_named(look);
  return (look_descr->is_noop == false && colormanage_compatible_look(look_descr, view_name));
}

void colormanage_cache_free(ImBuf *ibuf)
{
  MEM_SAFE_FREE(ibuf->display_buffer_flags);

  if (ibuf->colormanage_cache) {
    ColormanageCacheData *cache_data = colormanage_cachedata_get(ibuf);
    struct MovieCache *moviecache = colormanage_moviecache_get(ibuf);

    if (cache_data) {
      mem_freen(cache_data);
    }

    if (moviecache) {
      imbuf_moviecache_free(moviecache);
    }

    mem_freen(ibuf->colormanage_cache);

    ibuf->colormanage_cache = nullptr;
  }
}

void imbuf_colormanagement_display_settings_from_ctx(
    const Ctx *C,
    ColorManagedViewSettings **r_view_settings,
    ColorManagedDisplaySettings **r_display_settings)
{
  Scene *scene = ctx_data_scene(C);
  SpaceImage *sima = ctx_wm_space_image(C);

  *r_view_settings = &scene->view_settings;
  *r_display_settings = &scene->display_settings;

  if (sima && sima->image) {
    if ((sima->image->flag & IMA_VIEW_AS_RENDER) == 0) {
      *r_view_settings = nullptr;
    }
  }
}

static const char *get_display_colorspace_name(const ColorManagedViewSettings *view_settings,
                                               const ColorManagedDisplaySettings *display_settings)
{
  OCIO_ConstConfigRcPtr *config = OCIO_getCurrentConfig();

  const char *display = display_settings->display_device;
  const char *view = view_settings->view_transform;
  const char *colorspace_name;

  colorspace_name = OCIO_configGetDisplayColorSpaceName(config, display, view);

  OCIO_configRelease(config);

  return colorspace_name;
}

static ColorSpace *display_transform_get_colorspace(
    const ColorManagedViewSettings *view_settings,
    const ColorManagedDisplaySettings *display_settings)
{
  const char *colorspace_name = get_display_colorspace_name(view_settings, display_settings);

  if (colorspace_name) {
    return colormanage_colorspace_get_named(colorspace_name);
  }

  return nullptr;
}

static OCIO_ConstCPUProcessorRcPtr *create_display_buffer_processor(const char *look,
                                                                    const char *view_transform,
                                                                    const char *display,
                                                                    float exposure,
                                                                    float gamma,
                                                                    const char *from_colorspace)
{
  OCIO_ConstConfigRcPtr *config = OCIO_getCurrentConfig();
  const bool use_look = colormanage_use_look(look, view_transform);
  const float scale = (exposure == 0.0f) ? 1.0f : powf(2.0f, exposure);
  const float exponent = (gamma == 1.0f) ? 1.0f : 1.0f / max_ff(FLT_EPSILON, gamma);

  OCIO_ConstProcessorRcPtr *processor = OCIO_createDisplayProcessor(config,
                                                                    from_colorspace,
                                                                    view_transform,
                                                                    display,
                                                                    (use_look) ? look : "",
                                                                    scale,
                                                                    exponent,
                                                                    false);

  OCIO_configRelease(config);

  if (processor == nullptr) {
    return nullptr;
  }

  OCIO_ConstCPUProcessorRcPtr *cpu_processor = OCIO_processorGetCPUProcessor(processor);
  OCIO_processorRelease(processor);

  return cpu_processor;
}

static OCIO_ConstProcessorRcPtr *create_colorspace_transform_processor(const char *from_colorspace,
                                                                       const char *to_colorspace)
{
  OCIO_ConstConfigRcPtr *config = OCIO_getCurrentConfig();
  OCIO_ConstProcessorRcPtr *processor;

  processor = OCIO_configGetProcessorWithNames(config, from_colorspace, to_colorspace);

  OCIO_configRelease(config);

  return processor;
}

static OCIO_ConstCPUProcessorRcPtr *colorspace_to_scene_linear_cpu_processor(
    ColorSpace *colorspace)
{
  if (colorspace->to_scene_linear == nullptr) {
    lib_mutex_lock(&processor_lock);

    if (colorspace->to_scene_linear == nullptr) {
      OCIO_ConstProcessorRcPtr *processor = create_colorspace_transform_processor(
          colorspace->name, global_role_scene_linear);

      if (processor != nullptr) {
        colorspace->to_scene_linear = (OCIO_ConstCPUProcessorRcPtr *)OCIO_processorGetCPUProcessor(
            processor);
        OCIO_processorRelease(processor);
      }
    }

    lib_mutex_unlock(&processor_lock);
  }

  return (OCIO_ConstCPUProcessorRcPtr *)colorspace->to_scene_linear;
}

static OCIO_ConstCPUProcessorRcPtr *colorspace_from_scene_linear_cpu_processor(
    ColorSpace *colorspace)
{
  if (colorspace->from_scene_linear == nullptr) {
    lib_mutex_lock(&processor_lock);

    if (colorspace->from_scene_linear == nullptr) {
      OCIO_ConstProcessorRcPtr *processor = create_colorspace_transform_processor(
          global_role_scene_linear, colorspace->name);

      if (processor != nullptr) {
        colorspace->from_scene_linear = (OCIO_ConstCPUProcessorRcPtr *)
            OCIO_processorGetCPUProcessor(processor);
        OCIO_processorRelease(processor);
      }
    }

    lib_mutex_unlock(&processor_lock);
  }

  return (OCIO_ConstCPUProcessorRcPtr *)colorspace->from_scene_linear;
}

static OCIO_ConstCPUProcessorRcPtr *display_from_scene_linear_processor(
    ColorManagedDisplay *display)
{
  if (display->from_scene_linear == nullptr) {
    lib_mutex_lock(&processor_lock);

    if (display->from_scene_linear == nullptr) {
      const char *view_name = colormanage_view_get_default_name(display);
      OCIO_ConstConfigRcPtr *config = OCIO_getCurrentConfig();
      OCIO_ConstProcessorRcPtr *processor = nullptr;

      if (view_name && config) {
        processor = OCIO_createDisplayProcessor(config,
                                                global_role_scene_linear,
                                                view_name,
                                                display->name,
                                                nullptr,
                                                1.0f,
                                                1.0f,
                                                false);

        OCIO_configRelease(config);
      }

      if (processor != nullptr) {
        display->from_scene_linear = (OCIO_ConstCPUProcessorRcPtr *)OCIO_processorGetCPUProcessor(
            processor);
        OCIO_processorRelease(processor);
      }
    }

    lib_mutex_unlock(&processor_lock);
  }

  return (OCIO_ConstCPUProcessorRcPtr *)display->from_scene_linear;
}

static OCIO_ConstCPUProcessorRcPtr *display_to_scene_linear_processor(ColorManagedDisplay *display)
{
  if (display->to_scene_linear == nullptr) {
    lib_mutex_lock(&processor_lock);

    if (display->to_scene_linear == nullptr) {
      const char *view_name = colormanage_view_get_default_name(display);
      OCIO_ConstConfigRcPtr *config = OCIO_getCurrentConfig();
      OCIO_ConstProcessorRcPtr *processor = nullptr;

      if (view_name && config) {
        processor = OCIO_createDisplayProcessor(
            config, global_role_scene_linear, view_name, display->name, nullptr, 1.0f, 1.0f, true);

        OCIO_configRelease(config);
      }

      if (processor != nullptr) {
        display->to_scene_linear = (OCIO_ConstCPUProcessorRcPtr *)OCIO_processorGetCPUProcessor(
            processor);
        OCIO_processorRelease(processor);
      }
    }

    lib_mutex_unlock(&processor_lock);
  }

  return (OCIO_ConstCPUProcessorRcPtr *)display->to_scene_linear;
}

void imbuf_colormanagement_init_default_view_settings(
    ColorManagedViewSettings *view_settings, const ColorManagedDisplaySettings *display_settings)
{
  /* First, try use "Standard" view transform of the requested device. */
  ColorManagedView *default_view = colormanage_view_get_named_for_display(
      display_settings->display_device, "Standard");
  /* If that fails, we fall back to the default view transform of the display
   * as per OCIO configuration. */
  if (default_view == nullptr) {
    ColorManagedDisplay *display = colormanage_display_get_named(display_settings->display_device);
    if (display != nullptr) {
      default_view = colormanage_view_get_default(display);
    }
  }
  if (default_view != nullptr) {
    lib_strncpy(
        view_settings->view_transform, default_view->name, sizeof(view_settings->view_transform));
  }
  else {
    view_settings->view_transform[0] = '\0';
  }
  /* TODO: Find a way to safely/reliable un-hardcode this. */
  lib_strncpy(view_settings->look, "None", sizeof(view_settings->look));
  /* Initialize rest of the settings. */
  view_settings->flag = 0;
  view_settings->gamma = 1.0f;
  view_settings->exposure = 0.0f;
  view_settings->curve_mapping = nullptr;
}
static void curve_mapping_apply_pixel(CurveMapping *curve_mapping, float *pixel, int channels)
{
  if (channels == 1) {
    pixel[0] = dune_curvemap_evaluateF(curve_mapping, curve_mapping->cm, pixel[0]);
  }
  else if (channels == 2) {
    pixel[0] = dune_curvemap_evaluateF(curve_mapping, curve_mapping->cm, pixel[0]);
    pixel[1] = dune_curvemap_evaluateF(curve_mapping, curve_mapping->cm, pixel[1]);
  }
  else {
    dune_curvemapping_evaluate_premulRGBF(curve_mapping, pixel, pixel);
  }
}

void colorspace_set_default_role(char *colorspace, int size, int role)
{
  if (colorspace && colorspace[0] == '\0') {
    const char *role_colorspace;

    role_colorspace = imbuf_colormanagement_role_colorspace_name_get(role);

    lib_strncpy(colorspace, role_colorspace, size);
  }
}

void colormanage_imbuf_set_default_spaces(ImBuf *ibuf)
{
  ibuf->rect_colorspace = colormanage_colorspace_get_named(global_role_default_byte);
}

void colormanage_imbuf_make_linear(ImBuf *ibuf, const char *from_colorspace)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(from_colorspace);

  if (colorspace && colorspace->is_data) {
    ibuf->colormanage_flag |= IMB_COLORMANAGE_IS_DATA;
    return;
  }

  if (ibuf->rect_float) {
    const char *to_colorspace = global_role_scene_linear;
    const bool predivide = imbuf_alpha_affects_rgb(ibuf);

    if (ibuf->rect) {
      imb_freerectImBuf(ibuf);
    }

    imbuf_colormanagement_transform(ibuf->rect_float,
                                  ibuf->x,
                                  ibuf->y,
                                  ibuf->channels,
                                  from_colorspace,
                                  to_colorspace,
                                  predivide);
  }
}

/* -------------------------------------------------------------------- */
/** Generic Functions **/

static void colormanage_check_display_settings(ColorManagedDisplaySettings *display_settings,
                                               const char *what,
                                               const ColorManagedDisplay *default_display)
{
  if (display_settings->display_device[0] == '\0') {
    lib_strncpy(display_settings->display_device,
                default_display->name,
                sizeof(display_settings->display_device));
  }
  else {
    ColorManagedDisplay *display = colormanage_display_get_named(display_settings->display_device);

    if (!display) {
      printf(
          "Color management: display \"%s\" used by %s not found, setting to default (\"%s\").\n",
          display_settings->display_device,
          what,
          default_display->name);

      lib_strncpy(display_settings->display_device,
                  default_display->name,
                  sizeof(display_settings->display_device));
    }
  }
}

static void colormanage_check_view_settings(ColorManagedDisplaySettings *display_settings,
                                            ColorManagedViewSettings *view_settings,
                                            const char *what)
{
  ColorManagedDisplay *display;
  ColorManagedView *default_view = nullptr;
  ColorManagedLook *default_look = (ColorManagedLook *)global_looks.first;

  if (view_settings->view_transform[0] == '\0') {
    display = colormanage_display_get_named(display_settings->display_device);

    if (display) {
      default_view = colormanage_view_get_default(display);
    }

    if (default_view) {
      lib_strncpy(view_settings->view_transform,
                  default_view->name,
                  sizeof(view_settings->view_transform));
    }
  }
  else {
    ColorManagedView *view = colormanage_view_get_named(view_settings->view_transform);

    if (!view) {
      display = colormanage_display_get_named(display_settings->display_device);

      if (display) {
        default_view = colormanage_view_get_default(display);
      }

      if (default_view) {
        printf("Color management: %s view \"%s\" not found, setting default \"%s\".\n",
               what,
               view_settings->view_transform,
               default_view->name);

        lib_strncpy(view_settings->view_transform,
                    default_view->name,
                    sizeof(view_settings->view_transform));
      }
    }
  }

  if (view_settings->look[0] == '\0') {
    lib_strncpy(view_settings->look, default_look->name, sizeof(view_settings->look));
  }
  else {
    ColorManagedLook *look = colormanage_look_get_named(view_settings->look);
    if (look == nullptr) {
      printf("Color management: %s look \"%s\" not found, setting default \"%s\".\n",
             what,
             view_settings->look,
             default_look->name);

      lib_strncpy(view_settings->look, default_look->name, sizeof(view_settings->look));
    }
  }

  /* OCIO_TODO: move to do_versions() */
  if (view_settings->exposure == 0.0f && view_settings->gamma == 0.0f) {
    view_settings->exposure = 0.0f;
    view_settings->gamma = 1.0f;
  }
}

static void colormanage_check_colorspace_settings(
    ColorManagedColorspaceSettings *colorspace_settings, const char *what)
{
  if (colorspace_settings->name[0] == '\0') {
    /* pass */
  }
  else {
    ColorSpace *colorspace = colormanage_colorspace_get_named(colorspace_settings->name);

    if (!colorspace) {
      printf("Color management: %s colorspace \"%s\" not found, will use default instead.\n",
             what,
             colorspace_settings->name);

      BLI_strncpy(colorspace_settings->name, "", sizeof(colorspace_settings->name));
    }
  }

  (void)what;
}

static bool seq_callback(Sequence *seq, void * /*user_data*/)
{
  if (seq->strip) {
    colormanage_check_colorspace_settings(&seq->strip->colorspace_settings, "sequencer strip");
  }
  return true;
}

void IMB_colormanagement_check_file_config(Main *bmain)
{
  ColorManagedDisplay *default_display = colormanage_display_get_default();
  if (!default_display) {
    /* happens when OCIO configuration is incorrect */
    return;
  }

  LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
    ColorManagedColorspaceSettings *sequencer_colorspace_settings;

    /* check scene color management settings */
    colormanage_check_display_settings(&scene->display_settings, "scene", default_display);
    colormanage_check_view_settings(&scene->display_settings, &scene->view_settings, "scene");

    sequencer_colorspace_settings = &scene->sequencer_colorspace_settings;

    colormanage_check_colorspace_settings(sequencer_colorspace_settings, "sequencer");

    if (sequencer_colorspace_settings->name[0] == '\0') {
      lib_strncpy(
          sequencer_colorspace_settings->name, global_role_default_sequencer, MAX_COLORSPACE_NAME);
    }

    /* check sequencer strip input color space settings */
    if (scene->ed != nullptr) {
      seq_for_each_cb(&scene->ed->seqbase, seq_cb, nullptr);
    }
  }

  /* ** check input color space settings ** */

  LISTBASE_FOREACH (Image *, image, &main->images) {
    colormanage_check_colorspace_settings(&image->colorspace_settings, "image");
  }

  LISTBASE_FOREACH (MovieClip *, clip, &main->movieclips) {
    colormanage_check_colorspace_settings(&clip->colorspace_settings, "clip");
  }
}

void IMB_colormanagement_validate_settings(const ColorManagedDisplaySettings *display_settings,
                                           ColorManagedViewSettings *view_settings)
{
  ColorManagedDisplay *display = colormanage_display_get_named(display_settings->display_device);
  ColorManagedView *default_view = colormanage_view_get_default(display);

  bool found = false;
  LISTBASE_FOREACH (LinkData *, view_link, &display->views) {
    ColorManagedView *view = static_cast<ColorManagedView *>(view_link->data);

    if (STREQ(view->name, view_settings->view_transform)) {
      found = true;
      break;
    }
  }

  if (!found && default_view) {
    BLI_strncpy(
        view_settings->view_transform, default_view->name, sizeof(view_settings->view_transform));
  }
}

const char *IMB_colormanagement_role_colorspace_name_get(int role)
{
  switch (role) {
    case COLOR_ROLE_DATA:
      return global_role_data;
    case COLOR_ROLE_SCENE_LINEAR:
      return global_role_scene_linear;
    case COLOR_ROLE_COLOR_PICKING:
      return global_role_color_picking;
    case COLOR_ROLE_TEXTURE_PAINTING:
      return global_role_texture_painting;
    case COLOR_ROLE_DEFAULT_SEQUENCER:
      return global_role_default_sequencer;
    case COLOR_ROLE_DEFAULT_FLOAT:
      return global_role_default_float;
    case COLOR_ROLE_DEFAULT_BYTE:
      return global_role_default_byte;
    default:
      printf("Unknown role was passed to %s\n", __func__);
      BLI_assert(0);
      break;
  }

  return nullptr;
}

void IMB_colormanagement_check_is_data(ImBuf *ibuf, const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);

  if (colorspace && colorspace->is_data) {
    ibuf->colormanage_flag |= IMB_COLORMANAGE_IS_DATA;
  }
  else {
    ibuf->colormanage_flag &= ~IMB_COLORMANAGE_IS_DATA;
  }
}

void imbuf_colormanagegent_copy_settings(ImBuf *ibuf_src, ImBuf *ibuf_dst)
{
  imbuf_colormanagement_assign_rect_colorspace(ibuf_dst,
                                             imbuf_colormanagement_get_rect_colorspace(ibuf_src));
  imbuf_colormanagement_assign_float_colorspace(ibuf_dst,
                                              imbuf_colormanagement_get_float_colorspace(ibuf_src));
  if (ibuf_src->flags & IB_alphamode_premul) {
    ibuf_dst->flags |= IB_alphamode_premul;
  }
  else if (ibuf_src->flags & IB_alphamode_channel_packed) {
    ibuf_dst->flags |= IB_alphamode_channel_packed;
  }
  else if (ibuf_src->flags & IB_alphamode_ignore) {
    ibuf_dst->flags |= IB_alphamode_ignore;
  }
}

void imbuf_colormanagement_assign_float_colorspace(ImBuf *ibuf, const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);

  ibuf->float_colorspace = colorspace;

  if (colorspace && colorspace->is_data) {
    ibuf->colormanage_flag |= IMB_COLORMANAGE_IS_DATA;
  }
  else {
    ibuf->colormanage_flag &= ~IMB_COLORMANAGE_IS_DATA;
  }
}

void imbuf_colormanagement_assign_rect_colorspace(ImBuf *ibuf, const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);

  ibuf->rect_colorspace = colorspace;

  if (colorspace && colorspace->is_data) {
    ibuf->colormanage_flag |= IMB_COLORMANAGE_IS_DATA;
  }
  else {
    ibuf->colormanage_flag &= ~IMB_COLORMANAGE_IS_DATA;
  }
}

const char *IMB_colormanagement_get_float_colorspace(ImBuf *ibuf)
{
  if (ibuf->float_colorspace) {
    return ibuf->float_colorspace->name;
  }

  return imbuf_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR);
}

const char *imbuf_colormanagement_get_rect_colorspace(ImBuf *ibuf)
{
  if (ibuf->rect_colorspace) {
    return ibuf->rect_colorspace->name;
  }

  return imbuf_colormanagement_role_colorspace_name_get(COLOR_ROLE_DEFAULT_BYTE);
}

bool imbuf_colormanagement_space_is_data(ColorSpace *colorspace)
{
  return (colorspace && colorspace->is_data);
}

static void colormanage_ensure_srgb_scene_linear_info(ColorSpace *colorspace)
{
  if (!colorspace->info.cached) {
    OCIO_ConstConfigRcPtr *config = OCIO_getCurrentConfig();
    OCIO_ConstColorSpaceRcPtr *ocio_colorspace = OCIO_configGetColorSpace(config,
                                                                          colorspace->name);

    bool is_scene_linear, is_srgb;
    OCIO_colorSpaceIsBuiltin(config, ocio_colorspace, &is_scene_linear, &is_srgb);

    OCIO_colorSpaceRelease(ocio_colorspace);
    OCIO_configRelease(config);

    colorspace->info.is_scene_linear = is_scene_linear;
    colorspace->info.is_srgb = is_srgb;
    colorspace->info.cached = true;
  }
}

bool imbuf_colormanagement_space_is_scene_linear(ColorSpace *colorspace)
{
  colormanage_ensure_srgb_scene_linear_info(colorspace);
  return (colorspace && colorspace->info.is_scene_linear);
}

bool imbuf_colormanagement_space_is_srgb(ColorSpace *colorspace)
{
  colormanage_ensure_srgb_scene_linear_info(colorspace);
  return (colorspace && colorspace->info.is_srgb);
}

bool imbuf_colormanagement_space_name_is_data(const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);
  return (colorspace && colorspace->is_data);
}

bool imbuf_colormanagement_space_name_is_scene_linear(const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);
  return (colorspace && IMB_colormanagement_space_is_scene_linear(colorspace));
}

bool imbuf_colormanagement_space_name_is_srgb(const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);
  return (colorspace && imbuf_colormanagement_space_is_srgb(colorspace));
}

const float *imbuf_colormanagement_get_xyz_to_scene_linear(void)
{
  return &imbuf_xyz_to_scene_linear[0][0];
}

/* -------------------------------------------------------------------- */
/** Threaded Display Buffer Transform Routines **/

typedef struct DisplayBufferThread {
  ColormanageProcessor *cm_processor;

  const float *buffer;
  uchar *byte_buffer;

  float *display_buffer;
  uchar *display_buffer_byte;

  int width;
  int start_line;
  int tot_line;

  int channels;
  float dither;
  bool is_data;
  bool predivide;

  const char *byte_colorspace;
  const char *float_colorspace;
} DisplayBufferThread;

typedef struct DisplayBufferInitData {
  ImBuf *ibuf;
  ColormanageProcessor *cm_processor;
  const float *buffer;
  uchar *byte_buffer;

  float *display_buffer;
  uchar *display_buffer_byte;

  int width;

  const char *byte_colorspace;
  const char *float_colorspace;
} DisplayBufferInitData;

static void display_buffer_init_handle(void *handle_v,
                                       int start_line,
                                       int tot_line,
                                       void *init_data_v)
{
  DisplayBufferThread *handle = (DisplayBufferThread *)handle_v;
  DisplayBufferInitData *init_data = (DisplayBufferInitData *)init_data_v;
  ImBuf *ibuf = init_data->ibuf;

  int channels = ibuf->channels;
  float dither = ibuf->dither;
  bool is_data = (ibuf->colormanage_flag & IMB_COLORMANAGE_IS_DATA) != 0;

  size_t offset = size_t(channels) * start_line * ibuf->x;
  size_t display_buffer_byte_offset = size_t(DISPLAY_BUFFER_CHANNELS) * start_line * ibuf->x;

  memset(handle, 0, sizeof(DisplayBufferThread));

  handle->cm_processor = init_data->cm_processor;

  if (init_data->buffer) {
    handle->buffer = init_data->buffer + offset;
  }

  if (init_data->byte_buffer) {
    handle->byte_buffer = init_data->byte_buffer + offset;
  }

  if (init_data->display_buffer) {
    handle->display_buffer = init_data->display_buffer + offset;
  }

  if (init_data->display_buffer_byte) {
    handle->display_buffer_byte = init_data->display_buffer_byte + display_buffer_byte_offset;
  }

  handle->width = ibuf->x;

  handle->start_line = start_line;
  handle->tot_line = tot_line;

  handle->channels = channels;
  handle->dither = dither;
  handle->is_data = is_data;
  handle->predivide = IMB_alpha_affects_rgb(ibuf);

  handle->byte_colorspace = init_data->byte_colorspace;
  handle->float_colorspace = init_data->float_colorspace;
}

static void display_buffer_apply_get_linear_buffer(DisplayBufferThread *handle,
                                                   int height,
                                                   float *linear_buffer,
                                                   bool *is_straight_alpha)
{
  int channels = handle->channels;
  int width = handle->width;

  size_t buffer_size = size_t(channels) * width * height;

  bool is_data = handle->is_data;
  bool is_data_display = handle->cm_processor->is_data_result;
  bool predivide = handle->predivide;

  if (!handle->buffer) {
    uchar *byte_buffer = handle->byte_buffer;

    const char *from_colorspace = handle->byte_colorspace;
    const char *to_colorspace = global_role_scene_linear;

    float *fp;
    uchar *cp;
    const size_t i_last = size_t(width) * height;
    size_t i;

    /* first convert byte buffer to float, keep in image space */
    for (i = 0, fp = linear_buffer, cp = byte_buffer; i != i_last;
         i++, fp += channels, cp += channels) {
      if (channels == 3) {
        rgb_uchar_to_float(fp, cp);
      }
      else if (channels == 4) {
        rgba_uchar_to_float(fp, cp);
      }
      else {
        lib_assert_msg(0, "Buffers of 3 or 4 channels are only supported here");
      }
    }

    if (!is_data && !is_data_display) {
      /* convert float buffer to scene linear space */
      imbuf_colormanagement_transform(
          linear_buffer, width, height, channels, from_colorspace, to_colorspace, false);
    }

    *is_straight_alpha = true;
  }
  else if (handle->float_colorspace) {
    /* currently float is non-linear only in sequencer, which is working
     * in its own color space even to handle float buffers.
     * This color space is the same for byte and float images.
     * Need to convert float buffer to linear space before applying display transform
     */

    const char *from_colorspace = handle->float_colorspace;
    const char *to_colorspace = global_role_scene_linear;

    memcpy(linear_buffer, handle->buffer, buffer_size * sizeof(float));

    if (!is_data && !is_data_display) {
      imbuf_colormanagement_transform(
          linear_buffer, width, height, channels, from_colorspace, to_colorspace, predivide);
    }

    *is_straight_alpha = false;
  }
  else {
    /* some processors would want to modify float original buffer
     * before converting it into display byte buffer, so we need to
     * make sure original's ImBuf buffers wouldn't be modified by
     * using duplicated buffer here
     */

    memcpy(linear_buffer, handle->buffer, buffer_size * sizeof(float));

    *is_straight_alpha = false;
  }
}
static void curve_mapping_apply_pixel(CurveMapping *curve_mapping, float *pixel, int channels)
{
  if (channels == 1) {
    pixel[0] = BKE_curvemap_evaluateF(curve_mapping, curve_mapping->cm, pixel[0]);
  }
  else if (channels == 2) {
    pixel[0] = BKE_curvemap_evaluateF(curve_mapping, curve_mapping->cm, pixel[0]);
    pixel[1] = BKE_curvemap_evaluateF(curve_mapping, curve_mapping->cm, pixel[1]);
  }
  else {
    BKE_curvemapping_evaluate_premulRGBF(curve_mapping, pixel, pixel);
  }
}

void colorspace_set_default_role(char *colorspace, int size, int role)
{
  if (colorspace && colorspace[0] == '\0') {
    const char *role_colorspace;

    role_colorspace = IMB_colormanagement_role_colorspace_name_get(role);

    BLI_strncpy(colorspace, role_colorspace, size);
  }
}

void colormanage_imbuf_set_default_spaces(ImBuf *ibuf)
{
  ibuf->rect_colorspace = colormanage_colorspace_get_named(global_role_default_byte);
}

void colormanage_imbuf_make_linear(ImBuf *ibuf, const char *from_colorspace)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(from_colorspace);

  if (colorspace && colorspace->is_data) {
    ibuf->colormanage_flag |= IMB_COLORMANAGE_IS_DATA;
    return;
  }

  if (ibuf->rect_float) {
    const char *to_colorspace = global_role_scene_linear;
    const bool predivide = IMB_alpha_affects_rgb(ibuf);

    if (ibuf->rect) {
      imb_freerectImBuf(ibuf);
    }

    IMB_colormanagement_transform(ibuf->rect_float,
                                  ibuf->x,
                                  ibuf->y,
                                  ibuf->channels,
                                  from_colorspace,
                                  to_colorspace,
                                  predivide);
  }
}

/* -------------------------------------------------------------------- */
/** Generic Functions **/

static void colormanage_check_display_settings(ColorManagedDisplaySettings *display_settings,
                                               const char *what,
                                               const ColorManagedDisplay *default_display)
{
  if (display_settings->display_device[0] == '\0') {
    BLI_strncpy(display_settings->display_device,
                default_display->name,
                sizeof(display_settings->display_device));
  }
  else {
    ColorManagedDisplay *display = colormanage_display_get_named(display_settings->display_device);

    if (!display) {
      printf(
          "Color management: display \"%s\" used by %s not found, setting to default (\"%s\").\n",
          display_settings->display_device,
          what,
          default_display->name);

      BLI_strncpy(display_settings->display_device,
                  default_display->name,
                  sizeof(display_settings->display_device));
    }
  }
}

static void colormanage_check_view_settings(ColorManagedDisplaySettings *display_settings,
                                            ColorManagedViewSettings *view_settings,
                                            const char *what)
{
  ColorManagedDisplay *display;
  ColorManagedView *default_view = nullptr;
  ColorManagedLook *default_look = (ColorManagedLook *)global_looks.first;

  if (view_settings->view_transform[0] == '\0') {
    display = colormanage_display_get_named(display_settings->display_device);

    if (display) {
      default_view = colormanage_view_get_default(display);
    }

    if (default_view) {
      BLI_strncpy(view_settings->view_transform,
                  default_view->name,
                  sizeof(view_settings->view_transform));
    }
  }
  else {
    ColorManagedView *view = colormanage_view_get_named(view_settings->view_transform);

    if (!view) {
      display = colormanage_display_get_named(display_settings->display_device);

      if (display) {
        default_view = colormanage_view_get_default(display);
      }

      if (default_view) {
        printf("Color management: %s view \"%s\" not found, setting default \"%s\".\n",
               what,
               view_settings->view_transform,
               default_view->name);

        BLI_strncpy(view_settings->view_transform,
                    default_view->name,
                    sizeof(view_settings->view_transform));
      }
    }
  }

  if (view_settings->look[0] == '\0') {
    BLI_strncpy(view_settings->look, default_look->name, sizeof(view_settings->look));
  }
  else {
    ColorManagedLook *look = colormanage_look_get_named(view_settings->look);
    if (look == nullptr) {
      printf("Color management: %s look \"%s\" not found, setting default \"%s\".\n",
             what,
             view_settings->look,
             default_look->name);

      BLI_strncpy(view_settings->look, default_look->name, sizeof(view_settings->look));
    }
  }

  /* OCIO_TODO: move to do_versions() */
  if (view_settings->exposure == 0.0f && view_settings->gamma == 0.0f) {
    view_settings->exposure = 0.0f;
    view_settings->gamma = 1.0f;
  }
}

static void colormanage_check_colorspace_settings(
    ColorManagedColorspaceSettings *colorspace_settings, const char *what)
{
  if (colorspace_settings->name[0] == '\0') {
    /* pass */
  }
  else {
    ColorSpace *colorspace = colormanage_colorspace_get_named(colorspace_settings->name);

    if (!colorspace) {
      printf("Color management: %s colorspace \"%s\" not found, will use default instead.\n",
             what,
             colorspace_settings->name);

      BLI_strncpy(colorspace_settings->name, "", sizeof(colorspace_settings->name));
    }
  }

  (void)what;
}

static bool seq_callback(Sequence *seq, void * /*user_data*/)
{
  if (seq->strip) {
    colormanage_check_colorspace_settings(&seq->strip->colorspace_settings, "sequencer strip");
  }
  return true;
}

void IMB_colormanagement_check_file_config(Main *bmain)
{
  ColorManagedDisplay *default_display = colormanage_display_get_default();
  if (!default_display) {
    /* happens when OCIO configuration is incorrect */
    return;
  }

  LISTBASE_FOREACH (Scene *, scene, &bmain->scenes) {
    ColorManagedColorspaceSettings *sequencer_colorspace_settings;

    /* check scene color management settings */
    colormanage_check_display_settings(&scene->display_settings, "scene", default_display);
    colormanage_check_view_settings(&scene->display_settings, &scene->view_settings, "scene");

    sequencer_colorspace_settings = &scene->sequencer_colorspace_settings;

    colormanage_check_colorspace_settings(sequencer_colorspace_settings, "sequencer");

    if (sequencer_colorspace_settings->name[0] == '\0') {
      BLI_strncpy(
          sequencer_colorspace_settings->name, global_role_default_sequencer, MAX_COLORSPACE_NAME);
    }

    /* check sequencer strip input color space settings */
    if (scene->ed != nullptr) {
      SEQ_for_each_callback(&scene->ed->seqbase, seq_callback, nullptr);
    }
  }

  /* ** check input color space settings ** */

  LISTBASE_FOREACH (Image *, image, &bmain->images) {
    colormanage_check_colorspace_settings(&image->colorspace_settings, "image");
  }

  LISTBASE_FOREACH (MovieClip *, clip, &bmain->movieclips) {
    colormanage_check_colorspace_settings(&clip->colorspace_settings, "clip");
  }
}

void IMB_colormanagement_validate_settings(const ColorManagedDisplaySettings *display_settings,
                                           ColorManagedViewSettings *view_settings)
{
  ColorManagedDisplay *display = colormanage_display_get_named(display_settings->display_device);
  ColorManagedView *default_view = colormanage_view_get_default(display);

  bool found = false;
  LISTBASE_FOREACH (LinkData *, view_link, &display->views) {
    ColorManagedView *view = static_cast<ColorManagedView *>(view_link->data);

    if (STREQ(view->name, view_settings->view_transform)) {
      found = true;
      break;
    }
  }

  if (!found && default_view) {
    BLI_strncpy(
        view_settings->view_transform, default_view->name, sizeof(view_settings->view_transform));
  }
}

const char *IMB_colormanagement_role_colorspace_name_get(int role)
{
  switch (role) {
    case COLOR_ROLE_DATA:
      return global_role_data;
    case COLOR_ROLE_SCENE_LINEAR:
      return global_role_scene_linear;
    case COLOR_ROLE_COLOR_PICKING:
      return global_role_color_picking;
    case COLOR_ROLE_TEXTURE_PAINTING:
      return global_role_texture_painting;
    case COLOR_ROLE_DEFAULT_SEQUENCER:
      return global_role_default_sequencer;
    case COLOR_ROLE_DEFAULT_FLOAT:
      return global_role_default_float;
    case COLOR_ROLE_DEFAULT_BYTE:
      return global_role_default_byte;
    default:
      printf("Unknown role was passed to %s\n", __func__);
      BLI_assert(0);
      break;
  }

  return nullptr;
}

void IMB_colormanagement_check_is_data(ImBuf *ibuf, const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);

  if (colorspace && colorspace->is_data) {
    ibuf->colormanage_flag |= IMB_COLORMANAGE_IS_DATA;
  }
  else {
    ibuf->colormanage_flag &= ~IMB_COLORMANAGE_IS_DATA;
  }
}

void IMB_colormanagegent_copy_settings(ImBuf *ibuf_src, ImBuf *ibuf_dst)
{
  IMB_colormanagement_assign_rect_colorspace(ibuf_dst,
                                             IMB_colormanagement_get_rect_colorspace(ibuf_src));
  IMB_colormanagement_assign_float_colorspace(ibuf_dst,
                                              IMB_colormanagement_get_float_colorspace(ibuf_src));
  if (ibuf_src->flags & IB_alphamode_premul) {
    ibuf_dst->flags |= IB_alphamode_premul;
  }
  else if (ibuf_src->flags & IB_alphamode_channel_packed) {
    ibuf_dst->flags |= IB_alphamode_channel_packed;
  }
  else if (ibuf_src->flags & IB_alphamode_ignore) {
    ibuf_dst->flags |= IB_alphamode_ignore;
  }
}

void IMB_colormanagement_assign_float_colorspace(ImBuf *ibuf, const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);

  ibuf->float_colorspace = colorspace;

  if (colorspace && colorspace->is_data) {
    ibuf->colormanage_flag |= IMB_COLORMANAGE_IS_DATA;
  }
  else {
    ibuf->colormanage_flag &= ~IMB_COLORMANAGE_IS_DATA;
  }
}

void IMB_colormanagement_assign_rect_colorspace(ImBuf *ibuf, const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);

  ibuf->rect_colorspace = colorspace;

  if (colorspace && colorspace->is_data) {
    ibuf->colormanage_flag |= IMB_COLORMANAGE_IS_DATA;
  }
  else {
    ibuf->colormanage_flag &= ~IMB_COLORMANAGE_IS_DATA;
  }
}

const char *IMB_colormanagement_get_float_colorspace(ImBuf *ibuf)
{
  if (ibuf->float_colorspace) {
    return ibuf->float_colorspace->name;
  }

  return IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_SCENE_LINEAR);
}

const char *IMB_colormanagement_get_rect_colorspace(ImBuf *ibuf)
{
  if (ibuf->rect_colorspace) {
    return ibuf->rect_colorspace->name;
  }

  return IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DEFAULT_BYTE);
}

bool IMB_colormanagement_space_is_data(ColorSpace *colorspace)
{
  return (colorspace && colorspace->is_data);
}

static void colormanage_ensure_srgb_scene_linear_info(ColorSpace *colorspace)
{
  if (!colorspace->info.cached) {
    OCIO_ConstConfigRcPtr *config = OCIO_getCurrentConfig();
    OCIO_ConstColorSpaceRcPtr *ocio_colorspace = OCIO_configGetColorSpace(config,
                                                                          colorspace->name);

    bool is_scene_linear, is_srgb;
    OCIO_colorSpaceIsBuiltin(config, ocio_colorspace, &is_scene_linear, &is_srgb);

    OCIO_colorSpaceRelease(ocio_colorspace);
    OCIO_configRelease(config);

    colorspace->info.is_scene_linear = is_scene_linear;
    colorspace->info.is_srgb = is_srgb;
    colorspace->info.cached = true;
  }
}

bool IMB_colormanagement_space_is_scene_linear(ColorSpace *colorspace)
{
  colormanage_ensure_srgb_scene_linear_info(colorspace);
  return (colorspace && colorspace->info.is_scene_linear);
}

bool IMB_colormanagement_space_is_srgb(ColorSpace *colorspace)
{
  colormanage_ensure_srgb_scene_linear_info(colorspace);
  return (colorspace && colorspace->info.is_srgb);
}

bool IMB_colormanagement_space_name_is_data(const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);
  return (colorspace && colorspace->is_data);
}

bool IMB_colormanagement_space_name_is_scene_linear(const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);
  return (colorspace && IMB_colormanagement_space_is_scene_linear(colorspace));
}

bool IMB_colormanagement_space_name_is_srgb(const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);
  return (colorspace && IMB_colormanagement_space_is_srgb(colorspace));
}

const float *IMB_colormanagement_get_xyz_to_scene_linear(void)
{
  return &imbuf_xyz_to_scene_linear[0][0];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Threaded Display Buffer Transform Routines
 * \{ */

typedef struct DisplayBufferThread {
  ColormanageProcessor *cm_processor;

  const float *buffer;
  uchar *byte_buffer;

  float *display_buffer;
  uchar *display_buffer_byte;

  int width;
  int start_line;
  int tot_line;

  int channels;
  float dither;
  bool is_data;
  bool predivide;

  const char *byte_colorspace;
  const char *float_colorspace;
} DisplayBufferThread;

typedef struct DisplayBufferInitData {
  ImBuf *ibuf;
  ColormanageProcessor *cm_processor;
  const float *buffer;
  uchar *byte_buffer;

  float *display_buffer;
  uchar *display_buffer_byte;

  int width;

  const char *byte_colorspace;
  const char *float_colorspace;
} DisplayBufferInitData;

static void display_buffer_init_handle(void *handle_v,
                                       int start_line,
                                       int tot_line,
                                       void *init_data_v)
{
  DisplayBufferThread *handle = (DisplayBufferThread *)handle_v;
  DisplayBufferInitData *init_data = (DisplayBufferInitData *)init_data_v;
  ImBuf *ibuf = init_data->ibuf;

  int channels = ibuf->channels;
  float dither = ibuf->dither;
  bool is_data = (ibuf->colormanage_flag & IMB_COLORMANAGE_IS_DATA) != 0;

  size_t offset = size_t(channels) * start_line * ibuf->x;
  size_t display_buffer_byte_offset = size_t(DISPLAY_BUFFER_CHANNELS) * start_line * ibuf->x;

  memset(handle, 0, sizeof(DisplayBufferThread));

  handle->cm_processor = init_data->cm_processor;

  if (init_data->buffer) {
    handle->buffer = init_data->buffer + offset;
  }

  if (init_data->byte_buffer) {
    handle->byte_buffer = init_data->byte_buffer + offset;
  }

  if (init_data->display_buffer) {
    handle->display_buffer = init_data->display_buffer + offset;
  }

  if (init_data->display_buffer_byte) {
    handle->display_buffer_byte = init_data->display_buffer_byte + display_buffer_byte_offset;
  }

  handle->width = ibuf->x;

  handle->start_line = start_line;
  handle->tot_line = tot_line;

  handle->channels = channels;
  handle->dither = dither;
  handle->is_data = is_data;
  handle->predivide = IMB_alpha_affects_rgb(ibuf);

  handle->byte_colorspace = init_data->byte_colorspace;
  handle->float_colorspace = init_data->float_colorspace;
}

static void display_buffer_apply_get_linear_buffer(DisplayBufferThread *handle,
                                                   int height,
                                                   float *linear_buffer,
                                                   bool *is_straight_alpha)
{
  int channels = handle->channels;
  int width = handle->width;

  size_t buffer_size = size_t(channels) * width * height;

  bool is_data = handle->is_data;
  bool is_data_display = handle->cm_processor->is_data_result;
  bool predivide = handle->predivide;

  if (!handle->buffer) {
    uchar *byte_buffer = handle->byte_buffer;

    const char *from_colorspace = handle->byte_colorspace;
    const char *to_colorspace = global_role_scene_linear;

    float *fp;
    uchar *cp;
    const size_t i_last = size_t(width) * height;
    size_t i;

    /* first convert byte buffer to float, keep in image space */
    for (i = 0, fp = linear_buffer, cp = byte_buffer; i != i_last;
         i++, fp += channels, cp += channels) {
      if (channels == 3) {
        rgb_uchar_to_float(fp, cp);
      }
      else if (channels == 4) {
        rgba_uchar_to_float(fp, cp);
      }
      else {
        BLI_assert_msg(0, "Buffers of 3 or 4 channels are only supported here");
      }
    }

    if (!is_data && !is_data_display) {
      /* convert float buffer to scene linear space */
      IMB_colormanagement_transform(
          linear_buffer, width, height, channels, from_colorspace, to_colorspace, false);
    }

    *is_straight_alpha = true;
  }
  else if (handle->float_colorspace) {
    /* currently float is non-linear only in sequencer, which is working
     * in its own color space even to handle float buffers.
     * This color space is the same for byte and float images.
     * Need to convert float buffer to linear space before applying display transform
     */

    const char *from_colorspace = handle->float_colorspace;
    const char *to_colorspace = global_role_scene_linear;

    memcpy(linear_buffer, handle->buffer, buffer_size * sizeof(float));

    if (!is_data && !is_data_display) {
      IMB_colormanagement_transform(
          linear_buffer, width, height, channels, from_colorspace, to_colorspace, predivide);
    }

    *is_straight_alpha = false;
  }
  else {
    /* some processors would want to modify float original buffer
     * before converting it into display byte buffer, so we need to
     * make sure original's ImBuf buffers wouldn't be modified by
     * using duplicated buffer here
     */

    memcpy(linear_buffer, handle->buffer, buffer_size * sizeof(float));

    *is_straight_alpha = false;
  }
}

static void *do_display_buffer_apply_thread(void *handle_v)
{
  DisplayBufferThread *handle = (DisplayBufferThread *)handle_v;
  ColormanageProcessor *cm_processor = handle->cm_processor;
  float *display_buffer = handle->display_buffer;
  uchar *display_buffer_byte = handle->display_buffer_byte;
  int channels = handle->channels;
  int width = handle->width;
  int height = handle->tot_line;
  float dither = handle->dither;
  bool is_data = handle->is_data;

  if (cm_processor == nullptr) {
    if (display_buffer_byte && display_buffer_byte != handle->byte_buffer) {
      IMB_buffer_byte_from_byte(display_buffer_byte,
                                handle->byte_buffer,
                                IB_PROFILE_SRGB,
                                IB_PROFILE_SRGB,
                                false,
                                width,
                                height,
                                width,
                                width);
    }

    if (display_buffer) {
      IMB_buffer_float_from_byte(display_buffer,
                                 handle->byte_buffer,
                                 IB_PROFILE_SRGB,
                                 IB_PROFILE_SRGB,
                                 false,
                                 width,
                                 height,
                                 width,
                                 width);
    }
  }
  else {
    bool is_straight_alpha;
    float *linear_buffer = static_cast<float *>(MEM_mallocN(
        size_t(channels) * width * height * sizeof(float), "color conversion linear buffer"));

    display_buffer_apply_get_linear_buffer(handle, height, linear_buffer, &is_straight_alpha);

    bool predivide = handle->predivide && (is_straight_alpha == false);

    if (is_data) {
      /* special case for data buffers - no color space conversions,
       * only generate byte buffers
       */
    }
    else {
      /* apply processor */
      IMB_colormanagement_processor_apply(
          cm_processor, linear_buffer, width, height, channels, predivide);
    }

    /* copy result to output buffers */
    if (display_buffer_byte) {
      /* do conversion */
      IMB_buffer_byte_from_float(display_buffer_byte,
                                 linear_buffer,
                                 channels,
                                 dither,
                                 IB_PROFILE_SRGB,
                                 IB_PROFILE_SRGB,
                                 predivide,
                                 width,
                                 height,
                                 width,
                                 width);
    }

    if (display_buffer) {
      memcpy(display_buffer, linear_buffer, size_t(width) * height * channels * sizeof(float));

      if (is_straight_alpha && channels == 4) {
        const size_t i_last = size_t(width) * height;
        size_t i;
        float *fp;

        for (i = 0, fp = display_buffer; i != i_last; i++, fp += channels) {
          straight_to_premul_v4(fp);
        }
      }
    }

    MEM_freeN(linear_buffer);
  }

  return nullptr;
}

static void display_buffer_apply_threaded(ImBuf *ibuf,
                                          const float *buffer,
                                          uchar *byte_buffer,
                                          float *display_buffer,
                                          uchar *display_buffer_byte,
                                          ColormanageProcessor *cm_processor)
{
  DisplayBufferInitData init_data;

  init_data.ibuf = ibuf;
  init_data.cm_processor = cm_processor;
  init_data.buffer = buffer;
  init_data.byte_buffer = byte_buffer;
  init_data.display_buffer = display_buffer;
  init_data.display_buffer_byte = display_buffer_byte;

  if (ibuf->rect_colorspace != nullptr) {
    init_data.byte_colorspace = ibuf->rect_colorspace->name;
  }
  else {
    /* happens for viewer images, which are not so simple to determine where to
     * set image buffer's color spaces
     */
    init_data.byte_colorspace = global_role_default_byte;
  }

  if (ibuf->float_colorspace != nullptr) {
    /* sequencer stores float buffers in non-linear space */
    init_data.float_colorspace = ibuf->float_colorspace->name;
  }
  else {
    init_data.float_colorspace = nullptr;
  }

  IMB_processor_apply_threaded(ibuf->y,
                               sizeof(DisplayBufferThread),
                               &init_data,
                               display_buffer_init_handle,
                               do_display_buffer_apply_thread);
}

static bool is_ibuf_rect_in_display_space(ImBuf *ibuf,
                                          const ColorManagedViewSettings *view_settings,
                                          const ColorManagedDisplaySettings *display_settings)
{
  if ((view_settings->flag & COLORMANAGE_VIEW_USE_CURVES) == 0 &&
      view_settings->exposure == 0.0f && view_settings->gamma == 1.0f)
  {
    const char *from_colorspace = ibuf->rect_colorspace->name;
    const char *to_colorspace = get_display_colorspace_name(view_settings, display_settings);
    ColorManagedLook *look_descr = colormanage_look_get_named(view_settings->look);
    if (look_descr != nullptr && !STREQ(look_descr->process_space, "")) {
      return false;
    }

    if (to_colorspace && STREQ(from_colorspace, to_colorspace)) {
      return true;
    }
  }

  return false;
}

static void colormanage_display_buffer_process_ex(
    ImBuf *ibuf,
    float *display_buffer,
    uchar *display_buffer_byte,
    const ColorManagedViewSettings *view_settings,
    const ColorManagedDisplaySettings *display_settings)
{
  ColormanageProcessor *cm_processor = nullptr;
  bool skip_transform = false;

  /* if we're going to transform byte buffer, check whether transformation would
   * happen to the same color space as byte buffer itself is
   * this would save byte -> float -> byte conversions making display buffer
   * computation noticeable faster
   */
  if (ibuf->rect_float == nullptr && ibuf->rect_colorspace) {
    skip_transform = is_ibuf_rect_in_display_space(ibuf, view_settings, display_settings);
  }

  if (skip_transform == false) {
    cm_processor = IMB_colormanagement_display_processor_new(view_settings, display_settings);
  }

  display_buffer_apply_threaded(ibuf,
                                ibuf->rect_float,
                                (uchar *)ibuf->rect,
                                display_buffer,
                                display_buffer_byte,
                                cm_processor);

  if (cm_processor) {
    IMB_colormanagement_processor_free(cm_processor);
  }
}

static void colormanage_display_buffer_process(ImBuf *ibuf,
                                               uchar *display_buffer,
                                               const ColorManagedViewSettings *view_settings,
                                               const ColorManagedDisplaySettings *display_settings)
{
  colormanage_display_buffer_process_ex(
      ibuf, nullptr, display_buffer, view_settings, display_settings);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Threaded Processor Transform Routines
 * \{ */

typedef struct ProcessorTransformThread {
  ColormanageProcessor *cm_processor;
  uchar *byte_buffer;
  float *float_buffer;
  int width;
  int start_line;
  int tot_line;
  int channels;
  bool predivide;
  bool float_from_byte;
} ProcessorTransformThread;

typedef struct ProcessorTransformInit {
  ColormanageProcessor *cm_processor;
  uchar *byte_buffer;
  float *float_buffer;
  int width;
  int height;
  int channels;
  bool predivide;
  bool float_from_byte;
} ProcessorTransformInitData;

static void processor_transform_init_handle(void *handle_v,
                                            int start_line,
                                            int tot_line,
                                            void *init_data_v)
{
  ProcessorTransformThread *handle = (ProcessorTransformThread *)handle_v;
  ProcessorTransformInitData *init_data = (ProcessorTransformInitData *)init_data_v;

  const int channels = init_data->channels;
  const int width = init_data->width;
  const bool predivide = init_data->predivide;
  const bool float_from_byte = init_data->float_from_byte;

  const size_t offset = size_t(channels) * start_line * width;

  memset(handle, 0, sizeof(ProcessorTransformThread));

  handle->cm_processor = init_data->cm_processor;

  if (init_data->byte_buffer != nullptr) {
    /* TODO(serge): Offset might be different for byte and float buffers. */
    handle->byte_buffer = init_data->byte_buffer + offset;
  }
  if (init_data->float_buffer != nullptr) {
    handle->float_buffer = init_data->float_buffer + offset;
  }

  handle->width = width;

  handle->start_line = start_line;
  handle->tot_line = tot_line;

  handle->channels = channels;
  handle->predivide = predivide;
  handle->float_from_byte = float_from_byte;
}

static void *do_processor_transform_thread(void *handle_v)
{
  ProcessorTransformThread *handle = (ProcessorTransformThread *)handle_v;
  uchar *byte_buffer = handle->byte_buffer;
  float *float_buffer = handle->float_buffer;
  const int channels = handle->channels;
  const int width = handle->width;
  const int height = handle->tot_line;
  const bool predivide = handle->predivide;
  const bool float_from_byte = handle->float_from_byte;

  if (float_from_byte) {
    IMB_buffer_float_from_byte(float_buffer,
                               byte_buffer,
                               IB_PROFILE_SRGB,
                               IB_PROFILE_SRGB,
                               false,
                               width,
                               height,
                               width,
                               width);
    IMB_colormanagement_processor_apply(
        handle->cm_processor, float_buffer, width, height, channels, predivide);
    IMB_premultiply_rect_float(float_buffer, 4, width, height);
  }
  else {
    if (byte_buffer != nullptr) {
      IMB_colormanagement_processor_apply_byte(
          handle->cm_processor, byte_buffer, width, height, channels);
    }
    if (float_buffer != nullptr) {
      IMB_colormanagement_processor_apply(
          handle->cm_processor, float_buffer, width, height, channels, predivide);
    }
  }

  return nullptr;
}

static void processor_transform_apply_threaded(uchar *byte_buffer,
                                               float *float_buffer,
                                               const int width,
                                               const int height,
                                               const int channels,
                                               ColormanageProcessor *cm_processor,
                                               const bool predivide,
                                               const bool float_from_byte)
{
  ProcessorTransformInitData init_data;

  init_data.cm_processor = cm_processor;
  init_data.byte_buffer = byte_buffer;
  init_data.float_buffer = float_buffer;
  init_data.width = width;
  init_data.height = height;
  init_data.channels = channels;
  init_data.predivide = predivide;
  init_data.float_from_byte = float_from_byte;

  IMB_processor_apply_threaded(height,
                               sizeof(ProcessorTransformThread),
                               &init_data,
                               processor_transform_init_handle,
                               do_processor_transform_thread);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Space Transformation Functions
 * \{ */

/* Convert the whole buffer from specified by name color space to another -
 * internal implementation. */
static void colormanagement_transform_ex(uchar *byte_buffer,
                                         float *float_buffer,
                                         int width,
                                         int height,
                                         int channels,
                                         const char *from_colorspace,
                                         const char *to_colorspace,
                                         bool predivide,
                                         bool do_threaded)
{
  ColormanageProcessor *cm_processor;

  if (from_colorspace[0] == '\0') {
    return;
  }

  if (STREQ(from_colorspace, to_colorspace)) {
    /* if source and destination color spaces are identical, skip
     * threading overhead and simply do nothing
     */
    return;
  }

  cm_processor = IMB_colormanagement_colorspace_processor_new(from_colorspace, to_colorspace);

  if (do_threaded) {
    processor_transform_apply_threaded(
        byte_buffer, float_buffer, width, height, channels, cm_processor, predivide, false);
  }
  else {
    if (byte_buffer != nullptr) {
      IMB_colormanagement_processor_apply_byte(cm_processor, byte_buffer, width, height, channels);
    }
    if (float_buffer != nullptr) {
      IMB_colormanagement_processor_apply(
          cm_processor, float_buffer, width, height, channels, predivide);
    }
  }

  IMB_colormanagement_processor_free(cm_processor);
}

void IMB_colormanagement_transform(float *buffer,
                                   int width,
                                   int height,
                                   int channels,
                                   const char *from_colorspace,
                                   const char *to_colorspace,
                                   bool predivide)
{
  colormanagement_transform_ex(
      nullptr, buffer, width, height, channels, from_colorspace, to_colorspace, predivide, false);
}

void IMB_colormanagement_transform_threaded(float *buffer,
                                            int width,
                                            int height,
                                            int channels,
                                            const char *from_colorspace,
                                            const char *to_colorspace,
                                            bool predivide)
{
  colormanagement_transform_ex(
      nullptr, buffer, width, height, channels, from_colorspace, to_colorspace, predivide, true);
}

void IMB_colormanagement_transform_byte(uchar *buffer,
                                        int width,
                                        int height,
                                        int channels,
                                        const char *from_colorspace,
                                        const char *to_colorspace)
{
  colormanagement_transform_ex(
      buffer, nullptr, width, height, channels, from_colorspace, to_colorspace, false, false);
}
void IMB_colormanagement_transform_byte_threaded(uchar *buffer,
                                                 int width,
                                                 int height,
                                                 int channels,
                                                 const char *from_colorspace,
                                                 const char *to_colorspace)
{
  colormanagement_transform_ex(
      buffer, nullptr, width, height, channels, from_colorspace, to_colorspace, false, true);
}

void IMB_colormanagement_transform_from_byte(float *float_buffer,
                                             uchar *byte_buffer,
                                             int width,
                                             int height,
                                             int channels,
                                             const char *from_colorspace,
                                             const char *to_colorspace)
{
  IMB_buffer_float_from_byte(float_buffer,
                             byte_buffer,
                             IB_PROFILE_SRGB,
                             IB_PROFILE_SRGB,
                             true,
                             width,
                             height,
                             width,
                             width);
  IMB_colormanagement_transform(
      float_buffer, width, height, channels, from_colorspace, to_colorspace, true);
}
void IMB_colormanagement_transform_from_byte_threaded(float *float_buffer,
                                                      uchar *byte_buffer,
                                                      int width,
                                                      int height,
                                                      int channels,
                                                      const char *from_colorspace,
                                                      const char *to_colorspace)
{
  ColormanageProcessor *cm_processor;
  if (from_colorspace == nullptr || from_colorspace[0] == '\0') {
    return;
  }
  if (STREQ(from_colorspace, to_colorspace)) {
    /* Because this function always takes a byte buffer and returns a float buffer, it must
     * always do byte-to-float conversion of some kind. To avoid threading overhead
     * IMB_buffer_float_from_byte is used when color spaces are identical. See #51002.
     */
    IMB_buffer_float_from_byte(float_buffer,
                               byte_buffer,
                               IB_PROFILE_SRGB,
                               IB_PROFILE_SRGB,
                               false,
                               width,
                               height,
                               width,
                               width);
    IMB_premultiply_rect_float(float_buffer, 4, width, height);
    return;
  }
  cm_processor = IMB_colormanagement_colorspace_processor_new(from_colorspace, to_colorspace);
  processor_transform_apply_threaded(
      byte_buffer, float_buffer, width, height, channels, cm_processor, false, true);
  IMB_colormanagement_processor_free(cm_processor);
}

void IMB_colormanagement_transform_v4(float pixel[4],
                                      const char *from_colorspace,
                                      const char *to_colorspace)
{
  ColormanageProcessor *cm_processor;

  if (from_colorspace[0] == '\0') {
    return;
  }

  if (STREQ(from_colorspace, to_colorspace)) {
    /* if source and destination color spaces are identical, skip
     * threading overhead and simply do nothing
     */
    return;
  }

  cm_processor = IMB_colormanagement_colorspace_processor_new(from_colorspace, to_colorspace);

  IMB_colormanagement_processor_apply_v4(cm_processor, pixel);

  IMB_colormanagement_processor_free(cm_processor);
}

void IMB_colormanagement_colorspace_to_scene_linear_v3(float pixel[3], ColorSpace *colorspace)
{
  OCIO_ConstCPUProcessorRcPtr *processor;

  if (!colorspace) {
    /* should never happen */
    printf("%s: perform conversion from unknown color space\n", __func__);
    return;
  }

  processor = colorspace_to_scene_linear_cpu_processor(colorspace);

  if (processor != nullptr) {
    OCIO_cpuProcessorApplyRGB(processor, pixel);
  }
}

void IMB_colormanagement_scene_linear_to_colorspace_v3(float pixel[3], ColorSpace *colorspace)
{
  OCIO_ConstCPUProcessorRcPtr *processor;

  if (!colorspace) {
    /* should never happen */
    printf("%s: perform conversion from unknown color space\n", __func__);
    return;
  }

  processor = colorspace_from_scene_linear_cpu_processor(colorspace);

  if (processor != nullptr) {
    OCIO_cpuProcessorApplyRGB(processor, pixel);
  }
}

void IMB_colormanagement_colorspace_to_scene_linear_v4(float pixel[4],
                                                       bool predivide,
                                                       ColorSpace *colorspace)
{
  OCIO_ConstCPUProcessorRcPtr *processor;

  if (!colorspace) {
    /* should never happen */
    printf("%s: perform conversion from unknown color space\n", __func__);
    return;
  }

  processor = colorspace_to_scene_linear_cpu_processor(colorspace);

  if (processor != nullptr) {
    if (predivide) {
      OCIO_cpuProcessorApplyRGBA_predivide(processor, pixel);
    }
    else {
      OCIO_cpuProcessorApplyRGBA(processor, pixel);
    }
  }
}

void IMB_colormanagement_colorspace_to_scene_linear(float *buffer,
                                                    int width,
                                                    int height,
                                                    int channels,
                                                    struct ColorSpace *colorspace,
                                                    bool predivide)
{
  OCIO_ConstCPUProcessorRcPtr *processor;

  if (!colorspace) {
    /* should never happen */
    printf("%s: perform conversion from unknown color space\n", __func__);
    return;
  }

  processor = colorspace_to_scene_linear_cpu_processor(colorspace);

  if (processor != nullptr) {
    OCIO_PackedImageDesc *img;

    img = OCIO_createOCIO_PackedImageDesc(buffer,
                                          width,
                                          height,
                                          channels,
                                          sizeof(float),
                                          size_t(channels) * sizeof(float),
                                          size_t(channels) * sizeof(float) * width);

    if (predivide) {
      OCIO_cpuProcessorApply_predivide(processor, img);
    }
    else {
      OCIO_cpuProcessorApply(processor, img);
    }

    OCIO_PackedImageDescRelease(img);
  }
}

void IMB_colormanagement_imbuf_to_byte_texture(uchar *out_buffer,
                                               const int offset_x,
                                               const int offset_y,
                                               const int width,
                                               const int height,
                                               const struct ImBuf *ibuf,
                                               const bool store_premultiplied)
{
  /* Byte buffer storage, only for sRGB, scene linear and data texture since other
   * color space conversions can't be done on the GPU. */
  BLI_assert(ibuf->rect && ibuf->rect_float == nullptr);
  BLI_assert(IMB_colormanagement_space_is_srgb(ibuf->rect_colorspace) ||
             IMB_colormanagement_space_is_scene_linear(ibuf->rect_colorspace) ||
             IMB_colormanagement_space_is_data(ibuf->rect_colorspace));

  const uchar *in_buffer = (uchar *)ibuf->rect;
  const bool use_premultiply = IMB_alpha_affects_rgb(ibuf) && store_premultiplied;

  for (int y = 0; y < height; y++) {
    const size_t in_offset = (offset_y + y) * ibuf->x + offset_x;
    const size_t out_offset = y * width;
    const uchar *in = in_buffer + in_offset * 4;
    uchar *out = out_buffer + out_offset * 4;

    if (use_premultiply) {
      /* Premultiply only. */
      for (int x = 0; x < width; x++, in += 4, out += 4) {
        out[0] = (in[0] * in[3]) >> 8;
        out[1] = (in[1] * in[3]) >> 8;
        out[2] = (in[2] * in[3]) >> 8;
        out[3] = in[3];
      }
    }
    else {
      /* Copy only. */
      for (int x = 0; x < width; x++, in += 4, out += 4) {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        out[3] = in[3];
      }
    }
  }
}

typedef struct ImbufByteToFloatData {
  OCIO_ConstCPUProcessorRcPtr *processor;
  int width;
  int offset, stride;
  const uchar *in_buffer;
  float *out_buffer;
  bool use_premultiply;
} ImbufByteToFloatData;

static void imbuf_byte_to_float_cb(void *__restrict userdata,
                                   const int y,
                                   const TaskParallelTLS *__restrict /*tls*/)
{
  ImbufByteToFloatData *data = static_cast<ImbufByteToFloatData *>(userdata);

  const size_t in_offset = data->offset + y * data->stride;
  const size_t out_offset = y * data->width;
  const uchar *in = data->in_buffer + in_offset * 4;
  float *out = data->out_buffer + out_offset * 4;

  /* Convert to scene linear, to sRGB and premultiply. */
  for (int x = 0; x < data->width; x++, in += 4, out += 4) {
    float pixel[4];
    rgba_uchar_to_float(pixel, in);
    if (data->processor) {
      OCIO_cpuProcessorApplyRGB(data->processor, pixel);
    }
    else {
      srgb_to_linearrgb_v3_v3(pixel, pixel);
    }
    if (data->use_premultiply) {
      mul_v3_fl(pixel, pixel[3]);
    }
    copy_v4_v4(out, pixel);
  }
}

void IMB_colormanagement_imbuf_to_float_texture(float *out_buffer,
                                                const int offset_x,
                                                const int offset_y,
                                                const int width,
                                                const int height,
                                                const struct ImBuf *ibuf,
                                                const bool store_premultiplied)
{
  /* Float texture are stored in scene linear color space, with premultiplied
   * alpha depending on the image alpha mode. */
  if (ibuf->rect_float) {
    /* Float source buffer. */
    const float *in_buffer = ibuf->rect_float;
    const int in_channels = ibuf->channels;
    const bool use_unpremultiply = IMB_alpha_affects_rgb(ibuf) && !store_premultiplied;

    for (int y = 0; y < height; y++) {
      const size_t in_offset = (offset_y + y) * ibuf->x + offset_x;
      const size_t out_offset = y * width;
      const float *in = in_buffer + in_offset * in_channels;
      float *out = out_buffer + out_offset * 4;

      if (in_channels == 1) {
        /* Copy single channel. */
        for (int x = 0; x < width; x++, in += 1, out += 4) {
          out[0] = in[0];
          out[1] = in[0];
          out[2] = in[0];
          out[3] = in[0];
        }
      }
      else if (in_channels == 3) {
        /* Copy RGB. */
        for (int x = 0; x < width; x++, in += 3, out += 4) {
          out[0] = in[0];
          out[1] = in[1];
          out[2] = in[2];
          out[3] = 1.0f;
        }
      }
      else if (in_channels == 4) {
        /* Copy or convert RGBA. */
        if (use_unpremultiply) {
          for (int x = 0; x < width; x++, in += 4, out += 4) {
            premul_to_straight_v4_v4(out, in);
          }
        }
        else {
          memcpy(out, in, sizeof(float[4]) * width);
        }
      }
    }
  }
  else {
    /* Byte source buffer. */
    const uchar *in_buffer = (uchar *)ibuf->rect;
    const bool use_premultiply = IMB_alpha_affects_rgb(ibuf) && store_premultiplied;

    OCIO_ConstCPUProcessorRcPtr *processor = (ibuf->rect_colorspace) ?
                                                 colorspace_to_scene_linear_cpu_processor(
                                                     ibuf->rect_colorspace) :
                                                 nullptr;

    ImbufByteToFloatData data = {};
    data.processor = processor;
    data.width = width;
    data.offset = offset_y * ibuf->x + offset_x;
    data.stride = ibuf->x;
    data.in_buffer = in_buffer;
    data.out_buffer = out_buffer;
    data.use_premultiply = use_premultiply;

    TaskParallelSettings settings;
    BLI_parallel_range_settings_defaults(&settings);
    settings.use_threading = (height > 128);
    BLI_task_parallel_range(0, height, &data, imbuf_byte_to_float_cb, &settings);
  }
}

void IMB_colormanagement_scene_linear_to_color_picking_v3(float color_picking[3],
                                                          const float scene_linear[3])
{
  if (!global_color_picking_state.cpu_processor_to && !global_color_picking_state.failed) {
    /* Create processor if none exists. */
    BLI_mutex_lock(&processor_lock);

    if (!global_color_picking_state.cpu_processor_to && !global_color_picking_state.failed) {
      OCIO_ConstProcessorRcPtr *processor = create_colorspace_transform_processor(
          global_role_scene_linear, global_role_color_picking);

      if (processor != nullptr) {
        global_color_picking_state.cpu_processor_to = OCIO_processorGetCPUProcessor(processor);
        OCIO_processorRelease(processor);
      }
      else {
        global_color_picking_state.failed = true;
      }
    }

    BLI_mutex_unlock(&processor_lock);
  }

  copy_v3_v3(color_picking, scene_linear);

  if (global_color_picking_state.cpu_processor_to) {
    OCIO_cpuProcessorApplyRGB(global_color_picking_state.cpu_processor_to, color_picking);
  }
}

void IMB_colormanagement_color_picking_to_scene_linear_v3(float scene_linear[3],
                                                          const float color_picking[3])
{
  if (!global_color_picking_state.cpu_processor_from && !global_color_picking_state.failed) {
    /* Create processor if none exists. */
    BLI_mutex_lock(&processor_lock);

    if (!global_color_picking_state.cpu_processor_from && !global_color_picking_state.failed) {
      OCIO_ConstProcessorRcPtr *processor = create_colorspace_transform_processor(
          global_role_color_picking, global_role_scene_linear);

      if (processor != nullptr) {
        global_color_picking_state.cpu_processor_from = OCIO_processorGetCPUProcessor(processor);
        OCIO_processorRelease(processor);
      }
      else {
        global_color_picking_state.failed = true;
      }
    }

    BLI_mutex_unlock(&processor_lock);
  }

  copy_v3_v3(scene_linear, color_picking);

  if (global_color_picking_state.cpu_processor_from) {
    OCIO_cpuProcessorApplyRGB(global_color_picking_state.cpu_processor_from, scene_linear);
  }
}

void IMB_colormanagement_scene_linear_to_display_v3(float pixel[3], ColorManagedDisplay *display)
{
  OCIO_ConstCPUProcessorRcPtr *processor = display_from_scene_linear_processor(display);

  if (processor != nullptr) {
    OCIO_cpuProcessorApplyRGB(processor, pixel);
  }
}

void IMB_colormanagement_display_to_scene_linear_v3(float pixel[3], ColorManagedDisplay *display)
{
  OCIO_ConstCPUProcessorRcPtr *processor = display_to_scene_linear_processor(display);

  if (processor != nullptr) {
    OCIO_cpuProcessorApplyRGB(processor, pixel);
  }
}

void IMB_colormanagement_pixel_to_display_space_v4(
    float result[4],
    const float pixel[4],
    const ColorManagedViewSettings *view_settings,
    const ColorManagedDisplaySettings *display_settings)
{
  ColormanageProcessor *cm_processor;

  copy_v4_v4(result, pixel);

  cm_processor = IMB_colormanagement_display_processor_new(view_settings, display_settings);
  IMB_colormanagement_processor_apply_v4(cm_processor, result);
  IMB_colormanagement_processor_free(cm_processor);
}

void IMB_colormanagement_pixel_to_display_space_v3(
    float result[3],
    const float pixel[3],
    const ColorManagedViewSettings *view_settings,
    const ColorManagedDisplaySettings *display_settings)
{
  ColormanageProcessor *cm_processor;

  copy_v3_v3(result, pixel);

  cm_processor = IMB_colormanagement_display_processor_new(view_settings, display_settings);
  IMB_colormanagement_processor_apply_v3(cm_processor, result);
  IMB_colormanagement_processor_free(cm_processor);
}

static void colormanagement_imbuf_make_display_space(
    ImBuf *ibuf,
    const ColorManagedViewSettings *view_settings,
    const ColorManagedDisplaySettings *display_settings,
    bool make_byte)
{
  if (!ibuf->rect && make_byte) {
    imb_addrectImBuf(ibuf);
  }

  colormanage_display_buffer_process_ex(
      ibuf, ibuf->rect_float, (uchar *)ibuf->rect, view_settings, display_settings);
}

void IMB_colormanagement_imbuf_make_display_space(
    ImBuf *ibuf,
    const ColorManagedViewSettings *view_settings,
    const ColorManagedDisplaySettings *display_settings)
{
  colormanagement_imbuf_make_display_space(ibuf, view_settings, display_settings, false);
}

static ImBuf *imbuf_ensure_editable(ImBuf *ibuf, ImBuf *colormanaged_ibuf, bool allocate_result)
{
  if (colormanaged_ibuf != ibuf) {
    /* Is already an editable copy. */
    return colormanaged_ibuf;
  }

  if (allocate_result) {
    /* Copy full image buffer. */
    colormanaged_ibuf = IMB_dupImBuf(ibuf);
    IMB_metadata_copy(colormanaged_ibuf, ibuf);
    return colormanaged_ibuf;
  }

  /* Render pipeline is constructing image buffer itself,
   * but it's re-using byte and float buffers from render result make copy of this buffers
   * here sine this buffers would be transformed to other color space here. */
  if (ibuf->rect && (ibuf->mall & IB_rect) == 0) {
    ibuf->rect = static_cast<uint *>(MEM_dupallocN(ibuf->rect));
    ibuf->mall |= IB_rect;
  }

  if (ibuf->rect_float && (ibuf->mall & IB_rectfloat) == 0) {
    ibuf->rect_float = static_cast<float *>(MEM_dupallocN(ibuf->rect_float));
    ibuf->mall |= IB_rectfloat;
  }

  return ibuf;
}

ImBuf *IMB_colormanagement_imbuf_for_write(ImBuf *ibuf,
                                           bool save_as_render,
                                           bool allocate_result,
                                           const ImageFormatData *image_format)
{
  ImBuf *colormanaged_ibuf = ibuf;

  /* Update byte buffer if exists but invalid. */
  if (ibuf->rect_float && ibuf->rect &&
      (ibuf->userflags & (IB_DISPLAY_BUFFER_INVALID | IB_RECT_INVALID)) != 0)
  {
    IMB_rect_from_float(ibuf);
    ibuf->userflags &= ~(IB_RECT_INVALID | IB_DISPLAY_BUFFER_INVALID);
  }

  /* Detect if we are writing to a file format that needs a linear float buffer. */
  const bool linear_float_output = BKE_imtype_requires_linear_float(image_format->imtype);

  /* Detect if we are writing output a byte buffer, which we would need to create
   * with color management conversions applied. This may be for either applying the
   * display transform for renders, or a user specified color space for the file. */
  const bool byte_output = BKE_image_format_is_byte(image_format);

  BLI_assert(!(byte_output && linear_float_output));

  /* If we're saving from RGBA to RGB buffer then it's not so much useful to just ignore alpha --
   * it leads to bad artifacts especially when saving byte images.
   *
   * What we do here is we're overlaying our image on top of background color (which is currently
   * black). This is quite much the same as what Gimp does and it seems to be what artists expects
   * from saving.
   *
   * Do a conversion here, so image format writers could happily assume all the alpha tricks were
   * made already. helps keep things locally here, not spreading it to all possible image writers
   * we've got.
   */
  if (image_format->planes != R_IMF_PLANES_RGBA) {
    float color[3] = {0, 0, 0};

    colormanaged_ibuf = imbuf_ensure_editable(ibuf, colormanaged_ibuf, allocate_result);

    if (colormanaged_ibuf->rect_float && colormanaged_ibuf->channels == 4) {
      IMB_alpha_under_color_float(
          colormanaged_ibuf->rect_float, colormanaged_ibuf->x, colormanaged_ibuf->y, color);
    }

    if (colormanaged_ibuf->rect) {
      IMB_alpha_under_color_byte(
          (uchar *)colormanaged_ibuf->rect, colormanaged_ibuf->x, colormanaged_ibuf->y, color);
    }
  }

  if (save_as_render && !linear_float_output) {
    /* Render output: perform conversion to display space using view transform. */
    colormanaged_ibuf = imbuf_ensure_editable(ibuf, colormanaged_ibuf, allocate_result);

    colormanagement_imbuf_make_display_space(colormanaged_ibuf,
                                             &image_format->view_settings,
                                             &image_format->display_settings,
                                             byte_output);

    if (colormanaged_ibuf->rect_float) {
      /* Float buffer isn't linear anymore,
       * image format write callback should check for this flag and assume
       * no space conversion should happen if ibuf->float_colorspace != nullptr. */
      colormanaged_ibuf->float_colorspace = display_transform_get_colorspace(
          &image_format->view_settings, &image_format->display_settings);
      if (byte_output) {
        colormanaged_ibuf->rect_colorspace = colormanaged_ibuf->float_colorspace;
      }
    }
  }
  else {
    /* Linear render or regular file output: conversion between two color spaces. */

    /* Detect which color space we need to convert between. */
    const char *from_colorspace = (ibuf->rect_float && !(byte_output && ibuf->rect)) ?
                                      /* From float buffer. */
                                      (ibuf->float_colorspace) ? ibuf->float_colorspace->name :
                                                                 global_role_scene_linear :
                                      /* From byte buffer. */
                                      (ibuf->rect_colorspace) ? ibuf->rect_colorspace->name :
                                                                global_role_default_byte;

    const char *to_colorspace = image_format->linear_colorspace_settings.name;

    /* TODO: can we check with OCIO if color spaces are the same but have different names? */
    if (to_colorspace[0] == '\0' || STREQ(from_colorspace, to_colorspace)) {
      /* No conversion needed, but may still need to allocate byte buffer for output. */
      if (byte_output && !ibuf->rect) {
        ibuf->rect_colorspace = ibuf->float_colorspace;
        IMB_rect_from_float(ibuf);
      }
    }
    else {
      /* Color space conversion needed. */
      colormanaged_ibuf = imbuf_ensure_editable(ibuf, colormanaged_ibuf, allocate_result);

      if (byte_output) {
        colormanaged_ibuf->rect_colorspace = colormanage_colorspace_get_named(to_colorspace);

        if (colormanaged_ibuf->rect) {
          /* Byte to byte. */
          IMB_colormanagement_transform_byte_threaded((uchar *)colormanaged_ibuf->rect,
                                                      colormanaged_ibuf->x,
                                                      colormanaged_ibuf->y,
                                                      colormanaged_ibuf->channels,
                                                      from_colorspace,
                                                      to_colorspace);
        }
        else {
          /* Float to byte. */
          IMB_rect_from_float(colormanaged_ibuf);
        }
      }
      else {
        if (!colormanaged_ibuf->rect_float) {
          /* Byte to float. */
          IMB_float_from_rect(colormanaged_ibuf);
          imb_freerectImBuf(colormanaged_ibuf);

          /* This conversion always goes to scene linear. */
          from_colorspace = global_role_scene_linear;
        }

        if (colormanaged_ibuf->rect_float) {
          /* Float to float. */
          IMB_colormanagement_transform(colormanaged_ibuf->rect_float,
                                        colormanaged_ibuf->x,
                                        colormanaged_ibuf->y,
                                        colormanaged_ibuf->channels,
                                        from_colorspace,
                                        to_colorspace,
                                        false);

          colormanaged_ibuf->float_colorspace = colormanage_colorspace_get_named(to_colorspace);
        }
      }
    }
  }

  return colormanaged_ibuf;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public Display Buffers Interfaces
 * \{ */

uchar *IMB_display_buffer_acquire(ImBuf *ibuf,
                                  const ColorManagedViewSettings *view_settings,
                                  const ColorManagedDisplaySettings *display_settings,
                                  void **cache_handle)
{
  uchar *display_buffer;
  size_t buffer_size;
  ColormanageCacheViewSettings cache_view_settings;
  ColormanageCacheDisplaySettings cache_display_settings;
  ColorManagedViewSettings default_view_settings;
  const ColorManagedViewSettings *applied_view_settings;

  *cache_handle = nullptr;

  if (!ibuf->x || !ibuf->y) {
    return nullptr;
  }

  if (view_settings) {
    applied_view_settings = view_settings;
  }
  else {
    /* If no view settings were specified, use default ones, which will
     * attempt not to do any extra color correction. */
    IMB_colormanagement_init_default_view_settings(&default_view_settings, display_settings);
    applied_view_settings = &default_view_settings;
  }

  /* early out: no float buffer and byte buffer is already in display space,
   * let's just use if
   */
  if (ibuf->rect_float == nullptr && ibuf->rect_colorspace && ibuf->channels == 4) {
    if (is_ibuf_rect_in_display_space(ibuf, applied_view_settings, display_settings)) {
      return (uchar *)ibuf->rect;
    }
  }

  colormanage_view_settings_to_cache(ibuf, &cache_view_settings, applied_view_settings);
  colormanage_display_settings_to_cache(&cache_display_settings, display_settings);

  if (ibuf->invalid_rect.xmin != ibuf->invalid_rect.xmax) {
    if ((ibuf->userflags & IB_DISPLAY_BUFFER_INVALID) == 0) {
      IMB_partial_display_buffer_update_threaded(ibuf,
                                                 ibuf->rect_float,
                                                 (uchar *)ibuf->rect,
                                                 ibuf->x,
                                                 0,
                                                 0,
                                                 applied_view_settings,
                                                 display_settings,
                                                 ibuf->invalid_rect.xmin,
                                                 ibuf->invalid_rect.ymin,
                                                 ibuf->invalid_rect.xmax,
                                                 ibuf->invalid_rect.ymax);
    }

    BLI_rcti_init(&ibuf->invalid_rect, 0, 0, 0, 0);
  }

  BLI_thread_lock(LOCK_COLORMANAGE);

  /* ensure color management bit fields exists */
  if (!ibuf->display_buffer_flags) {
    ibuf->display_buffer_flags = static_cast<uint *>(
        MEM_callocN(sizeof(uint) * global_tot_display, "imbuf display_buffer_flags"));
  }
  else if (ibuf->userflags & IB_DISPLAY_BUFFER_INVALID) {
    /* all display buffers were marked as invalid from other areas,
     * now propagate this flag to internal color management routines
     */
    memset(ibuf->display_buffer_flags, 0, global_tot_display * sizeof(uint));

    ibuf->userflags &= ~IB_DISPLAY_BUFFER_INVALID;
  }

  display_buffer = colormanage_cache_get(
      ibuf, &cache_view_settings, &cache_display_settings, cache_handle);

  if (display_buffer) {
    BLI_thread_unlock(LOCK_COLORMANAGE);
    return display_buffer;
  }

  buffer_size = DISPLAY_BUFFER_CHANNELS * size_t(ibuf->x) * ibuf->y * sizeof(char);
  display_buffer = static_cast<uchar *>(MEM_callocN(buffer_size, "imbuf display buffer"));

  colormanage_display_buffer_process(
      ibuf, display_buffer, applied_view_settings, display_settings);

  colormanage_cache_put(
      ibuf, &cache_view_settings, &cache_display_settings, display_buffer, cache_handle);

  BLI_thread_unlock(LOCK_COLORMANAGE);

  return display_buffer;
}

uchar *IMB_display_buffer_acquire_ctx(const bContext *C, ImBuf *ibuf, void **cache_handle)
{
  ColorManagedViewSettings *view_settings;
  ColorManagedDisplaySettings *display_settings;

  IMB_colormanagement_display_settings_from_ctx(C, &view_settings, &display_settings);

  return IMB_display_buffer_acquire(ibuf, view_settings, display_settings, cache_handle);
}

void IMB_display_buffer_transform_apply(uchar *display_buffer,
                                        float *linear_buffer,
                                        int width,
                                        int height,
                                        int channels,
                                        const ColorManagedViewSettings *view_settings,
                                        const ColorManagedDisplaySettings *display_settings,
                                        bool predivide)
{
  float *buffer;
  ColormanageProcessor *cm_processor = IMB_colormanagement_display_processor_new(view_settings,
                                                                                 display_settings);

  buffer = static_cast<float *>(MEM_mallocN(size_t(channels) * width * height * sizeof(float),
                                            "display transform temp buffer"));
  memcpy(buffer, linear_buffer, size_t(channels) * width * height * sizeof(float));

  IMB_colormanagement_processor_apply(cm_processor, buffer, width, height, channels, predivide);

  IMB_colormanagement_processor_free(cm_processor);

  IMB_buffer_byte_from_float(display_buffer,
                             buffer,
                             channels,
                             0.0f,
                             IB_PROFILE_SRGB,
                             IB_PROFILE_SRGB,
                             false,
                             width,
                             height,
                             width,
                             width);

  MEM_freeN(buffer);
}

void IMB_display_buffer_release(void *cache_handle)
{
  if (cache_handle) {
    BLI_thread_lock(LOCK_COLORMANAGE);

    colormanage_cache_handle_release(cache_handle);

    BLI_thread_unlock(LOCK_COLORMANAGE);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Display Functions
 * \{ */

const char *colormanage_display_get_default_name(void)
{
  OCIO_ConstConfigRcPtr *config = OCIO_getCurrentConfig();
  const char *display_name;

  display_name = OCIO_configGetDefaultDisplay(config);

  OCIO_configRelease(config);

  return display_name;
}

ColorManagedDisplay *colormanage_display_get_default(void)
{
  const char *display_name = colormanage_display_get_default_name();

  if (display_name[0] == '\0') {
    return nullptr;
  }

  return colormanage_display_get_named(display_name);
}

ColorManagedDisplay *colormanage_display_add(const char *name)
{
  ColorManagedDisplay *display;
  int index = 0;

  if (global_displays.last) {
    ColorManagedDisplay *last_display = static_cast<ColorManagedDisplay *>(global_displays.last);

    index = last_display->index;
  }

  display = MEM_cnew<ColorManagedDisplay>("ColorManagedDisplay");

  display->index = index + 1;

  BLI_strncpy(display->name, name, sizeof(display->name));

  BLI_addtail(&global_displays, display);

  return display;
}

ColorManagedDisplay *colormanage_display_get_named(const char *name)
{
  LISTBASE_FOREACH (ColorManagedDisplay *, display, &global_displays) {
    if (STREQ(display->name, name)) {
      return display;
    }
  }

  return nullptr;
}

ColorManagedDisplay *colormanage_display_get_indexed(int index)
{
  /* display indices are 1-based */
  return static_cast<ColorManagedDisplay *>(BLI_findlink(&global_displays, index - 1));
}

int IMB_colormanagement_display_get_named_index(const char *name)
{
  ColorManagedDisplay *display;

  display = colormanage_display_get_named(name);

  if (display) {
    return display->index;
  }

  return 0;
}

const char *IMB_colormanagement_display_get_indexed_name(int index)
{
  ColorManagedDisplay *display;

  display = colormanage_display_get_indexed(index);

  if (display) {
    return display->name;
  }

  return nullptr;
}

const char *IMB_colormanagement_display_get_default_name(void)
{
  ColorManagedDisplay *display = colormanage_display_get_default();

  return display->name;
}

ColorManagedDisplay *IMB_colormanagement_display_get_named(const char *name)
{
  return colormanage_display_get_named(name);
}

const char *IMB_colormanagement_display_get_none_name(void)
{
  if (colormanage_display_get_named("None") != nullptr) {
    return "None";
  }

  return colormanage_display_get_default_name();
}

const char *IMB_colormanagement_display_get_default_view_transform_name(
    struct ColorManagedDisplay *display)
{
  return colormanage_view_get_default_name(display);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name View Functions
 * \{ */

const char *colormanage_view_get_default_name(const ColorManagedDisplay *display)
{
  OCIO_ConstConfigRcPtr *config = OCIO_getCurrentConfig();
  const char *name = OCIO_configGetDefaultView(config, display->name);

  OCIO_configRelease(config);

  return name;
}

ColorManagedView *colormanage_view_get_default(const ColorManagedDisplay *display)
{
  const char *name = colormanage_view_get_default_name(display);

  if (!name || name[0] == '\0') {
    return nullptr;
  }

  return colormanage_view_get_named(name);
}

ColorManagedView *colormanage_view_add(const char *name)
{
  ColorManagedView *view;
  int index = global_tot_view;

  view = MEM_cnew<ColorManagedView>("ColorManagedView");
  view->index = index + 1;
  BLI_strncpy(view->name, name, sizeof(view->name));

  BLI_addtail(&global_views, view);

  global_tot_view++;

  return view;
}

ColorManagedView *colormanage_view_get_named(const char *name)
{
  LISTBASE_FOREACH (ColorManagedView *, view, &global_views) {
    if (STREQ(view->name, name)) {
      return view;
    }
  }

  return nullptr;
}

ColorManagedView *colormanage_view_get_indexed(int index)
{
  /* view transform indices are 1-based */
  return static_cast<ColorManagedView *>(BLI_findlink(&global_views, index - 1));
}

ColorManagedView *colormanage_view_get_named_for_display(const char *display_name,
                                                         const char *name)
{
  ColorManagedDisplay *display = colormanage_display_get_named(display_name);
  if (display == nullptr) {
    return nullptr;
  }
  LISTBASE_FOREACH (LinkData *, view_link, &display->views) {
    ColorManagedView *view = static_cast<ColorManagedView *>(view_link->data);
    if (STRCASEEQ(name, view->name)) {
      return view;
    }
  }
  return nullptr;
}

int IMB_colormanagement_view_get_named_index(const char *name)
{
  ColorManagedView *view = colormanage_view_get_named(name);

  if (view) {
    return view->index;
  }

  return 0;
}

const char *IMB_colormanagement_view_get_indexed_name(int index)
{
  ColorManagedView *view = colormanage_view_get_indexed(index);

  if (view) {
    return view->name;
  }

  return nullptr;
}

const char *IMB_colormanagement_view_get_default_name(const char *display_name)
{
  ColorManagedDisplay *display = colormanage_display_get_named(display_name);
  ColorManagedView *view = nullptr;

  if (display) {
    view = colormanage_view_get_default(display);
  }

  if (view) {
    return view->name;
  }

  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Color Space Functions
 * \{ */

static void colormanage_description_strip(char *description)
{
  int i, n;

  for (i = int(strlen(description)) - 1; i >= 0; i--) {
    if (ELEM(description[i], '\r', '\n')) {
      description[i] = '\0';
    }
    else {
      break;
    }
  }

  for (i = 0, n = strlen(description); i < n; i++) {
    if (ELEM(description[i], '\r', '\n')) {
      description[i] = ' ';
    }
  }
}

ColorSpace *colormanage_colorspace_add(const char *name,
                                       const char *description,
                                       bool is_invertible,
                                       bool is_data)
{
  ColorSpace *colorspace, *prev_space;
  int counter = 1;

  colorspace = MEM_cnew<ColorSpace>("ColorSpace");

  BLI_strncpy(colorspace->name, name, sizeof(colorspace->name));

  if (description) {
    BLI_strncpy(colorspace->description, description, sizeof(colorspace->description));

    colormanage_description_strip(colorspace->description);
  }

  colorspace->is_invertible = is_invertible;
  colorspace->is_data = is_data;

  for (prev_space = static_cast<ColorSpace *>(global_colorspaces.first); prev_space;
       prev_space = prev_space->next)
  {
    if (BLI_strcasecmp(prev_space->name, colorspace->name) > 0) {
      break;
    }

    prev_space->index = counter++;
  }

  if (!prev_space) {
    BLI_addtail(&global_colorspaces, colorspace);
  }
  else {
    BLI_insertlinkbefore(&global_colorspaces, prev_space, colorspace);
  }

  colorspace->index = counter++;
  for (; prev_space; prev_space = prev_space->next) {
    prev_space->index = counter++;
  }

  global_tot_colorspace++;

  return colorspace;
}

ColorSpace *colormanage_colorspace_get_named(const char *name)
{
  LISTBASE_FOREACH (ColorSpace *, colorspace, &global_colorspaces) {
    if (STREQ(colorspace->name, name)) {
      return colorspace;
    }

    for (int i = 0; i < colorspace->num_aliases; i++) {
      if (STREQ(colorspace->aliases[i], name)) {
        return colorspace;
      }
    }
  }

  return nullptr;
}

ColorSpace *colormanage_colorspace_get_roled(int role)
{
  const char *role_colorspace = IMB_colormanagement_role_colorspace_name_get(role);

  return colormanage_colorspace_get_named(role_colorspace);
}

ColorSpace *colormanage_colorspace_get_indexed(int index)
{
  /* color space indices are 1-based */
  return static_cast<ColorSpace *>(BLI_findlink(&global_colorspaces, index - 1));
}

int IMB_colormanagement_colorspace_get_named_index(const char *name)
{
  ColorSpace *colorspace = colormanage_colorspace_get_named(name);

  if (colorspace) {
    return colorspace->index;
  }

  return 0;
}

const char *IMB_colormanagement_colorspace_get_indexed_name(int index)
{
  ColorSpace *colorspace = colormanage_colorspace_get_indexed(index);

  if (colorspace) {
    return colorspace->name;
  }

  return "";
}

const char *IMB_colormanagement_colorspace_get_name(const ColorSpace *colorspace)
{
  return colorspace->name;
}

void IMB_colormanagement_colorspace_from_ibuf_ftype(
    ColorManagedColorspaceSettings *colorspace_settings, ImBuf *ibuf)
{
  /* Don't modify non-color data space, it does not change with file type. */
  ColorSpace *colorspace = colormanage_colorspace_get_named(colorspace_settings->name);

  if (colorspace && colorspace->is_data) {
    return;
  }

  /* Get color space from file type. */
  const ImFileType *type = IMB_file_type_from_ibuf(ibuf);
  if (type != nullptr) {
    if (type->save != nullptr) {
      const char *role_colorspace = IMB_colormanagement_role_colorspace_name_get(
          type->default_save_role);
      BLI_strncpy(colorspace_settings->name, role_colorspace, sizeof(colorspace_settings->name));
    }
  }
}
