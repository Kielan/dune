#pragma once

#include "GPU_texture.h"
#include "GPU_vertex_buffer.h"

#define FONT_BATCH_DRAW_LEN_MAX 2048 /* in glyph */

/* Number of characters in GlyphCacheBLF.glyph_ascii_table. */
#define GLYPH_ASCII_TABLE_SIZE 128

/* Number of characters in KerningCacheBLF.table. */
#define KERNING_CACHE_TABLE_SIZE 128

/* A value in the kerning cache that indicates it is not yet set. */
#define KERNING_ENTRY_UNSET INT_MAX

typedef struct FontBatch {
  struct Font *font; /* can only batch glyph from the same font */
  struct GPUBatch *batch;
  struct GPUVertBuf *verts;
  struct GPUVertBufRaw pos_step, col_step, offset_step, glyph_size_step;
  unsigned int pos_loc, col_loc, offset_loc, glyph_size_loc;
  unsigned int glyph_len;
  float ofs[2];    /* copy of font->pos */
  float mat[4][4]; /* previous call modelmatrix. */
  bool enabled, active, simple_shader;
  struct FontGlyphCache *glyph_cache;
} FontBatch;

extern FontBatch g_batch;

typedef struct KerningCacheBLF {
  /* Cache a ascii glyph pairs. Only store the x offset we are interested in,
   * instead of the full #FT_Vector since it's not used for drawing at the moment. */
  int ascii_table[KERNING_CACHE_TABLE_SIZE][KERNING_CACHE_TABLE_SIZE];
} FontKerningCache;

typedef struct FontGlyphCache {
  struct FontGlyphCache *next;
  struct FontGlyphCache *prev;

  /* font size. */
  float size;

  /* and dpi. */
  unsigned int dpi;

  bool bold;
  bool italic;

  /* Column width when printing monospaced. */
  int fixed_width;

  /* and the glyphs. */
  List bucket[257];

  /* fast ascii lookup */
  struct FontGlyph *glyph_ascii_table[GLYPH_ASCII_TABLE_SIZE];

  /* texture array, to draw the glyphs. */
  GPUTexture *texture;
  char *bitmap_result;
  int bitmap_len;
  int bitmap_len_landed;
  int bitmap_len_alloc;

} FontGlyphCache;

typedef struct FontGlyph {
  struct FontGlyph *next;
  struct FontGlyph *prev;

  /* and the character, as UTF-32 */
  unsigned int c;

  /* freetype2 index, to speed-up the search. */
  FT_UInt idx;

  /* glyph box. */
  rctf box;

  /* advance size. */
  float advance;
  /* avoid conversion to int while drawing */
  int advance_i;

  /* position inside the texture where this glyph is store. */
  int offset;

  /* Bitmap data, from freetype. Take care that this
   * can be NULL. */
  unsigned char *bitmap;

  /* Glyph width and height. */
  int dims[2];
  int pitch;

  /* X and Y bearing of the glyph.
   * The X bearing is from the origin to the glyph left bbox edge.
   * The Y bearing is from the baseline to the top of the glyph edge.  */
  int pos[2];

  struct FontGlyphCache *glyph_cache;
} FontGlyph;

typedef struct FontBufInfo {
  /* for draw to buffer, always set this to NULL after finish! */
  float *fbuf;

  /* the same but unsigned char */
  unsigned char *cbuf;

  /** Buffer size, keep signed so comparisons with negative values work. */
  int dims[2];

  /* number of channels. */
  int ch;

  /* display device used for color management */
  struct ColorManagedDisplay *display;

  /* and the color, the alphas is get from the glyph!
   * color is sRGB space */
  float col_init[4];
  /* cached conversion from 'col_init' */
  unsigned char col_char[4];
  float col_float[4];

} FontBufInfo;

typedef struct Font {
  /* font name. */
  char *name;

  /* # of times this font was loaded */
  unsigned int reference_count;

  /* filename or NULL. */
  char *filename;

  /* aspect ratio or scale. */
  float aspect[3];

  /* initial position for draw the text. */
  float pos[3];

  /* angle in radians. */
  float angle;

#if 0 /* FONT_BLUR_ENABLE */
  /* blur: 3 or 5 large kernel */
  int blur;
#endif

  /* shadow level. */
  int shadow;

  /* and shadow offset. */
  int shadow_x;
  int shadow_y;

  /* shadow color. */
  unsigned char shadow_color[4];

  /* main text color. */
  unsigned char color[4];

  /* Multiplied this matrix with the current one before
   * draw the text! see blf_draw__start. */
  float m[16];

  /* clipping rectangle. */
  rctf clip_rec;

  /* the width to wrap the text, see BLF_WORD_WRAP */
  int wrap_width;

  /* font dpi (default 72). */
  unsigned int dpi;

  /* font size. */
  float size;

  /* max texture size. */
  int tex_size_max;

  /* font options. */
  int flags;

  /* List of glyph caches (GlyphCacheBLF) for this font for size, dpi, bold, italic.
   * Use blf_glyph_cache_acquire(font) and blf_glyph_cache_release(font) to access cache!  */
  List cache;

  /* Cache of unscaled kerning values. Will be NULL if font does not have kerning. */
  KerningCacheBLF *kerning_cache;

  /* freetype2 lib handle. */
  FT_Library ft_lib;

  /* Mutex lock for library */
  SpinLock *ft_lib_mutex;

  /* freetype2 face. */
  FT_Face face;

  /* data for buffer usage (drawing into a texture buffer) */
  FontBufInfoBLF buf_info;

  /* Mutex lock for glyph cache. */
  SpinLock *glyph_cache_mutex;
} FontBLF;

typedef struct DirBLF {
  struct DirBLF *next;
  struct DirBLF *prev;

  /* full path where search fonts. */
  char *path;
} DirBLF;
