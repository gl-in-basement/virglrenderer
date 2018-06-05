/**************************************************************************
 *
 * Copyright (C) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "tgsi/tgsi_info.h"
#include "tgsi/tgsi_iterate.h"
#include "tgsi/tgsi_scan.h"
#include "util/u_memory.h"
#include "util/u_math.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include "vrend_shader.h"

extern int vrend_dump_shaders;

/* start convert of tgsi to glsl */

#define INTERP_PREFIX "                           "
#define INVARI_PREFIX "invariant"

#define SHADER_REQ_NONE 0
#define SHADER_REQ_SAMPLER_RECT       (1 << 0)
#define SHADER_REQ_CUBE_ARRAY         (1 << 1)
#define SHADER_REQ_INTS               (1 << 2)
#define SHADER_REQ_SAMPLER_MS         (1 << 3)
#define SHADER_REQ_INSTANCE_ID        (1 << 4)
#define SHADER_REQ_LODQ               (1 << 5)
#define SHADER_REQ_TXQ_LEVELS         (1 << 6)
#define SHADER_REQ_TG4                (1 << 7)
#define SHADER_REQ_VIEWPORT_IDX       (1 << 8)
#define SHADER_REQ_STENCIL_EXPORT     (1 << 9)
#define SHADER_REQ_LAYER              (1 << 10)
#define SHADER_REQ_SAMPLE_SHADING     (1 << 11)
#define SHADER_REQ_GPU_SHADER5        (1 << 12)
#define SHADER_REQ_DERIVATIVE_CONTROL (1 << 13)

struct vrend_shader_io {
   unsigned                name;
   unsigned                gpr;
   unsigned                done;
   int                        sid;
   unsigned                interpolate;
   unsigned first;
   bool                 centroid;
   bool                    invariant;
   bool glsl_predefined_no_emit;
   bool glsl_no_index;
   bool glsl_gl_in;
   bool override_no_wm;
   bool is_int;
   char glsl_name[64];
   unsigned stream;
};

struct vrend_shader_sampler {
   int tgsi_sampler_type;
   enum tgsi_return_type tgsi_sampler_return;
};

struct vrend_shader_table {
   uint32_t key;
   const char *string;
};

#define MAX_IMMEDIATE 1024
struct immed {
   int type;
   union imm {
      uint32_t ui;
      int32_t i;
      float f;
   } val[4];
};

struct vrend_temp_range {
   int first;
   int last;
   int array_id;
};

struct dump_ctx {
   struct tgsi_iterate_context iter;
   struct vrend_shader_cfg *cfg;
   struct tgsi_shader_info info;
   int prog_type;
   int size;
   char *glsl_main;
   uint instno;

   int num_interps;
   int num_inputs;
   uint32_t attrib_input_mask;
   struct vrend_shader_io inputs[35];
   int num_outputs;
   struct vrend_shader_io outputs[35];
   int num_system_values;
   struct vrend_shader_io system_values[32];

   int num_temp_ranges;
   struct vrend_temp_range *temp_ranges;

   struct vrend_shader_sampler samplers[32];
   uint32_t samplers_used;
   bool sviews_used;
   struct vrend_sampler_array *sampler_arrays;
   int num_sampler_arrays;
   int last_sampler_array_idx;

   int num_consts;
   int num_imm;
   struct immed imm[MAX_IMMEDIATE];
   unsigned fragcoord_input;

   int num_ubo;
   int ubo_idx[32];
   int ubo_sizes[32];
   int num_address;

   uint32_t shader_req_bits;

   struct pipe_stream_output_info *so;
   char **so_names;
   bool write_so_outputs[PIPE_MAX_SO_OUTPUTS];
   bool uses_sampler_buf;
   bool write_all_cbufs;
   uint32_t shadow_samp_mask;

   int fs_coord_origin, fs_pixel_center;

   int gs_in_prim, gs_out_prim, gs_max_out_verts;
   int gs_num_invocations;

   struct vrend_shader_key *key;
   int indent_level;
   int num_in_clip_dist;
   int num_clip_dist;
   int glsl_ver_required;
   int color_in_mask;
   /* only used when cull is enabled */
   uint8_t num_cull_dist_prop, num_clip_dist_prop;
   bool front_face_emitted;

   bool has_clipvertex;
   bool has_clipvertex_so;
   bool vs_has_pervertex;
   bool write_mul_temp;
};

static const struct vrend_shader_table shader_req_table[] = {
    { SHADER_REQ_SAMPLER_RECT, "GL_ARB_texture_rectangle" },
    { SHADER_REQ_CUBE_ARRAY, "GL_ARB_texture_cube_map_array" },
    { SHADER_REQ_INTS, "GL_ARB_shader_bit_encoding" },
    { SHADER_REQ_SAMPLER_MS, "GL_ARB_texture_multisample" },
    { SHADER_REQ_INSTANCE_ID, "GL_ARB_draw_instanced" },
    { SHADER_REQ_LODQ, "GL_ARB_texture_query_lod" },
    { SHADER_REQ_TXQ_LEVELS, "GL_ARB_texture_query_levels" },
    { SHADER_REQ_TG4, "GL_ARB_texture_gather" },
    { SHADER_REQ_VIEWPORT_IDX, "GL_ARB_viewport_array" },
    { SHADER_REQ_STENCIL_EXPORT, "GL_ARB_shader_stencil_export" },
    { SHADER_REQ_LAYER, "GL_ARB_fragment_layer_viewport" },
    { SHADER_REQ_SAMPLE_SHADING, "GL_ARB_sample_shading" },
    { SHADER_REQ_GPU_SHADER5, "GL_ARB_gpu_shader5" },
    { SHADER_REQ_DERIVATIVE_CONTROL, "GL_ARB_derivative_control" },
};

enum vrend_type_qualifier {
   TYPE_CONVERSION_NONE = 0,
   FLOAT = 1,
   VEC2 = 2,
   VEC3 = 3,
   VEC4 = 4,
   INT = 5,
   IVEC2 = 6,
   IVEC3 = 7,
   IVEC4 = 8,
   UINT = 9,
   UVEC2 = 10,
   UVEC3 = 11,
   UVEC4 = 12,
   FLOAT_BITS_TO_UINT = 13,
   UINT_BITS_TO_FLOAT = 14,
   FLOAT_BITS_TO_INT = 15,
   INT_BITS_TO_FLOAT = 16,
};

static const struct vrend_shader_table conversion_table[] =
{
   {TYPE_CONVERSION_NONE, ""},
   {FLOAT, "float"},
   {VEC2, "vec2"},
   {VEC3, "vec3"},
   {VEC4, "vec4"},
   {INT, "int"},
   {IVEC2, "ivec2"},
   {IVEC3, "ivec3"},
   {IVEC4, "ivec4"},
   {UINT, "uint"},
   {UVEC2, "uvec2"},
   {UVEC3, "uvec3"},
   {UVEC4, "uvec4"},
   {FLOAT_BITS_TO_UINT, "floatBitsToUint"},
   {UINT_BITS_TO_FLOAT, "uintBitsToFloat"},
   {FLOAT_BITS_TO_INT, "floatBitsToInt"},
   {INT_BITS_TO_FLOAT, "intBitsToFloat"},
};

static inline const char *get_string(enum vrend_type_qualifier key)
{
   if (key >= ARRAY_SIZE(conversion_table)) {
      printf("Unable to find the correct conversion\n");
      return conversion_table[TYPE_CONVERSION_NONE].string;
   }

   return conversion_table[key].string;
}

static inline const char *tgsi_proc_to_prefix(int shader_type)
{
   switch (shader_type) {
   case TGSI_PROCESSOR_VERTEX: return "vs";
   case TGSI_PROCESSOR_FRAGMENT: return "fs";
   case TGSI_PROCESSOR_GEOMETRY: return "gs";
   default:
      return NULL;
   };
}

static inline const char *prim_to_name(int prim)
{
   switch (prim) {
   case PIPE_PRIM_POINTS: return "points";
   case PIPE_PRIM_LINES: return "lines";
   case PIPE_PRIM_LINE_STRIP: return "line_strip";
   case PIPE_PRIM_LINES_ADJACENCY: return "lines_adjacency";
   case PIPE_PRIM_TRIANGLES: return "triangles";
   case PIPE_PRIM_TRIANGLE_STRIP: return "triangle_strip";
   case PIPE_PRIM_TRIANGLES_ADJACENCY: return "triangles_adjacency";
   default: return "UNKNOWN";
   };
}

static inline int gs_input_prim_to_size(int prim)
{
   switch (prim) {
   case PIPE_PRIM_POINTS: return 1;
   case PIPE_PRIM_LINES: return 2;
   case PIPE_PRIM_LINES_ADJACENCY: return 4;
   case PIPE_PRIM_TRIANGLES: return 3;
   case PIPE_PRIM_TRIANGLES_ADJACENCY: return 6;
   default: return -1;
   };
}

static inline bool fs_emit_layout(struct dump_ctx *ctx)
{
   if (ctx->fs_pixel_center)
      return true;
   /* if coord origin is 0 and invert is 0 - emit origin_upper_left,
      if coord_origin is 0 and invert is 1 - emit nothing (lower)
      if coord origin is 1 and invert is 0 - emit nothing (lower)
      if coord_origin is 1 and invert is 1 - emit origin upper left */
   if (!(ctx->fs_coord_origin ^ ctx->key->invert_fs_origin))
      return true;
   return false;
}

static const char *get_stage_input_name_prefix(struct dump_ctx *ctx, int processor)
{
   const char *name_prefix;
   switch (processor) {
   case TGSI_PROCESSOR_FRAGMENT:
      if (ctx->key->gs_present)
         name_prefix = "gso";
      else
         name_prefix = "vso";
      break;
   case TGSI_PROCESSOR_GEOMETRY:
      name_prefix = "vso";
      break;
   case TGSI_PROCESSOR_VERTEX:
   default:
      name_prefix = "in";
      break;
   }
   return name_prefix;
}

static const char *get_stage_output_name_prefix(int processor)
{
   const char *name_prefix;
   switch (processor) {
   case TGSI_PROCESSOR_FRAGMENT:
      name_prefix = "fsout";
      break;
   case TGSI_PROCESSOR_GEOMETRY:
      name_prefix = "gso";
      break;
   case TGSI_PROCESSOR_VERTEX:
      name_prefix = "vso";
      break;
   default:
      name_prefix = "out";
      break;
   }
   return name_prefix;
}

static void require_glsl_ver(struct dump_ctx *ctx, int glsl_ver)
{
   if (glsl_ver > ctx->glsl_ver_required)
      ctx->glsl_ver_required = glsl_ver;
}

static char *strcat_realloc(char *str, const char *catstr)
{
   char *new = realloc(str, strlen(str) + strlen(catstr) + 1);
   if (!new) {
      free(str);
      return NULL;
   }
   strcat(new, catstr);
   return new;
}

static char *add_str_to_glsl_main(struct dump_ctx *ctx, const char *buf)
{
   ctx->glsl_main = strcat_realloc(ctx->glsl_main, buf);
   return ctx->glsl_main;
}

static int allocate_temp_range(struct dump_ctx *ctx, int first, int last,
                               int array_id)
{
   int idx = ctx->num_temp_ranges;

   ctx->temp_ranges = realloc(ctx->temp_ranges, sizeof(struct vrend_temp_range) * (idx + 1));
   if (!ctx->temp_ranges)
      return ENOMEM;

   ctx->temp_ranges[idx].first = first;
   ctx->temp_ranges[idx].last = last;
   ctx->temp_ranges[idx].array_id = array_id;
   ctx->num_temp_ranges++;
   return 0;
}

static struct vrend_temp_range *find_temp_range(struct dump_ctx *ctx, int index)
{
   int i;
   for (i = 0; i < ctx->num_temp_ranges; i++) {
      if (index >= ctx->temp_ranges[i].first &&
          index <= ctx->temp_ranges[i].last)
         return &ctx->temp_ranges[i];
   }
   return NULL;
}

static int add_sampler_array(struct dump_ctx *ctx, int first, int last, int sview_type, int sview_rtype)
{
   int idx = ctx->num_sampler_arrays;
   ctx->num_sampler_arrays++;
   ctx->sampler_arrays = realloc(ctx->sampler_arrays, sizeof(struct vrend_sampler_array) * ctx->num_sampler_arrays);
   if (!ctx->sampler_arrays)
      return -1;

   ctx->sampler_arrays[idx].first = first;
   ctx->sampler_arrays[idx].last = last;
   ctx->sampler_arrays[idx].idx = idx;
   ctx->sampler_arrays[idx].sview_type = sview_type;
   ctx->sampler_arrays[idx].sview_rtype = sview_rtype;
   return idx;
}

static int lookup_sampler_array(struct dump_ctx *ctx, int index)
{
   int i;
   for (i = 0; i < ctx->num_sampler_arrays; i++) {
      if (index >= ctx->sampler_arrays[i].first &&
          index <= ctx->sampler_arrays[i].last) {
         return ctx->sampler_arrays[i].idx;
      }
   }
   return -1;
}

int shader_lookup_sampler_array(struct vrend_shader_info *sinfo, int index)
{
   int i;
   for (i = 0; i < sinfo->num_sampler_arrays; i++) {
      if (index >= sinfo->sampler_arrays[i].first &&
          index <= sinfo->sampler_arrays[i].last) {
         return sinfo->sampler_arrays[i].idx;
      }
   }
   return -1;
}

static boolean
iter_declaration(struct tgsi_iterate_context *iter,
                 struct tgsi_full_declaration *decl )
{
   struct dump_ctx *ctx = (struct dump_ctx *)iter;
   int i;
   int color_offset = 0;
   const char *name_prefix = "";
   bool add_two_side = false;

   switch (decl->Declaration.File) {
   case TGSI_FILE_INPUT:
      i = ctx->num_inputs++;
      if (ctx->num_inputs > ARRAY_SIZE(ctx->inputs)) {
         fprintf(stderr, "Number of inputs exceeded, max is %lu\n", ARRAY_SIZE(ctx->inputs));
         return FALSE;
      }
      if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
         ctx->attrib_input_mask |= (1 << decl->Range.First);
      }
      ctx->inputs[i].name = decl->Semantic.Name;
      ctx->inputs[i].sid = decl->Semantic.Index;
      ctx->inputs[i].interpolate = decl->Interp.Interpolate;
      ctx->inputs[i].centroid = decl->Interp.Location == TGSI_INTERPOLATE_LOC_CENTROID;
      ctx->inputs[i].first = decl->Range.First;
      ctx->inputs[i].glsl_predefined_no_emit = false;
      ctx->inputs[i].glsl_no_index = false;
      ctx->inputs[i].override_no_wm = false;
      ctx->inputs[i].glsl_gl_in = false;

      switch (ctx->inputs[i].name) {
      case TGSI_SEMANTIC_COLOR:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            if (ctx->glsl_ver_required < 140) {
               if (decl->Semantic.Index == 0)
                  name_prefix = "gl_Color";
               else if (decl->Semantic.Index == 1)
                  name_prefix = "gl_SecondaryColor";
               else
                  fprintf(stderr, "got illegal color semantic index %d\n", decl->Semantic.Index);
               ctx->inputs[i].glsl_no_index = true;
            } else {
               if (ctx->key->color_two_side) {
                  int j = ctx->num_inputs++;
                  if (ctx->num_inputs > ARRAY_SIZE(ctx->inputs)) {
                     fprintf(stderr, "Number of inputs exceeded, max is %lu\n", ARRAY_SIZE(ctx->inputs));
                     return FALSE;
                  }

                  ctx->inputs[j].name = TGSI_SEMANTIC_BCOLOR;
                  ctx->inputs[j].sid = decl->Semantic.Index;
                  ctx->inputs[j].interpolate = decl->Interp.Interpolate;
                  ctx->inputs[j].first = decl->Range.First;
                  ctx->inputs[j].glsl_predefined_no_emit = false;
                  ctx->inputs[j].glsl_no_index = false;
                  ctx->inputs[j].override_no_wm = false;

                  ctx->color_in_mask |= (1 << decl->Semantic.Index);

                  if (ctx->front_face_emitted == false) {
                     int k = ctx->num_inputs++;
                     if (ctx->num_inputs > ARRAY_SIZE(ctx->inputs)) {
                        fprintf(stderr, "Number of inputs exceeded, max is %lu\n", ARRAY_SIZE(ctx->inputs));
                        return FALSE;
                     }

                     ctx->inputs[k].name = TGSI_SEMANTIC_FACE;
                     ctx->inputs[k].sid = 0;
                     ctx->inputs[k].interpolate = 0;
                     ctx->inputs[k].centroid = 0;
                     ctx->inputs[k].first = 0;
                     ctx->inputs[k].override_no_wm = false;
                     ctx->inputs[k].glsl_predefined_no_emit = true;
                     ctx->inputs[k].glsl_no_index = true;
                  }
                  add_two_side = true;
               }
               name_prefix = "ex";
            }
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_PRIMID:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            name_prefix = "gl_PrimitiveIDIn";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].override_no_wm = true;
            ctx->shader_req_bits |= SHADER_REQ_INTS;
            break;
         } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_PrimitiveID";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            require_glsl_ver(ctx, 150);
            break;
         }
      case TGSI_SEMANTIC_VIEWPORT_INDEX:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].is_int = true;
            ctx->inputs[i].override_no_wm = true;
            name_prefix = "gl_ViewportIndex";
            if (ctx->glsl_ver_required >= 140)
               ctx->shader_req_bits |= SHADER_REQ_LAYER;
            break;
         }
      case TGSI_SEMANTIC_LAYER:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_Layer";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].is_int = true;
            ctx->inputs[i].override_no_wm = true;
            ctx->shader_req_bits |= SHADER_REQ_LAYER;
            break;
         }
      case TGSI_SEMANTIC_PSIZE:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            name_prefix = "gl_PointSize";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].override_no_wm = true;
            ctx->inputs[i].glsl_gl_in = true;
            break;
         }
      case TGSI_SEMANTIC_CLIPDIST:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            name_prefix = "gl_ClipDistance";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].glsl_gl_in = true;
            ctx->num_in_clip_dist += 4;
            break;
         } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_ClipDistance";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->num_in_clip_dist += 4;
            break;
         }
      case TGSI_SEMANTIC_POSITION:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            name_prefix = "gl_Position";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].glsl_gl_in = true;
            break;
         } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_FragCoord";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            break;
         }
         /* fallthrough for vertex shader */
      case TGSI_SEMANTIC_FACE:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            if (ctx->front_face_emitted) {
               ctx->num_inputs--;
               return TRUE;
            }
            name_prefix = "gl_FrontFacing";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->front_face_emitted = true;
            break;
         }
      case TGSI_SEMANTIC_GENERIC:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            if (ctx->key->coord_replace & (1 << ctx->inputs[i].sid)) {
               if (ctx->cfg->use_gles)
                  name_prefix = "vec4(gl_PointCoord.x, 1.0 - gl_PointCoord.y, 0.0, 1.0)";
               else
                  name_prefix = "vec4(gl_PointCoord, 0.0, 1.0)";
               ctx->inputs[i].glsl_predefined_no_emit = true;
               ctx->inputs[i].glsl_no_index = true;
               break;
            }
         }
      default:
         name_prefix = get_stage_input_name_prefix(ctx, iter->processor.Processor);
         break;
      }

      if (ctx->inputs[i].glsl_no_index)
         snprintf(ctx->inputs[i].glsl_name, 64, "%s", name_prefix);
      else {
         if (ctx->inputs[i].name == TGSI_SEMANTIC_FOG)
            snprintf(ctx->inputs[i].glsl_name, 64, "%s_f%d", name_prefix, ctx->inputs[i].sid);
         else if (ctx->inputs[i].name == TGSI_SEMANTIC_COLOR)
            snprintf(ctx->inputs[i].glsl_name, 64, "%s_c%d", name_prefix, ctx->inputs[i].sid);
         else if (ctx->inputs[i].name == TGSI_SEMANTIC_GENERIC)
            snprintf(ctx->inputs[i].glsl_name, 64, "%s_g%d", name_prefix, ctx->inputs[i].sid);
         else
            snprintf(ctx->inputs[i].glsl_name, 64, "%s_%d", name_prefix, ctx->inputs[i].first);
      }
      if (add_two_side) {
         snprintf(ctx->inputs[i + 1].glsl_name, 64, "%s_bc%d", name_prefix, ctx->inputs[i + 1].sid);
         if (!ctx->front_face_emitted) {
            snprintf(ctx->inputs[i + 2].glsl_name, 64, "%s", "gl_FrontFacing");
            ctx->front_face_emitted = true;
         }
      }
      break;
   case TGSI_FILE_OUTPUT:
      i = ctx->num_outputs++;
      if (ctx->num_outputs > ARRAY_SIZE(ctx->outputs)) {
         fprintf(stderr, "Number of outputs exceeded, max is %lu\n", ARRAY_SIZE(ctx->outputs));
         return FALSE;
      }

      ctx->outputs[i].name = decl->Semantic.Name;
      ctx->outputs[i].sid = decl->Semantic.Index;
      ctx->outputs[i].interpolate = decl->Interp.Interpolate;
      ctx->outputs[i].invariant = decl->Declaration.Invariant;
      ctx->outputs[i].first = decl->Range.First;
      ctx->outputs[i].glsl_predefined_no_emit = false;
      ctx->outputs[i].glsl_no_index = false;
      ctx->outputs[i].override_no_wm = false;
      ctx->outputs[i].is_int = false;

      switch (ctx->outputs[i].name) {
      case TGSI_SEMANTIC_POSITION:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX ||
             iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            if (ctx->outputs[i].first > 0)
               fprintf(stderr,"Illegal position input\n");
            name_prefix = "gl_Position";
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
         } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_FragDepth";
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
         }
         break;
      case TGSI_SEMANTIC_STENCIL:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_FragStencilRefARB";
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_STENCIL_EXPORT);
         }
         break;
      case TGSI_SEMANTIC_CLIPDIST:
         name_prefix = "gl_ClipDistance";
         ctx->outputs[i].glsl_predefined_no_emit = true;
         ctx->outputs[i].glsl_no_index = true;
         ctx->num_clip_dist += 4;
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX &&
             ctx->key->gs_present)
            require_glsl_ver(ctx, 150);
         break;
      case TGSI_SEMANTIC_CLIPVERTEX:
         name_prefix = "gl_ClipVertex";
         ctx->outputs[i].glsl_predefined_no_emit = true;
         ctx->outputs[i].glsl_no_index = true;
         ctx->outputs[i].override_no_wm = true;
         if (ctx->glsl_ver_required >= 140)
            ctx->has_clipvertex = true;
         break;
      case TGSI_SEMANTIC_SAMPLEMASK:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_SAMPLE_SHADING);
            name_prefix = "gl_SampleMask";
            break;
         }
         break;
      case TGSI_SEMANTIC_COLOR:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
            if (ctx->glsl_ver_required < 140) {
               ctx->outputs[i].glsl_no_index = true;
               if (ctx->outputs[i].sid == 0)
                  name_prefix = "gl_FrontColor";
               else if (ctx->outputs[i].sid == 1)
                  name_prefix = "gl_FrontSecondaryColor";
            } else
               name_prefix = "ex";
            break;
         }

      case TGSI_SEMANTIC_BCOLOR:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
            if (ctx->glsl_ver_required < 140) {
               ctx->outputs[i].glsl_no_index = true;
               if (ctx->outputs[i].sid == 0)
                  name_prefix = "gl_BackColor";
               else if (ctx->outputs[i].sid == 1)
                  name_prefix = "gl_BackSecondaryColor";
               break;
            } else
               name_prefix = "ex";
            break;
         }
      case TGSI_SEMANTIC_PSIZE:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            name_prefix = "gl_PointSize";
            break;
         } else if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            name_prefix = "gl_PointSize";
            break;
         }
      case TGSI_SEMANTIC_LAYER:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            name_prefix = "gl_Layer";
            break;
         }
      case TGSI_SEMANTIC_PRIMID:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            name_prefix = "gl_PrimitiveID";
            break;
         }
      case TGSI_SEMANTIC_VIEWPORT_INDEX:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            name_prefix = "gl_ViewportIndex";
            if (ctx->glsl_ver_required >= 140)
               ctx->shader_req_bits |= SHADER_REQ_VIEWPORT_IDX;
            break;
         }
      case TGSI_SEMANTIC_GENERIC:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX)
            if (ctx->outputs[i].name == TGSI_SEMANTIC_GENERIC)
               color_offset = -1;
      default:
         name_prefix = get_stage_output_name_prefix(iter->processor.Processor);
         break;
      }

      if (ctx->outputs[i].glsl_no_index)
         snprintf(ctx->outputs[i].glsl_name, 64, "%s", name_prefix);
      else {
         if (ctx->outputs[i].name == TGSI_SEMANTIC_FOG)
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_f%d", name_prefix, ctx->outputs[i].sid);
         else if (ctx->outputs[i].name == TGSI_SEMANTIC_COLOR)
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_c%d", name_prefix, ctx->outputs[i].sid);
         else if (ctx->outputs[i].name == TGSI_SEMANTIC_BCOLOR)
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_bc%d", name_prefix, ctx->outputs[i].sid);
         else if (ctx->outputs[i].name == TGSI_SEMANTIC_GENERIC)
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_g%d", name_prefix, ctx->outputs[i].sid);
         else
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_%d", name_prefix, ctx->outputs[i].first + color_offset);

      }
      break;
   case TGSI_FILE_TEMPORARY:
      if (allocate_temp_range(ctx, decl->Range.First, decl->Range.Last,
                              decl->Array.ArrayID))
         return FALSE;
      break;
   case TGSI_FILE_SAMPLER:
      ctx->samplers_used |= (1 << decl->Range.Last);
      break;
   case TGSI_FILE_SAMPLER_VIEW:
      if (decl->Range.First >= ARRAY_SIZE(ctx->samplers)) {
         fprintf(stderr, "Sampler view exceeded, max is %lu\n", ARRAY_SIZE(ctx->samplers));
         return FALSE;
      }
      ctx->samplers[decl->Range.First].tgsi_sampler_return = decl->SamplerView.ReturnTypeX;
      if (ctx->info.indirect_files & (1 << TGSI_FILE_SAMPLER)) {
         if (ctx->last_sampler_array_idx != -1) {
            if (ctx->sampler_arrays[ctx->last_sampler_array_idx].sview_type == decl->SamplerView.Resource &&
                ctx->sampler_arrays[ctx->last_sampler_array_idx].sview_rtype == decl->SamplerView.ReturnTypeX) {
               ctx->sampler_arrays[ctx->last_sampler_array_idx].last = decl->Range.Last + 1;
            } else {
               ctx->last_sampler_array_idx = add_sampler_array(ctx, decl->Range.First, decl->Range.Last + 1, decl->SamplerView.Resource, decl->SamplerView.ReturnTypeX);
            }
         } else {
            ctx->last_sampler_array_idx = add_sampler_array(ctx, decl->Range.First, decl->Range.Last + 1, decl->SamplerView.Resource, decl->SamplerView.ReturnTypeX);
         }
      } else
      ctx->sviews_used = true;
      break;
   case TGSI_FILE_CONSTANT:
      if (decl->Declaration.Dimension) {
         if (ctx->num_ubo >= ARRAY_SIZE(ctx->ubo_idx)) {
            fprintf(stderr, "Number of uniforms exceeded, max is %lu\n", ARRAY_SIZE(ctx->ubo_idx));
            return FALSE;
         }
         ctx->ubo_idx[ctx->num_ubo] = decl->Dim.Index2D;
         ctx->ubo_sizes[ctx->num_ubo] = decl->Range.Last + 1;
         ctx->num_ubo++;
      } else {
         if (decl->Range.Last) {
            if (decl->Range.Last + 1 > ctx->num_consts)
               ctx->num_consts = decl->Range.Last + 1;
         } else
            ctx->num_consts++;
      }
      break;
   case TGSI_FILE_ADDRESS:
      ctx->num_address = decl->Range.Last + 1;
      break;
   case TGSI_FILE_SYSTEM_VALUE:
      i = ctx->num_system_values++;
      if (ctx->num_system_values > ARRAY_SIZE(ctx->system_values)) {
         fprintf(stderr, "Number of system values exceeded, max is %lu\n", ARRAY_SIZE(ctx->system_values));
         return FALSE;
      }

      ctx->system_values[i].name = decl->Semantic.Name;
      ctx->system_values[i].sid = decl->Semantic.Index;
      ctx->system_values[i].glsl_predefined_no_emit = true;
      ctx->system_values[i].glsl_no_index = true;
      ctx->system_values[i].override_no_wm = true;
      ctx->system_values[i].first = decl->Range.First;
      if (decl->Semantic.Name == TGSI_SEMANTIC_INSTANCEID) {
         name_prefix = "gl_InstanceID";
         ctx->shader_req_bits |= SHADER_REQ_INSTANCE_ID;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_VERTEXID) {
         name_prefix = "gl_VertexID";
         ctx->shader_req_bits |= SHADER_REQ_INTS;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_SAMPLEID) {
         name_prefix = "gl_SampleID";
         ctx->shader_req_bits |= (SHADER_REQ_SAMPLE_SHADING | SHADER_REQ_INTS);
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_SAMPLEPOS) {
         name_prefix = "gl_SamplePosition";
         ctx->shader_req_bits |= SHADER_REQ_SAMPLE_SHADING;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_INVOCATIONID) {
         name_prefix = "gl_InvocationID";
         ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_GPU_SHADER5);
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_SAMPLEMASK) {
         name_prefix = "gl_SampleMaskIn[0]";
         ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_GPU_SHADER5);
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_PRIMID) {
         name_prefix = "gl_PrimitiveID";
         ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_GPU_SHADER5);
      } else {
         fprintf(stderr, "unsupported system value %d\n", decl->Semantic.Name);
         name_prefix = "unknown";
      }
      snprintf(ctx->system_values[i].glsl_name, 64, "%s", name_prefix);
      break;
   default:
      fprintf(stderr,"unsupported file %d declaration\n", decl->Declaration.File);
      break;
   }

   return TRUE;
}

static boolean
iter_property(struct tgsi_iterate_context *iter,
              struct tgsi_full_property *prop)
{
   struct dump_ctx *ctx = (struct dump_ctx *) iter;

   if (prop->Property.PropertyName == TGSI_PROPERTY_FS_COLOR0_WRITES_ALL_CBUFS) {
      if (prop->u[0].Data == 1)
         ctx->write_all_cbufs = true;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_FS_COORD_ORIGIN) {
      ctx->fs_coord_origin = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_FS_COORD_PIXEL_CENTER) {
      ctx->fs_pixel_center = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_GS_INPUT_PRIM) {
      ctx->gs_in_prim = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_GS_OUTPUT_PRIM) {
      ctx->gs_out_prim = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_GS_MAX_OUTPUT_VERTICES) {
      ctx->gs_max_out_verts = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_GS_INVOCATIONS) {
      ctx->gs_num_invocations = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_NUM_CLIPDIST_ENABLED) {
      ctx->num_clip_dist_prop = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_NUM_CULLDIST_ENABLED) {
      ctx->num_cull_dist_prop = prop->u[0].Data;
   }
   return TRUE;
}

static boolean
iter_immediate(
   struct tgsi_iterate_context *iter,
   struct tgsi_full_immediate *imm )
{
   struct dump_ctx *ctx = (struct dump_ctx *) iter;
   int i;
   int first = ctx->num_imm;

   if (first >= ARRAY_SIZE(ctx->imm)) {
      fprintf(stderr, "Number of immediates exceeded, max is: %lu\n", ARRAY_SIZE(ctx->imm));
      return FALSE;
   }

   ctx->imm[first].type = imm->Immediate.DataType;
   for (i = 0; i < 4; i++) {
      if (imm->Immediate.DataType == TGSI_IMM_FLOAT32) {
         ctx->imm[first].val[i].f = imm->u[i].Float;
      } else if (imm->Immediate.DataType == TGSI_IMM_UINT32) {
         ctx->shader_req_bits |= SHADER_REQ_INTS;
         ctx->imm[first].val[i].ui  = imm->u[i].Uint;
      } else if (imm->Immediate.DataType == TGSI_IMM_INT32) {
         ctx->shader_req_bits |= SHADER_REQ_INTS;
         ctx->imm[first].val[i].i = imm->u[i].Int;
      }
   }
   ctx->num_imm++;
   return TRUE;
}

static char get_swiz_char(int swiz)
{
   switch(swiz){
   case TGSI_SWIZZLE_X: return 'x';
   case TGSI_SWIZZLE_Y: return 'y';
   case TGSI_SWIZZLE_Z: return 'z';
   case TGSI_SWIZZLE_W: return 'w';
   default: return 0;
   }
}

static int emit_cbuf_writes(struct dump_ctx *ctx)
{
   char buf[255];
   int i;
   char *sret;

   for (i = 1; i < 8; i++) {
      snprintf(buf, 255, "fsout_c%d = fsout_c0;\n", i);
      sret = add_str_to_glsl_main(ctx, buf);
      if (!sret)
         return ENOMEM;
   }
   return 0;
}

static int emit_a8_swizzle(struct dump_ctx *ctx)
{
   char buf[255];
   char *sret;
   snprintf(buf, 255, "fsout_c0.x = fsout_c0.w;\n");
   sret = add_str_to_glsl_main(ctx, buf);
   if (!sret)
      return ENOMEM;
   return 0;
}

static const char *atests[PIPE_FUNC_ALWAYS + 1] = {
   "false",
   "<",
   "==",
   "<=",
   ">",
   "!=",
   ">=",
   "true"
};

static int emit_alpha_test(struct dump_ctx *ctx)
{
   char buf[255];
   char comp_buf[128];
   char *sret;

   if (!ctx->num_outputs)
           return 0;

   if (!ctx->write_all_cbufs) {
           /* only emit alpha stanza if first output is 0 */
           if (ctx->outputs[0].sid != 0)
                   return 0;
   }
   switch (ctx->key->alpha_test) {
   case PIPE_FUNC_NEVER:
   case PIPE_FUNC_ALWAYS:
      snprintf(comp_buf, 128, "%s", atests[ctx->key->alpha_test]);
      break;
   case PIPE_FUNC_LESS:
   case PIPE_FUNC_EQUAL:
   case PIPE_FUNC_LEQUAL:
   case PIPE_FUNC_GREATER:
   case PIPE_FUNC_NOTEQUAL:
   case PIPE_FUNC_GEQUAL:
      snprintf(comp_buf, 128, "%s %s %f", "fsout_c0.w", atests[ctx->key->alpha_test], ctx->key->alpha_ref_val);
      break;
   default:
      fprintf(stderr, "invalid alpha-test: %x\n", ctx->key->alpha_test);
      return EINVAL;
   }

   snprintf(buf, 255, "if (!(%s)) {\n\tdiscard;\n}\n", comp_buf);
   sret = add_str_to_glsl_main(ctx, buf);
   if (!sret)
      return ENOMEM;
   return 0;
}

static int emit_pstipple_pass(struct dump_ctx *ctx)
{
   char buf[255];
   char *sret;
   snprintf(buf, 255, "stip_temp = texture(pstipple_sampler, vec2(gl_FragCoord.x / 32, gl_FragCoord.y / 32)).x;\n");
   sret = add_str_to_glsl_main(ctx, buf);
   if (!sret)
      return ENOMEM;
   snprintf(buf, 255, "if (stip_temp > 0) {\n\tdiscard;\n}\n");
   sret = add_str_to_glsl_main(ctx, buf);
   return sret ? 0 : ENOMEM;
}

static int emit_color_select(struct dump_ctx *ctx)
{
   char buf[255];
   char *sret = NULL;

   if (!ctx->key->color_two_side || !(ctx->color_in_mask & 0x3))
      return 0;

   if (ctx->color_in_mask & 1) {
      snprintf(buf, 255, "realcolor0 = gl_FrontFacing ? ex_c0 : ex_bc0;\n");
      sret = add_str_to_glsl_main(ctx, buf);
   }
   if (ctx->color_in_mask & 2) {
      snprintf(buf, 255, "realcolor1 = gl_FrontFacing ? ex_c1 : ex_bc1;\n");
      sret = add_str_to_glsl_main(ctx, buf);
   }
   return sret ? 0 : ENOMEM;
}

static int emit_prescale(struct dump_ctx *ctx)
{
   char buf[255];
   char *sret;

   snprintf(buf, 255, "gl_Position.y = gl_Position.y * winsys_adjust_y;\n");
   sret = add_str_to_glsl_main(ctx, buf);
   if (!sret)
      return ENOMEM;
   return 0;
}

static int prepare_so_movs(struct dump_ctx *ctx)
{
   int i;
   for (i = 0; i < ctx->so->num_outputs; i++) {
      ctx->write_so_outputs[i] = true;
      if (ctx->so->output[i].start_component != 0)
         continue;
      if (ctx->so->output[i].num_components != 4)
         continue;
      if (ctx->outputs[ctx->so->output[i].register_index].name == TGSI_SEMANTIC_CLIPDIST)
         continue;
      if (ctx->outputs[ctx->so->output[i].register_index].name == TGSI_SEMANTIC_POSITION)
         continue;

      ctx->outputs[ctx->so->output[i].register_index].stream = ctx->so->output[i].stream;
      if (ctx->prog_type == TGSI_PROCESSOR_GEOMETRY && ctx->so->output[i].stream)
         ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;

      ctx->write_so_outputs[i] = false;
   }
   return 0;
}

static int emit_so_movs(struct dump_ctx *ctx)
{
   char buf[255];
   int i, j;
   char outtype[15] = {0};
   char writemask[6];
   char *sret;

   if (ctx->so->num_outputs >= PIPE_MAX_SO_OUTPUTS) {
      fprintf(stderr, "Num outputs exceeded, max is %u\n", PIPE_MAX_SO_OUTPUTS);
      return EINVAL;
   }

   for (i = 0; i < ctx->so->num_outputs; i++) {
      if (ctx->so->output[i].start_component != 0) {
         int wm_idx = 0;
         writemask[wm_idx++] = '.';
         for (j = 0; j < ctx->so->output[i].num_components; j++) {
            unsigned idx = ctx->so->output[i].start_component + j;
            if (idx >= 4)
               break;
            if (idx <= 2)
               writemask[wm_idx++] = 'x' + idx;
            else
               writemask[wm_idx++] = 'w';
         }
         writemask[wm_idx] = '\0';
      } else
         writemask[0] = 0;

      if (!ctx->write_so_outputs[i]) {
         if (ctx->so->output[i].register_index > ctx->num_outputs)
            ctx->so_names[i] = NULL;
         else if (ctx->outputs[ctx->so->output[i].register_index].name == TGSI_SEMANTIC_CLIPVERTEX && ctx->has_clipvertex) {
            ctx->so_names[i] = strdup("clipv_tmp");
            ctx->has_clipvertex_so = true;
         } else
            ctx->so_names[i] = strdup(ctx->outputs[ctx->so->output[i].register_index].glsl_name);
      } else {
         char ntemp[8];
         snprintf(ntemp, 8, "tfout%d", i);
         ctx->so_names[i] = strdup(ntemp);
      }
      if (ctx->so->output[i].num_components == 1) {
         if (ctx->outputs[ctx->so->output[i].register_index].is_int)
            snprintf(outtype, 15, "intBitsToFloat");
         else
            snprintf(outtype, 15, "float");
      } else
         snprintf(outtype, 15, "vec%d", ctx->so->output[i].num_components);

      if (ctx->so->output[i].register_index >= 255)
         continue;

      buf[0] = 0;
      if (ctx->outputs[ctx->so->output[i].register_index].name == TGSI_SEMANTIC_CLIPDIST) {
         snprintf(buf, 255, "tfout%d = %s(clip_dist_temp[%d]%s);\n", i, outtype, ctx->outputs[ctx->so->output[i].register_index].sid,
                  writemask);
      } else {
         if (ctx->write_so_outputs[i])
            snprintf(buf, 255, "tfout%d = %s(%s%s);\n", i, outtype, ctx->outputs[ctx->so->output[i].register_index].glsl_name, writemask);
      }
      sret = add_str_to_glsl_main(ctx, buf);
      if (!sret)
         return ENOMEM;
   }
   return 0;
}

static int emit_clip_dist_movs(struct dump_ctx *ctx)
{
   char buf[255];
   int i;
   char *sret;
   bool has_prop = (ctx->num_clip_dist_prop + ctx->num_cull_dist_prop) > 0;
   int ndists;
   if (ctx->num_clip_dist == 0 && ctx->key->clip_plane_enable) {
      for (i = 0; i < 8; i++) {
         snprintf(buf, 255, "gl_ClipDistance[%d] = dot(%s, clipp[%d]);\n", i, ctx->has_clipvertex ? "clipv_tmp" : "gl_Position", i);
         sret = add_str_to_glsl_main(ctx, buf);
         if (!sret)
            return ENOMEM;
      }
      return 0;
   }
   ndists = ctx->num_clip_dist;
   if (has_prop)
      ndists = ctx->num_clip_dist_prop + ctx->num_cull_dist_prop;
   for (i = 0; i < ndists; i++) {
      int clipidx = i < 4 ? 0 : 1;
      char swiz = i & 3;
      char wm = 0;
      switch (swiz) {
      case 0: wm = 'x'; break;
      case 1: wm = 'y'; break;
      case 2: wm = 'z'; break;
      case 3: wm = 'w'; break;
      default:
         return EINVAL;
      }
      bool is_cull = false;
      if (has_prop) {
         if (i >= ctx->num_clip_dist_prop && i < ctx->num_clip_dist_prop + ctx->num_cull_dist_prop)
            is_cull = true;
      }
      const char *clip_cull = is_cull ? "Cull" : "Clip";
      snprintf(buf, 255, "gl_%sDistance[%d] = clip_dist_temp[%d].%c;\n", clip_cull,
               is_cull ? i - ctx->num_clip_dist_prop : i, clipidx, wm);
      sret = add_str_to_glsl_main(ctx, buf);
      if (!sret)
         return ENOMEM;
   }
   return 0;
}

#define emit_arit_op2(op) snprintf(buf, 255, "%s = %s(%s((%s %s %s))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], op, srcs[1], writemask)
#define emit_op1(op) snprintf(buf, 255, "%s = %s(%s(%s(%s))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), op, srcs[0], writemask)
#define emit_compare(op) snprintf(buf, 255, "%s = %s(%s((%s(%s(%s), %s(%s))))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), op, get_string(svec4), srcs[0], get_string(svec4), srcs[1], writemask)

#define emit_ucompare(op) snprintf(buf, 255, "%s = %s(uintBitsToFloat(%s(%s(%s(%s), %s(%s))%s) * %s(0xffffffff)));\n", dsts[0], get_string(dstconv), get_string(udstconv), op, get_string(svec4), srcs[0], get_string(svec4), srcs[1], writemask, get_string(udstconv))

static int emit_buf(struct dump_ctx *ctx, const char *buf)
{
   int i;
   char *sret;
   for (i = 0; i < ctx->indent_level; i++) {
      sret = add_str_to_glsl_main(ctx, "\t");
      if (!sret)
         return ENOMEM;
   }

   sret = add_str_to_glsl_main(ctx, buf);
   return sret ? 0 : ENOMEM;
}

#define EMIT_BUF_WITH_RET(ctx, buf) do {        \
      int _ret = emit_buf((ctx), (buf));                \
      if (_ret) return FALSE;                        \
   } while(0)

static int handle_vertex_proc_exit(struct dump_ctx *ctx)
{
    if (ctx->so && !ctx->key->gs_present) {
       if (emit_so_movs(ctx))
          return FALSE;
    }

    if (emit_clip_dist_movs(ctx))
       return FALSE;

    if (!ctx->key->gs_present) {
       if (emit_prescale(ctx))
          return FALSE;
    }

    return TRUE;
}

static int handle_fragment_proc_exit(struct dump_ctx *ctx)
{
    if (ctx->key->pstipple_tex) {
       if (emit_pstipple_pass(ctx))
          return FALSE;
    }

    if (ctx->key->cbufs_are_a8_bitmask) {
       if (emit_a8_swizzle(ctx))
          return FALSE;
    }

    if (ctx->key->add_alpha_test) {
       if (emit_alpha_test(ctx))
          return FALSE;
    }

    if (ctx->write_all_cbufs) {
       if (emit_cbuf_writes(ctx))
          return FALSE;
    }

    return TRUE;
}

/* size queries are pretty much separate */
static int emit_txq(struct dump_ctx *ctx,
                    struct tgsi_full_instruction *inst,
                    char srcs[4][255],
                    char dsts[3][255],
                    const char *writemask)
{
   const char *twm = "";
   char bias[128] = {0};
   char buf[512];
   const int sampler_index = 1;
   enum vrend_type_qualifier dtypeprefix = INT_BITS_TO_FLOAT;
   /* no lod parameter for txq for these */
   if (inst->Texture.Texture != TGSI_TEXTURE_RECT &&
       inst->Texture.Texture != TGSI_TEXTURE_SHADOWRECT &&
       inst->Texture.Texture != TGSI_TEXTURE_BUFFER &&
       inst->Texture.Texture != TGSI_TEXTURE_2D_MSAA &&
       inst->Texture.Texture != TGSI_TEXTURE_2D_ARRAY_MSAA)
      snprintf(bias, 128, ", int(%s.w)", srcs[0]);

   /* need to emit a textureQueryLevels */
   if (inst->Dst[0].Register.WriteMask & 0x8) {

      if (inst->Texture.Texture != TGSI_TEXTURE_BUFFER &&
          inst->Texture.Texture != TGSI_TEXTURE_RECT &&
          inst->Texture.Texture != TGSI_TEXTURE_2D_MSAA &&
          inst->Texture.Texture != TGSI_TEXTURE_2D_ARRAY_MSAA) {
         ctx->shader_req_bits |= SHADER_REQ_TXQ_LEVELS;
         if (inst->Dst[0].Register.WriteMask & 0x7)
            twm = ".w";
         snprintf(buf, 255, "%s%s = %s(textureQueryLevels(%s));\n", dsts[0], twm, get_string(dtypeprefix), srcs[sampler_index]);
         EMIT_BUF_WITH_RET(ctx, buf);
      }

      if (inst->Dst[0].Register.WriteMask & 0x7) {
         switch (inst->Texture.Texture) {
         case TGSI_TEXTURE_1D:
         case TGSI_TEXTURE_BUFFER:
         case TGSI_TEXTURE_SHADOW1D:
            twm = ".x";
            break;
         case TGSI_TEXTURE_1D_ARRAY:
         case TGSI_TEXTURE_SHADOW1D_ARRAY:
         case TGSI_TEXTURE_2D:
         case TGSI_TEXTURE_SHADOW2D:
         case TGSI_TEXTURE_RECT:
         case TGSI_TEXTURE_SHADOWRECT:
         case TGSI_TEXTURE_CUBE:
         case TGSI_TEXTURE_SHADOWCUBE:
         case TGSI_TEXTURE_2D_MSAA:
            twm = ".xy";
            break;
         case TGSI_TEXTURE_3D:
         case TGSI_TEXTURE_2D_ARRAY:
         case TGSI_TEXTURE_SHADOW2D_ARRAY:
         case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
         case TGSI_TEXTURE_CUBE_ARRAY:
         case TGSI_TEXTURE_2D_ARRAY_MSAA:
            twm = ".xyz";
            break;
         }
      }
   }

   if (inst->Dst[0].Register.WriteMask & 0x7) {
      snprintf(buf, 255, "%s%s = %s(textureSize(%s%s))%s;\n", dsts[0], twm, get_string(dtypeprefix), srcs[sampler_index], bias, util_bitcount(inst->Dst[0].Register.WriteMask) > 1 ? writemask : "");
      EMIT_BUF_WITH_RET(ctx, buf);
   }
   return 0;
}

static int translate_tex(struct dump_ctx *ctx,
                         struct tgsi_full_instruction *inst,
                         int sreg_index,
                         char srcs[4][255],
                         char dsts[3][255],
                         const char *writemask,
                         const char *dstconv,
                         bool dst0_override_no_wm,
                         bool tg4_has_component)
{
   const char *twm = "", *gwm = NULL, *txfi;
   enum vrend_type_qualifier dtypeprefix = TYPE_CONVERSION_NONE;
   bool is_shad = false;
   char buf[512];
   char offbuf[128] = {0};
   char bias[128] = {0};
   int sampler_index;
   const char *tex_ext;

   if (sreg_index >= ARRAY_SIZE(ctx->samplers)) {
      fprintf(stderr, "Sampler view exceeded, max is %lu\n", ARRAY_SIZE(ctx->samplers));
      return FALSE;
   }

   ctx->samplers[sreg_index].tgsi_sampler_type = inst->Texture.Texture;

   switch (ctx->samplers[sreg_index].tgsi_sampler_return) {
   case TGSI_RETURN_TYPE_SINT:
      /* if dstconv isn't an int */
      if (strcmp(dstconv, "int"))
         dtypeprefix = INT_BITS_TO_FLOAT;
      break;
   case TGSI_RETURN_TYPE_UINT:
      /* if dstconv isn't an int */
      if (strcmp(dstconv, "int"))
         dtypeprefix = UINT_BITS_TO_FLOAT;
      break;
   default:
      break;
   }

   switch (inst->Texture.Texture) {
   case TGSI_TEXTURE_1D:
   case TGSI_TEXTURE_2D:
   case TGSI_TEXTURE_3D:
   case TGSI_TEXTURE_CUBE:
   case TGSI_TEXTURE_1D_ARRAY:
   case TGSI_TEXTURE_2D_ARRAY:
      break;
   case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
      is_shad = true;
   case TGSI_TEXTURE_CUBE_ARRAY:
      ctx->shader_req_bits |= SHADER_REQ_CUBE_ARRAY;
      break;
   case TGSI_TEXTURE_2D_MSAA:
   case TGSI_TEXTURE_2D_ARRAY_MSAA:
      ctx->shader_req_bits |= SHADER_REQ_SAMPLER_MS;
      break;
   case TGSI_TEXTURE_BUFFER:
      ctx->uses_sampler_buf = true;
      break;
   case TGSI_TEXTURE_SHADOWRECT:
      is_shad = true;
   case TGSI_TEXTURE_RECT:
      ctx->shader_req_bits |= SHADER_REQ_SAMPLER_RECT;
      break;
   case TGSI_TEXTURE_SHADOW1D:
   case TGSI_TEXTURE_SHADOW2D:
   case TGSI_TEXTURE_SHADOWCUBE:
   case TGSI_TEXTURE_SHADOW1D_ARRAY:
   case TGSI_TEXTURE_SHADOW2D_ARRAY:
      is_shad = true;
      break;
   default:
      fprintf(stderr, "unhandled texture: %x\n", inst->Texture.Texture);
      return false;
   }

   if (ctx->cfg->glsl_version >= 140)
      if ((ctx->shader_req_bits & SHADER_REQ_SAMPLER_RECT) || ctx->uses_sampler_buf)
         require_glsl_ver(ctx, 140);

   sampler_index = 1;

   if (inst->Instruction.Opcode == TGSI_OPCODE_LODQ)
      ctx->shader_req_bits |= SHADER_REQ_LODQ;

   if (inst->Instruction.Opcode == TGSI_OPCODE_TXQ) {
      return emit_txq(ctx, inst, srcs, dsts, writemask);
   }

   switch (inst->Texture.Texture) {
   case TGSI_TEXTURE_1D:
   case TGSI_TEXTURE_BUFFER:
      if (inst->Instruction.Opcode == TGSI_OPCODE_TXP)
         twm = "";
      else
         twm = ".x";
      txfi = "int";
      break;
   case TGSI_TEXTURE_1D_ARRAY:
      twm = ".xy";
      txfi = "ivec2";
      break;
   case TGSI_TEXTURE_2D:
   case TGSI_TEXTURE_RECT:
      if (inst->Instruction.Opcode == TGSI_OPCODE_TXP)
         twm = "";
      else
         twm = ".xy";
      txfi = "ivec2";
      break;
   case TGSI_TEXTURE_SHADOW1D:
   case TGSI_TEXTURE_SHADOW2D:
   case TGSI_TEXTURE_SHADOW1D_ARRAY:
   case TGSI_TEXTURE_SHADOWRECT:
   case TGSI_TEXTURE_3D:
      if (inst->Instruction.Opcode == TGSI_OPCODE_TXP)
         twm = "";
      else if (inst->Instruction.Opcode == TGSI_OPCODE_TG4)
         twm = ".xy";
      else
         twm = ".xyz";
      txfi = "ivec3";
      break;
   case TGSI_TEXTURE_CUBE:
   case TGSI_TEXTURE_2D_ARRAY:
      twm = ".xyz";
      txfi = "ivec3";
      break;
   case TGSI_TEXTURE_2D_MSAA:
      twm = ".xy";
      txfi = "ivec2";
      break;
   case TGSI_TEXTURE_2D_ARRAY_MSAA:
      twm = ".xyz";
      txfi = "ivec3";
      break;

   case TGSI_TEXTURE_SHADOWCUBE:
   case TGSI_TEXTURE_SHADOW2D_ARRAY:
   case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
   case TGSI_TEXTURE_CUBE_ARRAY:
   default:
      if (inst->Instruction.Opcode == TGSI_OPCODE_TG4 &&
          inst->Texture.Texture != TGSI_TEXTURE_CUBE_ARRAY
          && inst->Texture.Texture != TGSI_TEXTURE_SHADOWCUBE_ARRAY)
         twm = ".xyz";
      else
         twm = "";
      txfi = "";
      break;
   }

   if (inst->Instruction.Opcode == TGSI_OPCODE_TXD) {
      switch (inst->Texture.Texture) {
      case TGSI_TEXTURE_1D:
      case TGSI_TEXTURE_SHADOW1D:
      case TGSI_TEXTURE_1D_ARRAY:
      case TGSI_TEXTURE_SHADOW1D_ARRAY:
         gwm = ".x";
         break;
      case TGSI_TEXTURE_2D:
      case TGSI_TEXTURE_SHADOW2D:
      case TGSI_TEXTURE_2D_ARRAY:
      case TGSI_TEXTURE_SHADOW2D_ARRAY:
      case TGSI_TEXTURE_RECT:
      case TGSI_TEXTURE_SHADOWRECT:
         gwm = ".xy";
         break;
      case TGSI_TEXTURE_3D:
      case TGSI_TEXTURE_CUBE:
      case TGSI_TEXTURE_SHADOWCUBE:
      case TGSI_TEXTURE_CUBE_ARRAY:
         gwm = ".xyz";
         break;
      default:
         gwm = "";
         break;
      }
   }

   if (inst->Instruction.Opcode == TGSI_OPCODE_TXB2 || inst->Instruction.Opcode == TGSI_OPCODE_TXL2 || inst->Instruction.Opcode == TGSI_OPCODE_TEX2) {
      sampler_index = 2;
      if (inst->Instruction.Opcode != TGSI_OPCODE_TEX2)
         snprintf(bias, 64, ", %s.x", srcs[1]);
      else if (inst->Texture.Texture == TGSI_TEXTURE_SHADOWCUBE_ARRAY)
         snprintf(bias, 64, ", float(%s)", srcs[1]);
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXB || inst->Instruction.Opcode == TGSI_OPCODE_TXL)
      snprintf(bias, 64, ", %s.w", srcs[0]);
   else if (inst->Instruction.Opcode == TGSI_OPCODE_TXF) {
      if (inst->Texture.Texture == TGSI_TEXTURE_1D ||
          inst->Texture.Texture == TGSI_TEXTURE_2D ||
          inst->Texture.Texture == TGSI_TEXTURE_2D_MSAA ||
          inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY_MSAA ||
          inst->Texture.Texture == TGSI_TEXTURE_3D ||
          inst->Texture.Texture == TGSI_TEXTURE_1D_ARRAY ||
          inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY) {
         snprintf(bias, 64, ", int(%s.w)", srcs[0]);
      }
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXD) {
      snprintf(bias, 128, ", %s%s, %s%s", srcs[1], gwm, srcs[2], gwm);
      sampler_index = 3;
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TG4) {
      sampler_index = 2;
      ctx->shader_req_bits |= SHADER_REQ_TG4;
      if (inst->Texture.NumOffsets > 1 || is_shad || (ctx->shader_req_bits & SHADER_REQ_SAMPLER_RECT))
         ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      if (inst->Texture.NumOffsets == 1) {
         if (inst->TexOffsets[0].File != TGSI_FILE_IMMEDIATE)
            ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      }
      if (is_shad) {
         if (inst->Texture.Texture == TGSI_TEXTURE_SHADOWCUBE ||
             inst->Texture.Texture == TGSI_TEXTURE_SHADOW2D_ARRAY)
            snprintf(bias, 64, ", %s.w", srcs[0]);
         else if (inst->Texture.Texture == TGSI_TEXTURE_SHADOWCUBE_ARRAY)
            snprintf(bias, 64, ", %s.x", srcs[1]);
         else
            snprintf(bias, 64, ", %s.z", srcs[0]);
      } else if (tg4_has_component) {
         if (inst->Texture.NumOffsets == 0) {
            if (inst->Texture.Texture == TGSI_TEXTURE_2D ||
                inst->Texture.Texture == TGSI_TEXTURE_RECT ||
                inst->Texture.Texture == TGSI_TEXTURE_CUBE ||
                inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY ||
                inst->Texture.Texture == TGSI_TEXTURE_CUBE_ARRAY)
               snprintf(bias, 64, ", int(%s)", srcs[1]);
         } else if (inst->Texture.NumOffsets) {
            if (inst->Texture.Texture == TGSI_TEXTURE_2D ||
                inst->Texture.Texture == TGSI_TEXTURE_RECT ||
                inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY)
               snprintf(bias, 64, ", int(%s)", srcs[1]);
         }
      }
   } else
      bias[0] = 0;

   if (inst->Instruction.Opcode == TGSI_OPCODE_LODQ) {
      tex_ext = "QueryLOD";
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXP) {
      if (inst->Texture.Texture == TGSI_TEXTURE_CUBE || inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY || inst->Texture.Texture == TGSI_TEXTURE_1D_ARRAY)
         tex_ext = "";
      else if (inst->Texture.NumOffsets == 1)
         tex_ext = "ProjOffset";
      else
         tex_ext = "Proj";
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXL || inst->Instruction.Opcode == TGSI_OPCODE_TXL2) {
      if (inst->Texture.NumOffsets == 1)
         tex_ext = "LodOffset";
      else
         tex_ext = "Lod";
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXD) {
      if (inst->Texture.NumOffsets == 1)
         tex_ext = "GradOffset";
      else
         tex_ext = "Grad";
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TG4) {
      if (inst->Texture.NumOffsets == 4)
         tex_ext = "GatherOffsets";
      else if (inst->Texture.NumOffsets == 1)
         tex_ext = "GatherOffset";
      else
         tex_ext = "Gather";
   } else {
      if (inst->Texture.NumOffsets == 1)
         tex_ext = "Offset";
      else
         tex_ext = "";
   }

   if (inst->Texture.NumOffsets == 1) {
      if (inst->TexOffsets[0].Index >= ARRAY_SIZE(ctx->imm)) {
         fprintf(stderr, "Immediate exceeded, max is %lu\n", ARRAY_SIZE(ctx->imm));
         return false;
      }
      if (inst->TexOffsets[0].File == TGSI_FILE_IMMEDIATE) {
         struct immed *imd = &ctx->imm[inst->TexOffsets[0].Index];
         switch (inst->Texture.Texture) {
         case TGSI_TEXTURE_1D:
         case TGSI_TEXTURE_1D_ARRAY:
         case TGSI_TEXTURE_SHADOW1D:
         case TGSI_TEXTURE_SHADOW1D_ARRAY:
            snprintf(offbuf, 25, ", int(%d)", imd->val[inst->TexOffsets[0].SwizzleX].i);
            break;
         case TGSI_TEXTURE_RECT:
         case TGSI_TEXTURE_SHADOWRECT:
         case TGSI_TEXTURE_2D:
         case TGSI_TEXTURE_2D_ARRAY:
         case TGSI_TEXTURE_SHADOW2D:
         case TGSI_TEXTURE_SHADOW2D_ARRAY:
            snprintf(offbuf, 25, ", ivec2(%d, %d)", imd->val[inst->TexOffsets[0].SwizzleX].i, imd->val[inst->TexOffsets[0].SwizzleY].i);
            break;
         case TGSI_TEXTURE_3D:
            snprintf(offbuf, 25, ", ivec3(%d, %d, %d)", imd->val[inst->TexOffsets[0].SwizzleX].i, imd->val[inst->TexOffsets[0].SwizzleY].i,
                     imd->val[inst->TexOffsets[0].SwizzleZ].i);
            break;
         default:
            fprintf(stderr, "unhandled texture: %x\n", inst->Texture.Texture);
            return false;
         }
      } else if (inst->TexOffsets[0].File == TGSI_FILE_TEMPORARY) {
         struct vrend_temp_range *range = find_temp_range(ctx, inst->TexOffsets[0].Index);
         int idx = inst->TexOffsets[0].Index - range->first;
         switch (inst->Texture.Texture) {
         case TGSI_TEXTURE_1D:
         case TGSI_TEXTURE_1D_ARRAY:
         case TGSI_TEXTURE_SHADOW1D:
         case TGSI_TEXTURE_SHADOW1D_ARRAY:
            snprintf(offbuf, 120, ", int(floatBitsToInt(temp%d[%d].%c))",
                     range->first, idx,
                     get_swiz_char(inst->TexOffsets[0].SwizzleX));
            break;
         case TGSI_TEXTURE_RECT:
         case TGSI_TEXTURE_SHADOWRECT:
         case TGSI_TEXTURE_2D:
         case TGSI_TEXTURE_2D_ARRAY:
         case TGSI_TEXTURE_SHADOW2D:
         case TGSI_TEXTURE_SHADOW2D_ARRAY:
            snprintf(offbuf, 120, ", ivec2(floatBitsToInt(temp%d[%d].%c), floatBitsToInt(temp%d[%d].%c))",
                     range->first, idx,
                     get_swiz_char(inst->TexOffsets[0].SwizzleX),
                     range->first, idx,
                     get_swiz_char(inst->TexOffsets[0].SwizzleY));
            break;
         case TGSI_TEXTURE_3D:
            snprintf(offbuf, 120, ", ivec2(floatBitsToInt(temp%d[%d].%c), floatBitsToInt(temp%d[%d].%c), floatBitsToInt(temp%d[%d].%c)",
                     range->first, idx,
                     get_swiz_char(inst->TexOffsets[0].SwizzleX),
                     range->first, idx,
                     get_swiz_char(inst->TexOffsets[0].SwizzleY),
                     range->first, idx,
                     get_swiz_char(inst->TexOffsets[0].SwizzleZ));
                     break;
         default:
            fprintf(stderr, "unhandled texture: %x\n", inst->Texture.Texture);
            return false;
            break;
         }
      }
      if (inst->Instruction.Opcode == TGSI_OPCODE_TXL || inst->Instruction.Opcode == TGSI_OPCODE_TXL2 || inst->Instruction.Opcode == TGSI_OPCODE_TXD || (inst->Instruction.Opcode == TGSI_OPCODE_TG4 && is_shad)) {
         char tmp[128];
         strcpy(tmp, offbuf);
         strcpy(offbuf, bias);
         strcpy(bias, tmp);
      }
   }
   if (inst->Instruction.Opcode == TGSI_OPCODE_TXF) {
      snprintf(buf, 255, "%s = %s(%s(texelFetch%s(%s, %s(%s%s)%s%s)%s));\n", dsts[0], dstconv, get_string(dtypeprefix), tex_ext, srcs[sampler_index], txfi, srcs[0], twm, bias, offbuf, dst0_override_no_wm ? "" : writemask);
   } else if (ctx->cfg->glsl_version < 140 && (ctx->shader_req_bits & SHADER_REQ_SAMPLER_RECT)) {
      /* rect is special in GLSL 1.30 */
      if (inst->Texture.Texture == TGSI_TEXTURE_RECT)
         snprintf(buf, 255, "%s = texture2DRect(%s, %s.xy)%s;\n", dsts[0], srcs[sampler_index], srcs[0], writemask);
      else if (inst->Texture.Texture == TGSI_TEXTURE_SHADOWRECT)
         snprintf(buf, 255, "%s = shadow2DRect(%s, %s.xyz)%s;\n", dsts[0], srcs[sampler_index], srcs[0], writemask);
   } else if (is_shad && inst->Instruction.Opcode != TGSI_OPCODE_TG4) { /* TGSI returns 1.0 in alpha */
      const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
      const struct tgsi_full_src_register *src = &inst->Src[sampler_index];
      snprintf(buf, 255, "%s = %s(%s(vec4(vec4(texture%s(%s, %s%s%s%s)) * %sshadmask%d + %sshadadd%d)%s));\n", dsts[0], dstconv, get_string(dtypeprefix), tex_ext, srcs[sampler_index], srcs[0], twm, offbuf, bias, cname, src->Register.Index, cname, src->Register.Index, writemask);
   } else {
      /* OpenGL ES do not support 1D texture
       * so we use a 2D texture with a parameter set to 0.5
       */
      if (ctx->cfg->use_gles && inst->Texture.Texture == TGSI_TEXTURE_1D) {
         snprintf(buf, 255, "%s = %s(%s(texture2D(%s, vec2(%s%s%s%s, 0.5))%s));\n", dsts[0], dstconv, get_string(dtypeprefix), srcs[sampler_index], srcs[0], twm, offbuf, bias, dst0_override_no_wm ? "" : writemask);
      } else {
         snprintf(buf, 255, "%s = %s(%s(texture%s(%s, %s%s%s%s)%s));\n", dsts[0], dstconv, get_string(dtypeprefix), tex_ext, srcs[sampler_index], srcs[0], twm, offbuf, bias, dst0_override_no_wm ? "" : writemask);
      }
   }
   return emit_buf(ctx, buf);
}

static void
create_swizzled_clipdist(struct dump_ctx *ctx,
                         char *result,
                         const struct tgsi_full_src_register *src,
                         int input_idx,
                         bool gl_in,
                         const char *stypeprefix,
                         const char *prefix,
                         const char *arrayname)
{
   char clipdistvec[4][64] = {};
   int idx;
   bool has_prev_vals = (ctx->key->prev_stage_num_cull_out + ctx->key->prev_stage_num_clip_out) > 0;
   int num_culls = has_prev_vals ? ctx->key->prev_stage_num_cull_out : 0;
   int num_clips = has_prev_vals ? ctx->key->prev_stage_num_clip_out : ctx->num_in_clip_dist;
   for (unsigned cc = 0; cc < 4; cc++) {
      const char *cc_name = ctx->inputs[input_idx].glsl_name;
      idx = ctx->inputs[input_idx].sid * 4;
      if (cc == 0)
         idx += src->Register.SwizzleX;
      else if (cc == 1)
         idx += src->Register.SwizzleY;
      else if (cc == 2)
         idx += src->Register.SwizzleZ;
      else if (cc == 3)
         idx += src->Register.SwizzleW;

      if (num_culls) {
         if (idx >= num_clips) {
            idx -= num_clips;
            cc_name = "gl_CullDistance";
         }
         if (ctx->key->prev_stage_num_cull_out)
            if (idx >= ctx->key->prev_stage_num_cull_out)
               idx = 0;
      } else {
         if (ctx->key->prev_stage_num_clip_out)
            if (idx >= ctx->key->prev_stage_num_clip_out)
               idx = 0;
      }
      if (gl_in)
         snprintf(clipdistvec[cc], 64, "%sgl_in%s.%s[%d]", prefix, arrayname, cc_name, idx);
      else
         snprintf(clipdistvec[cc], 64, "%s%s%s[%d]", prefix, arrayname, cc_name, idx);
   }
   snprintf(result, 255, "%s(vec4(%s,%s,%s,%s))", stypeprefix, clipdistvec[0], clipdistvec[1], clipdistvec[2], clipdistvec[3]);
}

static int
get_destination_info(struct dump_ctx *ctx,
                     const struct tgsi_full_instruction *inst,
                     enum vrend_type_qualifier *dtypeprefix,
                     enum vrend_type_qualifier *dstconv,
                     enum vrend_type_qualifier *udstconv,
                     bool dst_override_no_wm[2], char dsts[3][255],
                     char *writemask)
{
   const struct tgsi_full_dst_register *dst_reg;
   enum tgsi_opcode_type dtype = tgsi_opcode_infer_dst_type(inst->Instruction.Opcode);

   if (dtype == TGSI_TYPE_SIGNED || dtype == TGSI_TYPE_UNSIGNED)
      ctx->shader_req_bits |= SHADER_REQ_INTS;

   if (inst->Instruction.Opcode == TGSI_OPCODE_TXQ) {
      *dtypeprefix = INT_BITS_TO_FLOAT;
   } else {
      switch (dtype) {
      case TGSI_TYPE_UNSIGNED:
         *dtypeprefix = UINT_BITS_TO_FLOAT;
         break;
      case TGSI_TYPE_SIGNED:
         *dtypeprefix = INT_BITS_TO_FLOAT;
         break;
      default:
         break;
      }
   }

   for (uint32_t i = 0; i < inst->Instruction.NumDstRegs; i++) {
      dst_reg = &inst->Dst[i];
      dst_override_no_wm[i] = false;
      if (dst_reg->Register.WriteMask != TGSI_WRITEMASK_XYZW) {
         int wm_idx = 0;
         writemask[wm_idx++] = '.';
         if (dst_reg->Register.WriteMask & 0x1)
            writemask[wm_idx++] = 'x';
         if (dst_reg->Register.WriteMask & 0x2)
            writemask[wm_idx++] = 'y';
         if (dst_reg->Register.WriteMask & 0x4)
            writemask[wm_idx++] = 'z';
         if (dst_reg->Register.WriteMask & 0x8)
            writemask[wm_idx++] = 'w';

         *dstconv = FLOAT + wm_idx - 2;
         *udstconv = UINT + wm_idx - 2;
      } else {
         *dstconv = VEC4;
         *udstconv = UVEC4;
      }

      if (dst_reg->Register.File == TGSI_FILE_OUTPUT) {
         for (uint32_t j = 0; j < ctx->num_outputs; j++) {
            if (ctx->outputs[j].first == dst_reg->Register.Index) {
               if (ctx->glsl_ver_required >= 140 && ctx->outputs[j].name == TGSI_SEMANTIC_CLIPVERTEX) {
                  snprintf(dsts[i], 255, "clipv_tmp");
               } else if (ctx->outputs[j].name == TGSI_SEMANTIC_CLIPDIST) {
                  snprintf(dsts[i], 255, "clip_dist_temp[%d]", ctx->outputs[j].sid);
               } else if (ctx->outputs[j].name == TGSI_SEMANTIC_SAMPLEMASK) {
                  int idx;
                  switch (dst_reg->Register.WriteMask) {
                  case 0x1: idx = 0; break;
                  case 0x2: idx = 1; break;
                  case 0x4: idx = 2; break;
                  case 0x8: idx = 3; break;
                  default:
                     idx = 0;
                     break;
                  }
                  snprintf(dsts[i], 255, "%s[%d]", ctx->outputs[j].glsl_name, idx);
                  if (ctx->outputs[j].is_int) {
                     *dtypeprefix = FLOAT_BITS_TO_INT;
                     *dstconv = INT;
                  }
               } else {
                  snprintf(dsts[i], 255, "%s%s", ctx->outputs[j].glsl_name, ctx->outputs[j].override_no_wm ? "" : writemask);
                  dst_override_no_wm[i] = ctx->outputs[j].override_no_wm;
                  if (ctx->outputs[j].is_int) {
                     if (*dtypeprefix == TYPE_CONVERSION_NONE)
                        *dtypeprefix = FLOAT_BITS_TO_INT;
                     *dstconv = INT;
                  }
                  if (ctx->outputs[j].name == TGSI_SEMANTIC_PSIZE) {
                     *dstconv = FLOAT;
                     break;
                  }
               }
            }
         }
      }
      else if (dst_reg->Register.File == TGSI_FILE_TEMPORARY) {
         struct vrend_temp_range *range = find_temp_range(ctx, dst_reg->Register.Index);
         if (!range)
            return FALSE;
         if (dst_reg->Register.Indirect) {
            snprintf(dsts[i], 255, "temp%d[addr0 + %d]%s", range->first, dst_reg->Register.Index - range->first, writemask);
         } else
            snprintf(dsts[i], 255, "temp%d[%d]%s", range->first, dst_reg->Register.Index - range->first, writemask);
      }
      else if (dst_reg->Register.File == TGSI_FILE_ADDRESS) {
         snprintf(dsts[i], 255, "addr%d", dst_reg->Register.Index);
      }
   }

   return 0;
}

static int
get_source_info(struct dump_ctx *ctx,
                const struct tgsi_full_instruction *inst,
                enum vrend_type_qualifier *svec4, int *sreg_index,
                char srcs[3][255], char src_swizzle0[10],
                bool *tg4_has_component, bool *override_no_wm)
{
   bool stprefix = false;

   enum vrend_type_qualifier stypeprefix = TYPE_CONVERSION_NONE;
   enum tgsi_opcode_type stype = tgsi_opcode_infer_src_type(inst->Instruction.Opcode);

   if (stype == TGSI_TYPE_SIGNED || stype == TGSI_TYPE_UNSIGNED)
      ctx->shader_req_bits |= SHADER_REQ_INTS;

   switch (stype) {
   case TGSI_TYPE_UNSIGNED:
      stypeprefix = FLOAT_BITS_TO_UINT;
      *svec4 = UVEC4;
      stprefix = true;
      break;
   case TGSI_TYPE_SIGNED:
      stypeprefix = FLOAT_BITS_TO_INT;
      *svec4 = IVEC4;
      stprefix = true;
      break;
   default:
      break;
   }

   for (uint32_t i = 0; i < inst->Instruction.NumSrcRegs; i++) {
      const struct tgsi_full_src_register *src = &inst->Src[i];
      char swizzle[8] = {0};
      char prefix[6] = {0};
      char arrayname[16] = {0};
      int swz_idx = 0, pre_idx = 0;
      boolean isabsolute = src->Register.Absolute;

      override_no_wm[i] = false;
      if (isabsolute)
         swizzle[swz_idx++] = ')';

      if (src->Register.Negate)
         prefix[pre_idx++] = '-';
      if (isabsolute)
         strcpy(&prefix[pre_idx++], "abs(");

      if (src->Register.Dimension) {
         if (src->Dimension.Indirect) {
            assert(src->DimIndirect.File == TGSI_FILE_ADDRESS);
            sprintf(arrayname, "[addr%d]", src->DimIndirect.Index);
         } else
            sprintf(arrayname, "[%d]", src->Dimension.Index);
      }

      if (src->Register.SwizzleX != TGSI_SWIZZLE_X ||
          src->Register.SwizzleY != TGSI_SWIZZLE_Y ||
          src->Register.SwizzleZ != TGSI_SWIZZLE_Z ||
          src->Register.SwizzleW != TGSI_SWIZZLE_W) {
         swizzle[swz_idx++] = '.';
         swizzle[swz_idx++] = get_swiz_char(src->Register.SwizzleX);
         swizzle[swz_idx++] = get_swiz_char(src->Register.SwizzleY);
         swizzle[swz_idx++] = get_swiz_char(src->Register.SwizzleZ);
         swizzle[swz_idx++] = get_swiz_char(src->Register.SwizzleW);
      }
      if (src->Register.File == TGSI_FILE_INPUT) {
         for (uint32_t j = 0; j < ctx->num_inputs; j++)
            if (ctx->inputs[j].first == src->Register.Index) {
               if (ctx->key->color_two_side && ctx->inputs[j].name == TGSI_SEMANTIC_COLOR)
                  snprintf(srcs[i], 255, "%s(%s%s%d%s%s)", get_string(stypeprefix), prefix, "realcolor", ctx->inputs[j].sid, arrayname, swizzle);
               else if (ctx->inputs[j].glsl_gl_in) {
                  /* GS input clipdist requires a conversion */
                  if (ctx->inputs[j].name == TGSI_SEMANTIC_CLIPDIST) {
                     create_swizzled_clipdist(ctx, srcs[i], src, j, true, get_string(stypeprefix), prefix, arrayname);
                  } else {
                     snprintf(srcs[i], 255, "%s(vec4(%sgl_in%s.%s)%s)", get_string(stypeprefix), prefix, arrayname, ctx->inputs[j].glsl_name, swizzle);
                  }
               }
               else if (ctx->inputs[j].name == TGSI_SEMANTIC_PRIMID)
                  snprintf(srcs[i], 255, "%s(vec4(intBitsToFloat(%s)))", get_string(stypeprefix), ctx->inputs[j].glsl_name);
               else if (ctx->inputs[j].name == TGSI_SEMANTIC_FACE)
                  snprintf(srcs[i], 255, "%s(%s ? 1.0 : -1.0)", get_string(stypeprefix), ctx->inputs[j].glsl_name);
               else if (ctx->inputs[j].name == TGSI_SEMANTIC_CLIPDIST) {
                  create_swizzled_clipdist(ctx, srcs[i], src, j, false, get_string(stypeprefix), prefix, arrayname);
               } else {
                  enum vrend_type_qualifier srcstypeprefix = stypeprefix;
                  if (stype == TGSI_TYPE_UNSIGNED &&
                      ctx->inputs[j].is_int)
                     srcstypeprefix = TYPE_CONVERSION_NONE;

                  if (inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE && i == 1) {
                     snprintf(srcs[i], 255, "floatBitsToInt(%s%s%s%s)", prefix, ctx->inputs[j].glsl_name, arrayname, swizzle);
                  } else
                     snprintf(srcs[i], 255, "%s(%s%s%s%s)", get_string(srcstypeprefix), prefix, ctx->inputs[j].glsl_name, arrayname, ctx->inputs[j].is_int ? "" : swizzle);
               }
               if ((inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE ||
                    inst->Instruction.Opcode == TGSI_OPCODE_INTERP_OFFSET ||
                    inst->Instruction.Opcode == TGSI_OPCODE_INTERP_CENTROID) &&
                   i == 0) {
                  snprintf(srcs[0], 255, "%s", ctx->inputs[j].glsl_name);
                  snprintf(src_swizzle0, 10, "%s", swizzle);
               }
               override_no_wm[i] = ctx->inputs[j].override_no_wm;
               break;
            }
      }
      else if (src->Register.File == TGSI_FILE_TEMPORARY) {
         struct vrend_temp_range *range = find_temp_range(ctx, src->Register.Index);
         if (!range)
            return FALSE;
         if (inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE && i == 1) {
            stprefix = true;
            stypeprefix = FLOAT_BITS_TO_INT;
         }

         if (src->Register.Indirect) {
            assert(src->Indirect.File == TGSI_FILE_ADDRESS);
            snprintf(srcs[i], 255, "%s%c%stemp%d[addr%d + %d]%s%c", get_string(stypeprefix), stprefix ? '(' : ' ', prefix, range->first, src->Indirect.Index, src->Register.Index - range->first, swizzle, stprefix ? ')' : ' ');
         } else
            snprintf(srcs[i], 255, "%s%c%stemp%d[%d]%s%c", get_string(stypeprefix), stprefix ? '(' : ' ', prefix, range->first, src->Register.Index - range->first, swizzle, stprefix ? ')' : ' ');
      } else if (src->Register.File == TGSI_FILE_CONSTANT) {
         const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
         int dim = 0;
         if (src->Register.Dimension) {
            dim = src->Dimension.Index;
            if (src->Dimension.Indirect) {
               assert(src->DimIndirect.File == TGSI_FILE_ADDRESS);
               ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
               if (src->Register.Indirect) {
                  assert(src->Indirect.File == TGSI_FILE_ADDRESS);
                  snprintf(srcs[i], 255, "%s(%s%suboarr[addr%d].ubocontents[addr%d + %d]%s)", get_string(stypeprefix), prefix, cname, src->DimIndirect.Index, src->Indirect.Index, src->Register.Index, swizzle);
               } else
                  snprintf(srcs[i], 255, "%s(%s%suboarr[addr%d].ubocontents[%d]%s)", get_string(stypeprefix), prefix, cname, src->DimIndirect.Index, src->Register.Index, swizzle);
            } else {
               if (ctx->info.dimension_indirect_files & (1 << TGSI_FILE_CONSTANT)) {
                  if (src->Register.Indirect) {
                     snprintf(srcs[i], 255, "%s(%s%suboarr[%d].ubocontents[addr%d + %d]%s)", get_string(stypeprefix), prefix, cname, dim, src->Indirect.Index, src->Register.Index, swizzle);
                  } else
                     snprintf(srcs[i], 255, "%s(%s%suboarr[%d].ubocontents[%d]%s)", get_string(stypeprefix), prefix, cname, dim, src->Register.Index, swizzle);
               } else {
                  if (src->Register.Indirect) {
                     snprintf(srcs[i], 255, "%s(%s%subo%dcontents[addr0 + %d]%s)", get_string(stypeprefix), prefix, cname, dim, src->Register.Index, swizzle);
                  } else
                     snprintf(srcs[i], 255, "%s(%s%subo%dcontents[%d]%s)", get_string(stypeprefix), prefix, cname, dim, src->Register.Index, swizzle);
               }
            }
         } else {
            enum vrend_type_qualifier csp = TYPE_CONVERSION_NONE;
            ctx->shader_req_bits |= SHADER_REQ_INTS;
            if (inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE && i == 1)
               csp = IVEC4;
            else if (stype == TGSI_TYPE_FLOAT || stype == TGSI_TYPE_UNTYPED)
               csp = UINT_BITS_TO_FLOAT;
            else if (stype == TGSI_TYPE_SIGNED)
               csp = IVEC4;

            if (src->Register.Indirect) {
               snprintf(srcs[i], 255, "%s%s(%sconst%d[addr0 + %d]%s)", prefix, get_string(csp), cname, dim, src->Register.Index, swizzle);
            } else
               snprintf(srcs[i], 255, "%s%s(%sconst%d[%d]%s)", prefix, get_string(csp), cname, dim, src->Register.Index, swizzle);
         }
      } else if (src->Register.File == TGSI_FILE_SAMPLER) {
         const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
         if (ctx->info.indirect_files & (1 << TGSI_FILE_SAMPLER)) {
            int arr_idx = lookup_sampler_array(ctx, src->Register.Index);
            if (src->Register.Indirect) {

               snprintf(srcs[i], 255, "%ssamp%d[addr%d+%d]%s", cname, arr_idx, src->Indirect.Index, src->Register.Index - ctx->sampler_arrays[arr_idx].first, swizzle);
            } else {
               snprintf(srcs[i], 255, "%ssamp%d[%d]%s", cname, arr_idx, src->Register.Index - ctx->sampler_arrays[arr_idx].first, swizzle);
            }
         } else {
            snprintf(srcs[i], 255, "%ssamp%d%s", cname, src->Register.Index, swizzle);
         }
         *sreg_index = src->Register.Index;
      } else if (src->Register.File == TGSI_FILE_IMMEDIATE) {
         if (src->Register.Index >= ARRAY_SIZE(ctx->imm)) {
            fprintf(stderr, "Immediate exceeded, max is %lu\n", ARRAY_SIZE(ctx->imm));
            return false;
         }
         struct immed *imd = &ctx->imm[src->Register.Index];
         int idx = src->Register.SwizzleX;
         char temp[48];
         enum vrend_type_qualifier vtype = VEC4;
         enum vrend_type_qualifier imm_stypeprefix = stypeprefix;

         if ((inst->Instruction.Opcode == TGSI_OPCODE_TG4 && i == 1) ||
             (inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE && i == 1))
            stype = TGSI_TYPE_SIGNED;

         if (imd->type == TGSI_IMM_UINT32 || imd->type == TGSI_IMM_INT32) {
            if (imd->type == TGSI_IMM_UINT32)
               vtype = UVEC4;
            else
               vtype = IVEC4;

            if (stype == TGSI_TYPE_UNSIGNED && imd->type == TGSI_IMM_INT32)
               imm_stypeprefix = UVEC4;
            else if (stype == TGSI_TYPE_SIGNED && imd->type == TGSI_IMM_UINT32)
               imm_stypeprefix = IVEC4;
            else if (stype == TGSI_TYPE_FLOAT || stype == TGSI_TYPE_UNTYPED) {
               if (imd->type == TGSI_IMM_INT32)
                  imm_stypeprefix = INT_BITS_TO_FLOAT;
               else
                  imm_stypeprefix = UINT_BITS_TO_FLOAT;
            } else if (stype == TGSI_TYPE_UNSIGNED || stype == TGSI_TYPE_SIGNED)
               imm_stypeprefix = TYPE_CONVERSION_NONE;
         }

         /* build up a vec4 of immediates */
         snprintf(srcs[i], 255, "%s(%s%s(", get_string(imm_stypeprefix), prefix, get_string(vtype));
         for (uint32_t j = 0; j < 4; j++) {
            if (j == 0)
               idx = src->Register.SwizzleX;
            else if (j == 1)
               idx = src->Register.SwizzleY;
            else if (j == 2)
               idx = src->Register.SwizzleZ;
            else if (j == 3)
               idx = src->Register.SwizzleW;

            if (inst->Instruction.Opcode == TGSI_OPCODE_TG4 && i == 1 && j == 0) {
               if (imd->val[idx].ui > 0) {
                  *tg4_has_component = true;
                  ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
               }
            }

            switch (imd->type) {
            case TGSI_IMM_FLOAT32:
               if (isinf(imd->val[idx].f) || isnan(imd->val[idx].f)) {
                  ctx->shader_req_bits |= SHADER_REQ_INTS;
                  snprintf(temp, 48, "uintBitsToFloat(%uU)", imd->val[idx].ui);
               } else
                  snprintf(temp, 25, "%.8g", imd->val[idx].f);
               break;
            case TGSI_IMM_UINT32:
               snprintf(temp, 25, "%uU", imd->val[idx].ui);
               break;
            case TGSI_IMM_INT32:
               snprintf(temp, 25, "%d", imd->val[idx].i);
               break;
            default:
               fprintf(stderr, "unhandled imm type: %x\n", imd->type);
               return false;
            }
            strncat(srcs[i], temp, 255);
            if (j < 3)
               strcat(srcs[i], ",");
            else {
               snprintf(temp, 4, "))%c", isabsolute ? ')' : 0);
               strncat(srcs[i], temp, 255);
            }
         }
      } else if (src->Register.File == TGSI_FILE_SYSTEM_VALUE) {
         for (uint32_t j = 0; j < ctx->num_system_values; j++)
            if (ctx->system_values[j].first == src->Register.Index) {
               if (ctx->system_values[j].name == TGSI_SEMANTIC_VERTEXID ||
                   ctx->system_values[j].name == TGSI_SEMANTIC_INSTANCEID ||
                   ctx->system_values[j].name == TGSI_SEMANTIC_INVOCATIONID ||
                   ctx->system_values[j].name == TGSI_SEMANTIC_SAMPLEID)
                  snprintf(srcs[i], 255, "%s(vec4(intBitsToFloat(%s)))", get_string(stypeprefix), ctx->system_values[j].glsl_name);
               else if (ctx->system_values[j].name == TGSI_SEMANTIC_SAMPLEPOS) {
                  snprintf(srcs[i], 255, "vec4(%s.%c, %s.%c, %s.%c, %s.%c)",
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleX),
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleY),
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleZ),
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleW));
               } else
                  snprintf(srcs[i], 255, "%s%s", prefix, ctx->system_values[j].glsl_name);
               override_no_wm[i] = ctx->system_values[j].override_no_wm;
               break;
            }
      }
   }

   return 0;
}

static boolean
iter_instruction(struct tgsi_iterate_context *iter,
                 struct tgsi_full_instruction *inst)
{
   struct dump_ctx *ctx = (struct dump_ctx *)iter;
   char srcs[4][255], dsts[3][255], buf[512];
   uint instno = ctx->instno++;
   int sreg_index = 0;
   char writemask[6] = {0};
   bool override_no_wm[3];
   bool dst_override_no_wm[2];
   char *sret;
   int ret;
   char src_swizzle0[10];
   bool tg4_has_component = false;

   enum vrend_type_qualifier dtypeprefix, dstconv, udstconv, svec4;
   dtypeprefix = dstconv = udstconv = TYPE_CONVERSION_NONE;
   svec4 = VEC4;

   if (ctx->prog_type == -1)
      ctx->prog_type = iter->processor.Processor;

   if (instno == 0) {
      sret = add_str_to_glsl_main(ctx, "void main(void)\n{\n");
      if (!sret)
         return FALSE;
      if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
         ret = emit_color_select(ctx);
         if (ret)
            return FALSE;
      }
      if (ctx->so)
         prepare_so_movs(ctx);
   }

   ret = get_destination_info(ctx, inst, &dtypeprefix, &dstconv, &udstconv, dst_override_no_wm, dsts, writemask);
   if (ret)
      return FALSE;

   ret = get_source_info(ctx, inst, &svec4, &sreg_index, srcs, src_swizzle0, &tg4_has_component, override_no_wm);
   if (ret)
      return FALSE;

   switch (inst->Instruction.Opcode) {
   case TGSI_OPCODE_SQRT:
      snprintf(buf, 255, "%s = sqrt(vec4(%s))%s;\n", dsts[0], srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_LRP:
      snprintf(buf, 255, "%s = mix(vec4(%s), vec4(%s), vec4(%s))%s;\n", dsts[0], srcs[2], srcs[1], srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DP2:
      snprintf(buf, 255, "%s = %s(dot(vec2(%s), vec2(%s)));\n", dsts[0], get_string(dstconv), srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DP3:
      snprintf(buf, 255, "%s = %s(dot(vec3(%s), vec3(%s)));\n", dsts[0], get_string(dstconv), srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DP4:
      snprintf(buf, 255, "%s = %s(dot(vec4(%s), vec4(%s)));\n", dsts[0], get_string(dstconv), srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DPH:
      snprintf(buf, 255, "%s = %s(dot(vec4(vec3(%s), 1.0), vec4(%s)));\n", dsts[0], get_string(dstconv), srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_MAX:
   case TGSI_OPCODE_IMAX:
   case TGSI_OPCODE_UMAX:
      snprintf(buf, 255, "%s = %s(%s(max(%s, %s)));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_MIN:
   case TGSI_OPCODE_IMIN:
   case TGSI_OPCODE_UMIN:
      snprintf(buf, 255, "%s = %s(%s(min(%s, %s)));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_ABS:
   case TGSI_OPCODE_IABS:
      emit_op1("abs");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_KILL_IF:
      snprintf(buf, 255, "if (any(lessThan(%s, vec4(0.0))))\ndiscard;\n", srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_IF:
   case TGSI_OPCODE_UIF:
      snprintf(buf, 255, "if (any(bvec4(%s))) {\n", srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->indent_level++;
      break;
   case TGSI_OPCODE_ELSE:
      snprintf(buf, 255, "} else {\n");
      ctx->indent_level--;
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->indent_level++;
      break;
   case TGSI_OPCODE_ENDIF:
      snprintf(buf, 255, "}\n");
      ctx->indent_level--;
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_KILL:
      snprintf(buf, 255, "discard;\n");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DST:
      snprintf(buf, 512, "%s = vec4(1.0, %s.y * %s.y, %s.z, %s.w);\n", dsts[0],
               srcs[0], srcs[1], srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_LIT:
      snprintf(buf, 512, "%s = %s(vec4(1.0, max(%s.x, 0.0), step(0.0, %s.x) * pow(max(0.0, %s.y), clamp(%s.w, -128.0, 128.0)), 1.0)%s);\n", dsts[0], get_string(dstconv), srcs[0], srcs[0], srcs[0], srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_EX2:
      emit_op1("exp2");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_LG2:
      emit_op1("log2");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_EXP:
      snprintf(buf, 512, "%s = %s(vec4(pow(2.0, floor(%s.x)), %s.x - floor(%s.x), exp2(%s.x), 1.0)%s);\n", dsts[0], get_string(dstconv), srcs[0], srcs[0], srcs[0], srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_LOG:
      snprintf(buf, 512, "%s = %s(vec4(floor(log2(%s.x)), %s.x / pow(2.0, floor(log2(%s.x))), log2(%s.x), 1.0)%s);\n", dsts[0], get_string(dstconv), srcs[0], srcs[0], srcs[0], srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_COS:
      emit_op1("cos");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_SIN:
      emit_op1("sin");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_SCS:
      snprintf(buf, 255, "%s = %s(vec4(cos(%s.x), sin(%s.x), 0, 1)%s);\n", dsts[0], get_string(dstconv),
               srcs[0], srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DDX:
      emit_op1("dFdx");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DDY:
      emit_op1("dFdy");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DDX_FINE:
      ctx->shader_req_bits |= SHADER_REQ_DERIVATIVE_CONTROL;
      emit_op1("dFdxFine");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DDY_FINE:
      ctx->shader_req_bits |= SHADER_REQ_DERIVATIVE_CONTROL;
      emit_op1("dFdyFine");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_RCP:
      snprintf(buf, 255, "%s = %s(1.0/(%s));\n", dsts[0], get_string(dstconv), srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_FLR:
      emit_op1("floor");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_ROUND:
      emit_op1("round");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_ISSG:
      emit_op1("sign");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_CEIL:
      emit_op1("ceil");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_FRC:
      emit_op1("fract");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_TRUNC:
      emit_op1("trunc");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_SSG:
      emit_op1("sign");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_RSQ:
      snprintf(buf, 255, "%s = %s(inversesqrt(%s.x));\n", dsts[0], get_string(dstconv), srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_MOV:
      snprintf(buf, 255, "%s = %s(%s(%s%s));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], override_no_wm[0] ? "" : writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_ADD:
      emit_arit_op2("+");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_UADD:
      snprintf(buf, 255, "%s = %s(%s(ivec4((uvec4(%s) + uvec4(%s))))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_SUB:
      emit_arit_op2("-");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_MUL:
      emit_arit_op2("*");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_DIV:
      emit_arit_op2("/");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_UMUL:
      snprintf(buf, 255, "%s = %s(%s((uvec4(%s) * uvec4(%s)))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_UMOD:
      snprintf(buf, 255, "%s = %s(%s((uvec4(%s) %% uvec4(%s)))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_IDIV:
      snprintf(buf, 255, "%s = %s(%s((ivec4(%s) / ivec4(%s)))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_UDIV:
      snprintf(buf, 255, "%s = %s(%s((uvec4(%s) / uvec4(%s)))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_ISHR:
   case TGSI_OPCODE_USHR:
      emit_arit_op2(">>");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_SHL:
      emit_arit_op2("<<");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_MAD:
      snprintf(buf, 255, "%s = %s((%s * %s + %s)%s);\n", dsts[0], get_string(dstconv), srcs[0], srcs[1], srcs[2], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_UMAD:
      snprintf(buf, 255, "%s = %s(%s((%s * %s + %s)%s));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], srcs[2], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_OR:
      emit_arit_op2("|");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_AND:
      emit_arit_op2("&");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_XOR:
      emit_arit_op2("^");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_MOD:
      emit_arit_op2("%");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_TEX:
   case TGSI_OPCODE_TEX2:
   case TGSI_OPCODE_TXB:
   case TGSI_OPCODE_TXL:
   case TGSI_OPCODE_TXB2:
   case TGSI_OPCODE_TXL2:
   case TGSI_OPCODE_TXD:
   case TGSI_OPCODE_TXF:
   case TGSI_OPCODE_TG4:
   case TGSI_OPCODE_TXP:
   case TGSI_OPCODE_TXQ:
   case TGSI_OPCODE_LODQ:
      ret = translate_tex(ctx, inst, sreg_index, srcs, dsts, writemask, get_string(dstconv), dst_override_no_wm[0], tg4_has_component);
      if (ret)
         return FALSE;
      break;
   case TGSI_OPCODE_I2F:
      snprintf(buf, 255, "%s = %s(ivec4(%s)%s);\n", dsts[0], get_string(dstconv), srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_U2F:
      snprintf(buf, 255, "%s = %s(uvec4(%s)%s);\n", dsts[0], get_string(dstconv), srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_F2I:
      snprintf(buf, 255, "%s = %s(%s(ivec4(%s))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_F2U:
      snprintf(buf, 255, "%s = %s(%s(uvec4(%s))%s);\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_NOT:
      snprintf(buf, 255, "%s = %s(uintBitsToFloat(~(uvec4(%s))));\n", dsts[0], get_string(dstconv), srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_INEG:
      snprintf(buf, 255, "%s = %s(intBitsToFloat(-(ivec4(%s))));\n", dsts[0], get_string(dstconv), srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_SEQ:
      emit_compare("equal");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_USEQ:
   case TGSI_OPCODE_FSEQ:
      emit_ucompare("equal");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_SLT:
      emit_compare("lessThan");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_ISLT:
   case TGSI_OPCODE_USLT:
   case TGSI_OPCODE_FSLT:
      emit_ucompare("lessThan");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_SNE:
      emit_compare("notEqual");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_USNE:
   case TGSI_OPCODE_FSNE:
      emit_ucompare("notEqual");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_SGE:
      emit_compare("greaterThanEqual");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_ISGE:
   case TGSI_OPCODE_USGE:
   case TGSI_OPCODE_FSGE:
      emit_ucompare("greaterThanEqual");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_POW:
      snprintf(buf, 255, "%s = %s(pow(%s, %s));\n", dsts[0], get_string(dstconv), srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_CMP:
      snprintf(buf, 255, "%s = mix(%s, %s, greaterThanEqual(%s, vec4(0.0)))%s;\n", dsts[0], srcs[1], srcs[2], srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_UCMP:
      snprintf(buf, 512, "%s = mix(%s, %s, notEqual(floatBitsToUint(%s), uvec4(0.0)))%s;\n", dsts[0], srcs[2], srcs[1], srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_END:
      if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
         if (handle_vertex_proc_exit(ctx) == FALSE)
            return FALSE;
      } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
         if (handle_fragment_proc_exit(ctx) == FALSE)
            return FALSE;
      }
      sret = add_str_to_glsl_main(ctx, "}\n");
      if (!sret)
         return FALSE;
      break;
   case TGSI_OPCODE_RET:
      if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
         if (handle_vertex_proc_exit(ctx) == FALSE)
            return FALSE;
      } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
         if (handle_fragment_proc_exit(ctx) == FALSE)
            return FALSE;
      }
      EMIT_BUF_WITH_RET(ctx, "return;\n");
      break;
   case TGSI_OPCODE_ARL:
      snprintf(buf, 255, "%s = int(floor(%s)%s);\n", dsts[0], srcs[0], writemask);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_UARL:
      snprintf(buf, 255, "%s = int(%s);\n", dsts[0], srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_XPD:
      snprintf(buf, 255, "%s = %s(cross(vec3(%s), vec3(%s)));\n", dsts[0], get_string(dstconv), srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_BGNLOOP:
      snprintf(buf, 255, "do {\n");
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->indent_level++;
      break;
   case TGSI_OPCODE_ENDLOOP:
      ctx->indent_level--;
      snprintf(buf, 255, "} while(true);\n");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_BRK:
      snprintf(buf, 255, "break;\n");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   case TGSI_OPCODE_EMIT: {
      struct immed *imd = &ctx->imm[(inst->Src[0].Register.Index)];
      if (ctx->so && ctx->key->gs_present) {
         emit_so_movs(ctx);
      }
      ret = emit_clip_dist_movs(ctx);
      if (ret)
         return FALSE;
      ret = emit_prescale(ctx);
      if (ret)
         return FALSE;
      if (imd->val[inst->Src[0].Register.SwizzleX].ui > 0) {
         ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
         snprintf(buf, 255, "EmitStreamVertex(%d);\n", imd->val[inst->Src[0].Register.SwizzleX].ui);
      } else
         snprintf(buf, 255, "EmitVertex();\n");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   }
   case TGSI_OPCODE_ENDPRIM: {
      struct immed *imd = &ctx->imm[(inst->Src[0].Register.Index)];
      if (imd->val[inst->Src[0].Register.SwizzleX].ui > 0) {
         ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
         snprintf(buf, 255, "EndStreamPrimitive(%d);\n", imd->val[inst->Src[0].Register.SwizzleX].ui);
      } else
         snprintf(buf, 255, "EndPrimitive();\n");
      EMIT_BUF_WITH_RET(ctx, buf);
      break;
   }
   case TGSI_OPCODE_INTERP_CENTROID:
      snprintf(buf, 255, "%s = %s(%s(vec4(interpolateAtCentroid(%s))%s));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], src_swizzle0);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_INTERP_SAMPLE:
      snprintf(buf, 255, "%s = %s(%s(vec4(interpolateAtSample(%s, %s.x))%s));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], src_swizzle0);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_INTERP_OFFSET:
      snprintf(buf, 255, "%s = %s(%s(vec4(interpolateAtOffset(%s, %s.xy))%s));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], src_swizzle0);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_UMUL_HI:
      snprintf(buf, 255, "umulExtended(%s, %s, umul_temp, mul_temp);\n", srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      snprintf(buf, 255, "%s = %s(%s(umul_temp));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix));
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      ctx->write_mul_temp = true;
      break;
   case TGSI_OPCODE_IMUL_HI:
      snprintf(buf, 255, "imulExtended(%s, %s, imul_temp, mul_temp);\n", srcs[0], srcs[1]);
      EMIT_BUF_WITH_RET(ctx, buf);
      snprintf(buf, 255, "%s = %s(%s(imul_temp));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix));
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      ctx->write_mul_temp = true;
      break;

   case TGSI_OPCODE_IBFE:
      snprintf(buf, 255, "%s = %s(%s(bitfieldExtract(%s, int(%s.x), int(%s.x))));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], srcs[2]);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_UBFE:
      snprintf(buf, 255, "%s = %s(%s(bitfieldExtract(%s, int(%s.x), int(%s.x))));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0], srcs[1], srcs[2]);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_BFI:
      snprintf(buf, 255, "%s = %s(uintBitsToFloat(bitfieldInsert(%s, %s, int(%s), int(%s))));\n", dsts[0], get_string(dstconv), srcs[0], srcs[1], srcs[2], srcs[3]);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_BREV:
      snprintf(buf, 255, "%s = %s(%s(bitfieldReverse(%s)));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_POPC:
      snprintf(buf, 255, "%s = %s(%s(bitCount(%s)));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_LSB:
      snprintf(buf, 255, "%s = %s(%s(findLSB(%s)));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_IMSB:
   case TGSI_OPCODE_UMSB:
      snprintf(buf, 255, "%s = %s(%s(findMSB(%s)));\n", dsts[0], get_string(dstconv), get_string(dtypeprefix), srcs[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_BARRIER:
      snprintf(buf, 255, "barrier();\n");
      break;
   default:
      fprintf(stderr,"failed to convert opcode %d\n", inst->Instruction.Opcode);
      break;
   }

   if (inst->Instruction.Saturate) {
      snprintf(buf, 255, "%s = clamp(%s, 0.0, 1.0);\n", dsts[0], dsts[0]);
      EMIT_BUF_WITH_RET(ctx, buf);
   }
   return TRUE;
}

static boolean
prolog(struct tgsi_iterate_context *iter)
{
   struct dump_ctx *ctx = (struct dump_ctx *)iter;

   if (ctx->prog_type == -1)
      ctx->prog_type = iter->processor.Processor;

   if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX &&
       ctx->key->gs_present)
      require_glsl_ver(ctx, 150);

   return TRUE;
}

#define STRCAT_WITH_RET(mainstr, buf) do {              \
      (mainstr) = strcat_realloc((mainstr), (buf));        \
      if ((mainstr) == NULL) return NULL;               \
   } while(0)

static char *emit_header(struct dump_ctx *ctx, char *glsl_hdr)
{
   if (ctx->cfg->use_gles) {
      STRCAT_WITH_RET(glsl_hdr, "#version 300 es\n");
      STRCAT_WITH_RET(glsl_hdr, "precision highp float;\n");
      STRCAT_WITH_RET(glsl_hdr, "precision highp int;\n");
   } else {
      char buf[128];
      if (ctx->prog_type == TGSI_PROCESSOR_GEOMETRY || ctx->glsl_ver_required == 150)
         STRCAT_WITH_RET(glsl_hdr, "#version 150\n");
      else if (ctx->glsl_ver_required == 140)
         STRCAT_WITH_RET(glsl_hdr, "#version 140\n");
      else
         STRCAT_WITH_RET(glsl_hdr, "#version 130\n");

      if (ctx->prog_type == TGSI_PROCESSOR_VERTEX && ctx->cfg->use_explicit_locations)
         STRCAT_WITH_RET(glsl_hdr, "#extension GL_ARB_explicit_attrib_location : require\n");
      if (ctx->prog_type == TGSI_PROCESSOR_FRAGMENT && fs_emit_layout(ctx))
         STRCAT_WITH_RET(glsl_hdr, "#extension GL_ARB_fragment_coord_conventions : require\n");

      if (ctx->num_ubo)
         STRCAT_WITH_RET(glsl_hdr, "#extension GL_ARB_uniform_buffer_object : require\n");

      if (ctx->num_cull_dist_prop || ctx->key->prev_stage_num_cull_out)
         STRCAT_WITH_RET(glsl_hdr, "#extension GL_ARB_cull_distance : require\n");

      for (uint32_t i = 0; i < ARRAY_SIZE(shader_req_table); i++) {
         if (shader_req_table[i].key == SHADER_REQ_SAMPLER_RECT && ctx->glsl_ver_required >= 140)
            continue;

         if (ctx->shader_req_bits & shader_req_table[i].key) {
            snprintf(buf, 128, "#extension %s : require\n", shader_req_table[i].string);
            STRCAT_WITH_RET(glsl_hdr, buf);
         }
      }
   }

   return glsl_hdr;
}

char vrend_shader_samplerreturnconv(enum tgsi_return_type type)
{
   switch (type) {
   case TGSI_RETURN_TYPE_SINT:
      return 'i';
   case TGSI_RETURN_TYPE_UINT:
      return 'u';
   default:
      return ' ';
   }
}

const char *vrend_shader_samplertypeconv(int sampler_type, int *is_shad)
{
   switch (sampler_type) {
   case TGSI_TEXTURE_BUFFER: return "Buffer";
   case TGSI_TEXTURE_1D: return "1D";
   case TGSI_TEXTURE_2D: return "2D";
   case TGSI_TEXTURE_3D: return "3D";
   case TGSI_TEXTURE_CUBE: return "Cube";
   case TGSI_TEXTURE_RECT: return "2DRect";
   case TGSI_TEXTURE_SHADOW1D: *is_shad = 1; return "1DShadow";
   case TGSI_TEXTURE_SHADOW2D: *is_shad = 1; return "2DShadow";
   case TGSI_TEXTURE_SHADOWRECT: *is_shad = 1; return "2DRectShadow";
   case TGSI_TEXTURE_1D_ARRAY: return "1DArray";
   case TGSI_TEXTURE_2D_ARRAY: return "2DArray";
   case TGSI_TEXTURE_SHADOW1D_ARRAY: *is_shad = 1; return "1DArrayShadow";
   case TGSI_TEXTURE_SHADOW2D_ARRAY: *is_shad = 1; return "2DArrayShadow";
   case TGSI_TEXTURE_SHADOWCUBE: *is_shad = 1; return "CubeShadow";
   case TGSI_TEXTURE_CUBE_ARRAY: return "CubeArray";
   case TGSI_TEXTURE_SHADOWCUBE_ARRAY: *is_shad = 1; return "CubeArrayShadow";
   case TGSI_TEXTURE_2D_MSAA: return "2DMS";
   case TGSI_TEXTURE_2D_ARRAY_MSAA: return "2DMSArray";
   default: return NULL;
   }
}

static const char *get_interp_string(struct vrend_shader_cfg *cfg, int interpolate, bool flatshade)
{
   switch (interpolate) {
   case TGSI_INTERPOLATE_LINEAR:
   if (!cfg->use_gles)
      return "noperspective ";
   else
      return "";
   case TGSI_INTERPOLATE_PERSPECTIVE:
      return "smooth ";
   case TGSI_INTERPOLATE_CONSTANT:
      return "flat ";
   case TGSI_INTERPOLATE_COLOR:
      if (flatshade)
         return "flat ";
   default:
      return NULL;
   }
}

static const char *get_aux_string(struct vrend_shader_cfg *cfg, bool centroid)
{
   return centroid ? "centroid " : "";
}

static const char get_return_type_prefix(enum tgsi_return_type type)
{
   if (type == TGSI_RETURN_TYPE_SINT)
      return 'i';
   if (type == TGSI_RETURN_TYPE_UINT)
      return 'u';
   return ' ';
}

static char *emit_ios(struct dump_ctx *ctx, char *glsl_hdr)
{
   int i;
   char buf[255];
   char postfix[8];
   const char *prefix = "", *auxprefix = "";
   bool fcolor_emitted[2], bcolor_emitted[2];
   int nsamp;
   const char *sname = tgsi_proc_to_prefix(ctx->prog_type);
   ctx->num_interps = 0;

   if (ctx->so && ctx->so->num_outputs >= PIPE_MAX_SO_OUTPUTS) {
      fprintf(stderr, "Num outputs exceeded, max is %u\n", PIPE_MAX_SO_OUTPUTS);
      return NULL;
   }

   if (ctx->key->color_two_side) {
      fcolor_emitted[0] = fcolor_emitted[1] = false;
      bcolor_emitted[0] = bcolor_emitted[1] = false;
   }
   if (ctx->prog_type == TGSI_PROCESSOR_FRAGMENT) {
      if (fs_emit_layout(ctx)) {
         bool upper_left = !(ctx->fs_coord_origin ^ ctx->key->invert_fs_origin);
         char comma = (upper_left && ctx->fs_pixel_center) ? ',' : ' ';

         snprintf(buf, 255, "layout(%s%c%s) in vec4 gl_FragCoord;\n",
                  upper_left ? "origin_upper_left" : "",
                  comma,
                  ctx->fs_pixel_center ? "pixel_center_integer" : "");
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
   }
   if (ctx->prog_type == TGSI_PROCESSOR_GEOMETRY) {
      char invocbuf[25];

      if (ctx->gs_num_invocations)
         snprintf(invocbuf, 25, ", invocations = %d", ctx->gs_num_invocations);

      snprintf(buf, 255, "layout(%s%s) in;\n", prim_to_name(ctx->gs_in_prim),
               ctx->gs_num_invocations > 1 ? invocbuf : "");
      STRCAT_WITH_RET(glsl_hdr, buf);
      snprintf(buf, 255, "layout(%s, max_vertices = %d) out;\n", prim_to_name(ctx->gs_out_prim), ctx->gs_max_out_verts);
      STRCAT_WITH_RET(glsl_hdr, buf);
   }
   for (i = 0; i < ctx->num_inputs; i++) {
      if (!ctx->inputs[i].glsl_predefined_no_emit) {
         if (ctx->prog_type == TGSI_PROCESSOR_VERTEX && ctx->cfg->use_explicit_locations) {
            snprintf(buf, 255, "layout(location=%d) ", ctx->inputs[i].first);
            STRCAT_WITH_RET(glsl_hdr, buf);
         }
         if (ctx->prog_type == TGSI_PROCESSOR_FRAGMENT &&
             (ctx->inputs[i].name == TGSI_SEMANTIC_GENERIC ||
              ctx->inputs[i].name == TGSI_SEMANTIC_COLOR)) {
            prefix = get_interp_string(ctx->cfg, ctx->inputs[i].interpolate, ctx->key->flatshade);
            if (!prefix)
               prefix = "";
            auxprefix = get_aux_string(ctx->cfg, ctx->inputs[i].centroid);
            ctx->num_interps++;
         }

         if (ctx->prog_type == TGSI_PROCESSOR_GEOMETRY) {
            snprintf(postfix, 8, "[%d]", gs_input_prim_to_size(ctx->gs_in_prim));
         } else
            postfix[0] = 0;
         snprintf(buf, 255, "%s%sin vec4 %s%s;\n", prefix, auxprefix, ctx->inputs[i].glsl_name, postfix);
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
   }
   if (ctx->write_all_cbufs) {
      for (i = 0; i < 8; i++) {
         if (ctx->cfg->use_gles)
            snprintf(buf, 255, "layout (location=%d) out vec4 fsout_c%d;\n", i, i);
         else
            snprintf(buf, 255, "out vec4 fsout_c%d;\n", i);
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
   } else {
      for (i = 0; i < ctx->num_outputs; i++) {
         if (ctx->prog_type == TGSI_PROCESSOR_VERTEX && ctx->key->color_two_side && ctx->outputs[i].sid < 2) {
            if (ctx->outputs[i].name == TGSI_SEMANTIC_COLOR)
               fcolor_emitted[ctx->outputs[i].sid] = true;
            if (ctx->outputs[i].name == TGSI_SEMANTIC_BCOLOR)
               bcolor_emitted[ctx->outputs[i].sid] = true;
         }
         if (!ctx->outputs[i].glsl_predefined_no_emit) {
            if ((ctx->prog_type == TGSI_PROCESSOR_VERTEX || ctx->prog_type == TGSI_PROCESSOR_GEOMETRY) && (ctx->outputs[i].name == TGSI_SEMANTIC_GENERIC || ctx->outputs[i].name == TGSI_SEMANTIC_COLOR || ctx->outputs[i].name == TGSI_SEMANTIC_BCOLOR)) {
               ctx->num_interps++;
               prefix = INTERP_PREFIX;
            } else
               prefix = "";
            /* ugly leave spaces to patch interp in later */
            if (ctx->prog_type == TGSI_PROCESSOR_GEOMETRY && ctx->outputs[i].stream)
               snprintf(buf, 255, "layout (stream = %d) %s%sout vec4 %s;\n", ctx->outputs[i].stream, prefix, ctx->outputs[i].invariant ? "invariant " : "", ctx->outputs[i].glsl_name);
            else
               snprintf(buf, 255, "%s%sout vec4 %s;\n", prefix, ctx->outputs[i].invariant ? "invariant " : "", ctx->outputs[i].glsl_name);
            STRCAT_WITH_RET(glsl_hdr, buf);
         } else if (ctx->outputs[i].invariant) {
            snprintf(buf, 255, "invariant %s;\n", ctx->outputs[i].glsl_name);
            STRCAT_WITH_RET(glsl_hdr, buf);
         }
      }
   }

   if (ctx->prog_type == TGSI_PROCESSOR_VERTEX && ctx->key->color_two_side) {
      for (i = 0; i < 2; i++) {
         if (fcolor_emitted[i] && !bcolor_emitted[i]) {
            snprintf(buf, 255, "%sout vec4 ex_bc%d;\n", INTERP_PREFIX, i);
            STRCAT_WITH_RET(glsl_hdr, buf);
         }
         if (bcolor_emitted[i] && !fcolor_emitted[i]) {
            snprintf(buf, 255, "%sout vec4 ex_c%d;\n", INTERP_PREFIX, i);
            STRCAT_WITH_RET(glsl_hdr, buf);
         }
      }
   }

   if (ctx->prog_type == TGSI_PROCESSOR_VERTEX) {
      snprintf(buf, 255, "uniform float winsys_adjust_y;\n");
      STRCAT_WITH_RET(glsl_hdr, buf);

      if (ctx->has_clipvertex) {
         snprintf(buf, 255, "%svec4 clipv_tmp;\n", ctx->has_clipvertex_so ? "out " : "");
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
      if (ctx->num_clip_dist || ctx->key->clip_plane_enable) {
         bool has_prop = (ctx->num_clip_dist_prop + ctx->num_cull_dist_prop) > 0;
         int num_clip_dists = ctx->num_clip_dist ? ctx->num_clip_dist : 8;
         int num_cull_dists = 0;
         char cull_buf[64] = { 0 };
         char clip_buf[64] = { 0 };
         if (has_prop) {
            num_clip_dists = ctx->num_clip_dist_prop;
            num_cull_dists = ctx->num_cull_dist_prop;
            if (num_clip_dists)
               snprintf(clip_buf, 64, "out float gl_ClipDistance[%d];\n", num_clip_dists);
            if (num_cull_dists)
               snprintf(cull_buf, 64, "out float gl_CullDistance[%d];\n", num_cull_dists);
         } else
            snprintf(clip_buf, 64, "out float gl_ClipDistance[%d];\n", num_clip_dists);
         if (ctx->key->clip_plane_enable) {
            snprintf(buf, 255, "uniform vec4 clipp[8];\n");
            STRCAT_WITH_RET(glsl_hdr, buf);
         }
         if (ctx->key->gs_present) {
            ctx->vs_has_pervertex = true;
            snprintf(buf, 255, "out gl_PerVertex {\n vec4 gl_Position;\n float gl_PointSize;\n%s%s};\n", clip_buf, cull_buf);
            STRCAT_WITH_RET(glsl_hdr, buf);
         } else {
            snprintf(buf, 255, "%s%s", clip_buf, cull_buf);
            STRCAT_WITH_RET(glsl_hdr, buf);
         }
         snprintf(buf, 255, "vec4 clip_dist_temp[2];\n");
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
   }

   if (ctx->prog_type == TGSI_PROCESSOR_GEOMETRY) {
      snprintf(buf, 255, "uniform float winsys_adjust_y;\n");
      STRCAT_WITH_RET(glsl_hdr, buf);
      if (ctx->num_in_clip_dist || ctx->key->clip_plane_enable || ctx->key->prev_stage_pervertex_out) {
         int clip_dist, cull_dist;
         char clip_var[64] = {}, cull_var[64] = {};

         clip_dist = ctx->key->prev_stage_num_clip_out ? ctx->key->prev_stage_num_clip_out : ctx->num_in_clip_dist;
         cull_dist = ctx->key->prev_stage_num_cull_out;

         if (clip_dist)
            snprintf(clip_var, 64, "float gl_ClipDistance[%d];\n", clip_dist);
         if (cull_dist)
            snprintf(cull_var, 64, "float gl_CullDistance[%d];\n", cull_dist);

         snprintf(buf, 255, "in gl_PerVertex {\n vec4 gl_Position;\n float gl_PointSize; \n %s%s\n} gl_in[];\n", clip_var, cull_var);
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
      if (ctx->num_clip_dist) {
         bool has_prop = (ctx->num_clip_dist_prop + ctx->num_cull_dist_prop) > 0;
         int num_clip_dists = ctx->num_clip_dist ? ctx->num_clip_dist : 8;
         int num_cull_dists = 0;
         char cull_buf[64] = { 0 };
         char clip_buf[64] = { 0 };
         if (has_prop) {
            num_clip_dists = ctx->num_clip_dist_prop;
            num_cull_dists = ctx->num_cull_dist_prop;
            if (num_clip_dists)
               snprintf(clip_buf, 64, "out float gl_ClipDistance[%d];\n", num_clip_dists);
            if (num_cull_dists)
               snprintf(cull_buf, 64, "out float gl_CullDistance[%d];\n", num_cull_dists);
         } else
            snprintf(clip_buf, 64, "out float gl_ClipDistance[%d];\n", num_clip_dists);
         snprintf(buf, 255, "%s%s\n", clip_buf, cull_buf);
         STRCAT_WITH_RET(glsl_hdr, buf);
         snprintf(buf, 255, "vec4 clip_dist_temp[2];\n");
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
   }

   if (ctx->prog_type == TGSI_PROCESSOR_FRAGMENT && ctx->num_in_clip_dist) {
      if (ctx->key->prev_stage_num_clip_out) {
         snprintf(buf, 255, "in float gl_ClipDistance[%d];\n", ctx->key->prev_stage_num_clip_out);
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
      if (ctx->key->prev_stage_num_cull_out) {
         snprintf(buf, 255, "in float gl_CullDistance[%d];\n", ctx->key->prev_stage_num_cull_out);
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
   }

   if (ctx->so) {
      char outtype[6] = {0};
      for (i = 0; i < ctx->so->num_outputs; i++) {
         if (!ctx->write_so_outputs[i])
            continue;
         if (ctx->so->output[i].num_components == 1)
            snprintf(outtype, 6, "float");
         else
            snprintf(outtype, 6, "vec%d", ctx->so->output[i].num_components);
         if (ctx->so->output[i].stream && ctx->prog_type == TGSI_PROCESSOR_GEOMETRY)
            snprintf(buf, 255, "layout (stream=%d) out %s tfout%d;\n", ctx->so->output[i].stream, outtype, i);
         else
            snprintf(buf, 255, "out %s tfout%d;\n", outtype, i);
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
   }
   for (i = 0; i < ctx->num_temp_ranges; i++) {
      snprintf(buf, 255, "vec4 temp%d[%d];\n", ctx->temp_ranges[i].first, ctx->temp_ranges[i].last - ctx->temp_ranges[i].first + 1);
      STRCAT_WITH_RET(glsl_hdr, buf);
   }

   if (ctx->write_mul_temp) {
      snprintf(buf, 255, "uvec4 mul_temp;\n");
      STRCAT_WITH_RET(glsl_hdr, buf);
      snprintf(buf, 255, "uvec4 umul_temp;\n");
      STRCAT_WITH_RET(glsl_hdr, buf);
      snprintf(buf, 255, "ivec4 imul_temp;\n");
      STRCAT_WITH_RET(glsl_hdr, buf);
   }

   for (i = 0; i < ctx->num_address; i++) {
      snprintf(buf, 255, "int addr%d;\n", i);
      STRCAT_WITH_RET(glsl_hdr, buf);
   }
   if (ctx->num_consts) {
      const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
      snprintf(buf, 255, "uniform uvec4 %sconst0[%d];\n", cname, ctx->num_consts);
      STRCAT_WITH_RET(glsl_hdr, buf);
   }

   if (ctx->key->color_two_side) {
      if (ctx->color_in_mask & 1) {
         snprintf(buf, 255, "vec4 realcolor0;\n");
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
      if (ctx->color_in_mask & 2) {
         snprintf(buf, 255, "vec4 realcolor1;\n");
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
   }
   if (ctx->num_ubo) {
      const char *cname = tgsi_proc_to_prefix(ctx->prog_type);

      if (ctx->info.dimension_indirect_files & (1 << TGSI_FILE_CONSTANT)) {
         require_glsl_ver(ctx, 150);
         snprintf(buf, 255, "uniform %subo { vec4 ubocontents[%d]; } %suboarr[%d];\n", cname, ctx->ubo_sizes[0], cname, ctx->num_ubo);
         STRCAT_WITH_RET(glsl_hdr, buf);
      } else {
         for (i = 0; i < ctx->num_ubo; i++) {
            snprintf(buf, 255, "uniform %subo%d { vec4 %subo%dcontents[%d]; };\n", cname, ctx->ubo_idx[i], cname, ctx->ubo_idx[i], ctx->ubo_sizes[i]);
            STRCAT_WITH_RET(glsl_hdr, buf);
         }
      }
   }

   if (ctx->info.indirect_files & (1 << TGSI_FILE_SAMPLER)) {
      for (i = 0; i < ctx->num_sampler_arrays; i++) {
         int is_shad = 0;
         const char *stc;
         stc = vrend_shader_samplertypeconv(ctx->sampler_arrays[i].sview_type, &is_shad);
         if (!stc)
            continue;
         snprintf(buf, 255, "uniform %csampler%s %ssamp%d[%d];\n",
                  get_return_type_prefix(ctx->sampler_arrays[i].sview_rtype),
                  stc, sname, ctx->sampler_arrays[i].idx,
                  ctx->sampler_arrays[i].last - ctx->sampler_arrays[i].first);
         STRCAT_WITH_RET(glsl_hdr, buf);
      }
   } else {
      nsamp = util_last_bit(ctx->samplers_used);
      for (i = 0; i < nsamp; i++) {
         int is_shad = 0;
         const char *stc;
         char ptc;

         if ((ctx->samplers_used & (1 << i)) == 0)
            continue;

         const char *sname;
         const char *precision;

         ptc = vrend_shader_samplerreturnconv(ctx->samplers[i].tgsi_sampler_return);
         stc = vrend_shader_samplertypeconv(ctx->samplers[i].tgsi_sampler_type, &is_shad);

         sname = tgsi_proc_to_prefix(ctx->prog_type);

         if (ctx->cfg->use_gles) {
            precision = "highp ";
         } else {
            precision = " ";
         }

         /* OpenGL ES do not support 1D texture
          * so we use a 2D texture with a parameter set to 0.5
          */
         if (ctx->cfg->use_gles && !strcmp(stc, "1D"))
            snprintf(buf, 255, "uniform %csampler2D %ssamp%d;\n", ptc, sname, i);
         else
            snprintf(buf, 255, "uniform %s%csampler%s %ssamp%d;\n", precision,  ptc, stc, sname, i);

         STRCAT_WITH_RET(glsl_hdr, buf);
         if (is_shad) {
            snprintf(buf, 255, "uniform %svec4 %sshadmask%d;\n", precision,  sname, i);
            STRCAT_WITH_RET(glsl_hdr, buf);
            snprintf(buf, 255, "uniform %svec4 %sshadadd%d;\n", precision,  sname, i);
            STRCAT_WITH_RET(glsl_hdr, buf);
            ctx->shadow_samp_mask |= (1 << i);
         }
      }
   }
   if (ctx->prog_type == TGSI_PROCESSOR_FRAGMENT &&
       ctx->key->pstipple_tex == true) {
      snprintf(buf, 255, "uniform sampler2D pstipple_sampler;\nfloat stip_temp;\n");
      STRCAT_WITH_RET(glsl_hdr, buf);
   }
   return glsl_hdr;
}

static boolean fill_fragment_interpolants(struct dump_ctx *ctx, struct vrend_shader_info *sinfo)
{
   int i, index = 0;

   for (i = 0; i < ctx->num_inputs; i++) {
      if (ctx->inputs[i].glsl_predefined_no_emit)
         continue;

      if (ctx->inputs[i].name != TGSI_SEMANTIC_GENERIC &&
          ctx->inputs[i].name != TGSI_SEMANTIC_COLOR)
         continue;

      if (index >= ctx->num_interps) {
         fprintf(stderr, "mismatch in number of interps %d %d\n", index, ctx->num_interps);
         return TRUE;
      }
      sinfo->interpinfo[index].semantic_name = ctx->inputs[i].name;
      sinfo->interpinfo[index].semantic_index = ctx->inputs[i].sid;
      sinfo->interpinfo[index].interpolate = ctx->inputs[i].interpolate;
      sinfo->interpinfo[index].centroid = ctx->inputs[i].centroid;
      index++;
   }
   return TRUE;
}

static boolean fill_interpolants(struct dump_ctx *ctx, struct vrend_shader_info *sinfo)
{
   boolean ret;

   if (!ctx->num_interps)
      return TRUE;
   if (ctx->prog_type == TGSI_PROCESSOR_VERTEX || ctx->prog_type == TGSI_PROCESSOR_GEOMETRY)
      return TRUE;

   free(sinfo->interpinfo);
   sinfo->interpinfo = calloc(ctx->num_interps, sizeof(struct vrend_interp_info));
   if (!sinfo->interpinfo)
      return FALSE;

   ret = fill_fragment_interpolants(ctx, sinfo);
   if (ret == FALSE)
      goto out_fail;

   return TRUE;
 out_fail:
   free(sinfo->interpinfo);
   return FALSE;
}

char *vrend_convert_shader(struct vrend_shader_cfg *cfg,
                           const struct tgsi_token *tokens,
                           struct vrend_shader_key *key,
                           struct vrend_shader_info *sinfo)
{
   struct dump_ctx ctx;
   char *glsl_final = NULL;
   boolean bret;
   char *glsl_hdr = NULL;

   memset(&ctx, 0, sizeof(struct dump_ctx));
   ctx.iter.prolog = prolog;
   ctx.iter.iterate_instruction = iter_instruction;
   ctx.iter.iterate_declaration = iter_declaration;
   ctx.iter.iterate_immediate = iter_immediate;
   ctx.iter.iterate_property = iter_property;
   ctx.iter.epilog = NULL;
   ctx.key = key;
   ctx.cfg = cfg;
   ctx.prog_type = -1;
   ctx.num_sampler_arrays = 0;
   ctx.sampler_arrays = NULL;
   ctx.last_sampler_array_idx = -1;
   tgsi_scan_shader(tokens, &ctx.info);
   /* if we are in core profile mode we should use GLSL 1.40 */
   if (cfg->use_core_profile && cfg->glsl_version >= 140)
      require_glsl_ver(&ctx, 140);

   if (sinfo->so_info.num_outputs) {
      ctx.so = &sinfo->so_info;
      ctx.so_names = calloc(sinfo->so_info.num_outputs, sizeof(char *));
      if (!ctx.so_names)
         goto fail;
   } else
      ctx.so_names = NULL;

   if (ctx.info.dimension_indirect_files & (1 << TGSI_FILE_CONSTANT))
      require_glsl_ver(&ctx, 150);

   if (ctx.info.indirect_files & (1 << TGSI_FILE_SAMPLER))
      ctx.shader_req_bits |= SHADER_REQ_GPU_SHADER5;

   ctx.glsl_main = malloc(4096);
   if (!ctx.glsl_main)
      goto fail;

   ctx.glsl_main[0] = '\0';
   bret = tgsi_iterate_shader(tokens, &ctx.iter);
   if (bret == FALSE)
      goto fail;

   glsl_hdr = malloc(1024);
   if (!glsl_hdr)
      goto fail;
   glsl_hdr[0] = '\0';
   glsl_hdr = emit_header(&ctx, glsl_hdr);
   if (!glsl_hdr)
      goto fail;

   glsl_hdr = emit_ios(&ctx, glsl_hdr);
   if (!glsl_hdr)
      goto fail;

   glsl_final = malloc(strlen(glsl_hdr) + strlen(ctx.glsl_main) + 1);
   if (!glsl_final)
      goto fail;

   glsl_final[0] = '\0';

   bret = fill_interpolants(&ctx, sinfo);
   if (bret == FALSE)
      goto fail;

   strcat(glsl_final, glsl_hdr);
   strcat(glsl_final, ctx.glsl_main);
   if (vrend_dump_shaders)
      fprintf(stderr,"GLSL: %s\n", glsl_final);
   free(ctx.temp_ranges);
   free(ctx.glsl_main);
   free(glsl_hdr);
   sinfo->num_ucp = ctx.key->clip_plane_enable ? 8 : 0;
   sinfo->has_pervertex_out = ctx.vs_has_pervertex;
   bool has_prop = (ctx.num_clip_dist_prop + ctx.num_cull_dist_prop) > 0;
   sinfo->num_clip_out = has_prop ? ctx.num_clip_dist_prop : (ctx.num_clip_dist ? ctx.num_clip_dist : 8);
   sinfo->num_cull_out = has_prop ? ctx.num_cull_dist_prop : 0;
   sinfo->samplers_used_mask = ctx.samplers_used;
   sinfo->num_consts = ctx.num_consts;
   sinfo->num_ubos = ctx.num_ubo;
   memcpy(sinfo->ubo_idx, ctx.ubo_idx, ctx.num_ubo * sizeof(*ctx.ubo_idx));
   sinfo->ubo_indirect = ctx.info.dimension_indirect_files & (1 << TGSI_FILE_CONSTANT);
   sinfo->num_inputs = ctx.num_inputs;
   sinfo->num_interps = ctx.num_interps;
   sinfo->num_outputs = ctx.num_outputs;
   sinfo->shadow_samp_mask = ctx.shadow_samp_mask;
   sinfo->glsl_ver = ctx.glsl_ver_required;
   sinfo->gs_out_prim = ctx.gs_out_prim;
   sinfo->so_names = ctx.so_names;
   sinfo->attrib_input_mask = ctx.attrib_input_mask;
   sinfo->sampler_arrays = ctx.sampler_arrays;
   sinfo->num_sampler_arrays = ctx.num_sampler_arrays;
   return glsl_final;
 fail:
   free(ctx.glsl_main);
   free(glsl_final);
   free(glsl_hdr);
   free(ctx.so_names);
   free(ctx.temp_ranges);
   return NULL;
}

static void replace_interp(char *program,
                           const char *var_name,
                           const char *pstring, const char *auxstring)
{
   char *ptr;
   int mylen = strlen(INTERP_PREFIX) + strlen("out vec4 ");

   ptr = strstr(program, var_name);

   if (!ptr)
      return;

   ptr -= mylen;

   memset(ptr, ' ', strlen(INTERP_PREFIX));
   memcpy(ptr, pstring, strlen(pstring));
   memcpy(ptr + strlen(pstring), auxstring, strlen(auxstring));
}

bool vrend_patch_vertex_shader_interpolants(struct vrend_shader_cfg *cfg, char *program,
                                            struct vrend_shader_info *vs_info,
                                            struct vrend_shader_info *fs_info, const char *oprefix, bool flatshade)
{
   int i;
   const char *pstring, *auxstring;
   char glsl_name[64];
   if (!vs_info || !fs_info)
      return true;

   if (!fs_info->interpinfo)
      return true;

   for (i = 0; i < fs_info->num_interps; i++) {
      pstring = get_interp_string(cfg, fs_info->interpinfo[i].interpolate, flatshade);
      if (!pstring)
         continue;

      auxstring = get_aux_string(cfg, fs_info->interpinfo[i].centroid);

      switch (fs_info->interpinfo[i].semantic_name) {
      case TGSI_SEMANTIC_COLOR:
         /* color is a bit trickier */
         if (fs_info->glsl_ver < 140) {
            if (fs_info->interpinfo[i].semantic_index == 1) {
               replace_interp(program, "gl_FrontSecondaryColor", pstring, auxstring);
               replace_interp(program, "gl_BackSecondaryColor", pstring, auxstring);
            } else {
               replace_interp(program, "gl_FrontColor", pstring, auxstring);
               replace_interp(program, "gl_BackColor", pstring, auxstring);
            }
         } else {
            snprintf(glsl_name, 64, "ex_c%d", fs_info->interpinfo[i].semantic_index);
            replace_interp(program, glsl_name, pstring, auxstring);
            snprintf(glsl_name, 64, "ex_bc%d", fs_info->interpinfo[i].semantic_index);
            replace_interp(program, glsl_name, pstring, auxstring);
         }
         break;
      case TGSI_SEMANTIC_GENERIC:
         snprintf(glsl_name, 64, "%s_g%d", oprefix, fs_info->interpinfo[i].semantic_index);
         replace_interp(program, glsl_name, pstring, auxstring);
         break;
      default:
         fprintf(stderr,"unhandled semantic: %x\n", fs_info->interpinfo[i].semantic_name);
         return false;
      }
   }

   if (vrend_dump_shaders)
      fprintf(stderr,"GLSL: post interp:  %s\n", program);
   return true;
}
