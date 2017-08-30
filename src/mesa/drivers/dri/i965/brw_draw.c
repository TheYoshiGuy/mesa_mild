/*
 * Copyright 2003 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/errno.h>

#include "main/context.h"
#include "main/condrender.h"
#include "main/samplerobj.h"
#include "main/state.h"
#include "main/enums.h"
#include "main/macros.h"
#include "main/transformfeedback.h"
#include "main/framebuffer.h"
#include "tnl/tnl.h"
#include "vbo/vbo_context.h"
#include "swrast/swrast.h"
#include "swrast_setup/swrast_setup.h"
#include "drivers/common/meta.h"
#include "util/bitscan.h"

#include "brw_blorp.h"
#include "brw_draw.h"
#include "brw_defines.h"
#include "compiler/brw_eu_defines.h"
#include "brw_context.h"
#include "brw_state.h"

#include "intel_batchbuffer.h"
#include "intel_buffers.h"
#include "intel_fbo.h"
#include "intel_mipmap_tree.h"
#include "intel_buffer_objects.h"

#define FILE_DEBUG_FLAG DEBUG_PRIMS


static const GLenum reduced_prim[GL_POLYGON+1] = {
   [GL_POINTS] = GL_POINTS,
   [GL_LINES] = GL_LINES,
   [GL_LINE_LOOP] = GL_LINES,
   [GL_LINE_STRIP] = GL_LINES,
   [GL_TRIANGLES] = GL_TRIANGLES,
   [GL_TRIANGLE_STRIP] = GL_TRIANGLES,
   [GL_TRIANGLE_FAN] = GL_TRIANGLES,
   [GL_QUADS] = GL_TRIANGLES,
   [GL_QUAD_STRIP] = GL_TRIANGLES,
   [GL_POLYGON] = GL_TRIANGLES
};

/* When the primitive changes, set a state bit and re-validate.  Not
 * the nicest and would rather deal with this by having all the
 * programs be immune to the active primitive (ie. cope with all
 * possibilities).  That may not be realistic however.
 */
static void
brw_set_prim(struct brw_context *brw, const struct _mesa_prim *prim)
{
   struct gl_context *ctx = &brw->ctx;
   uint32_t hw_prim = get_hw_prim_for_gl_prim(prim->mode);

   DBG("PRIM: %s\n", _mesa_enum_to_string(prim->mode));

   /* Slight optimization to avoid the GS program when not needed:
    */
   if (prim->mode == GL_QUAD_STRIP &&
       ctx->Light.ShadeModel != GL_FLAT &&
       ctx->Polygon.FrontMode == GL_FILL &&
       ctx->Polygon.BackMode == GL_FILL)
      hw_prim = _3DPRIM_TRISTRIP;

   if (prim->mode == GL_QUADS && prim->count == 4 &&
       ctx->Light.ShadeModel != GL_FLAT &&
       ctx->Polygon.FrontMode == GL_FILL &&
       ctx->Polygon.BackMode == GL_FILL) {
      hw_prim = _3DPRIM_TRIFAN;
   }

   if (hw_prim != brw->primitive) {
      brw->primitive = hw_prim;
      brw->ctx.NewDriverState |= BRW_NEW_PRIMITIVE;

      if (reduced_prim[prim->mode] != brw->reduced_primitive) {
         brw->reduced_primitive = reduced_prim[prim->mode];
         brw->ctx.NewDriverState |= BRW_NEW_REDUCED_PRIMITIVE;
      }
   }
}

static void
gen6_set_prim(struct brw_context *brw, const struct _mesa_prim *prim)
{
   const struct gl_context *ctx = &brw->ctx;
   uint32_t hw_prim;

   DBG("PRIM: %s\n", _mesa_enum_to_string(prim->mode));

   if (prim->mode == GL_PATCHES) {
      hw_prim = _3DPRIM_PATCHLIST(ctx->TessCtrlProgram.patch_vertices);
   } else {
      hw_prim = get_hw_prim_for_gl_prim(prim->mode);
   }

   if (hw_prim != brw->primitive) {
      brw->primitive = hw_prim;
      brw->ctx.NewDriverState |= BRW_NEW_PRIMITIVE;
      if (prim->mode == GL_PATCHES)
         brw->ctx.NewDriverState |= BRW_NEW_PATCH_PRIMITIVE;
   }
}


/**
 * The hardware is capable of removing dangling vertices on its own; however,
 * prior to Gen6, we sometimes convert quads into trifans (and quad strips
 * into tristrips), since pre-Gen6 hardware requires a GS to render quads.
 * This function manually trims dangling vertices from a draw call involving
 * quads so that those dangling vertices won't get drawn when we convert to
 * trifans/tristrips.
 */
static GLuint
trim(GLenum prim, GLuint length)
{
   if (prim == GL_QUAD_STRIP)
      return length > 3 ? (length - length % 2) : 0;
   else if (prim == GL_QUADS)
      return length - length % 4;
   else
      return length;
}


static void
brw_emit_prim(struct brw_context *brw,
              const struct _mesa_prim *prim,
              uint32_t hw_prim,
              struct brw_transform_feedback_object *xfb_obj,
              unsigned stream)
{
   const struct gen_device_info *devinfo = &brw->screen->devinfo;
   int verts_per_instance;
   int vertex_access_type;
   int indirect_flag;

   DBG("PRIM: %s %d %d\n", _mesa_enum_to_string(prim->mode),
       prim->start, prim->count);

   int start_vertex_location = prim->start;
   int base_vertex_location = prim->basevertex;

   if (prim->indexed) {
      vertex_access_type = devinfo->gen >= 7 ?
         GEN7_3DPRIM_VERTEXBUFFER_ACCESS_RANDOM :
         GEN4_3DPRIM_VERTEXBUFFER_ACCESS_RANDOM;
      start_vertex_location += brw->ib.start_vertex_offset;
      base_vertex_location += brw->vb.start_vertex_bias;
   } else {
      vertex_access_type = devinfo->gen >= 7 ?
         GEN7_3DPRIM_VERTEXBUFFER_ACCESS_SEQUENTIAL :
         GEN4_3DPRIM_VERTEXBUFFER_ACCESS_SEQUENTIAL;
      start_vertex_location += brw->vb.start_vertex_bias;
   }

   /* We only need to trim the primitive count on pre-Gen6. */
   if (devinfo->gen < 6)
      verts_per_instance = trim(prim->mode, prim->count);
   else
      verts_per_instance = prim->count;

   /* If nothing to emit, just return. */
   if (verts_per_instance == 0 && !prim->is_indirect && !xfb_obj)
      return;

   /* If we're set to always flush, do it before and after the primitive emit.
    * We want to catch both missed flushes that hurt instruction/state cache
    * and missed flushes of the render cache as it heads to other parts of
    * the besides the draw code.
    */
   if (brw->always_flush_cache)
      brw_emit_mi_flush(brw);

   /* If indirect, emit a bunch of loads from the indirect BO. */
   if (xfb_obj) {
      indirect_flag = GEN7_3DPRIM_INDIRECT_PARAMETER_ENABLE;

      brw_load_register_mem(brw, GEN7_3DPRIM_VERTEX_COUNT,
                            xfb_obj->prim_count_bo,
                            stream * sizeof(uint32_t));
      BEGIN_BATCH(9);
      OUT_BATCH(MI_LOAD_REGISTER_IMM | (9 - 2));
      OUT_BATCH(GEN7_3DPRIM_INSTANCE_COUNT);
      OUT_BATCH(prim->num_instances);
      OUT_BATCH(GEN7_3DPRIM_START_VERTEX);
      OUT_BATCH(0);
      OUT_BATCH(GEN7_3DPRIM_BASE_VERTEX);
      OUT_BATCH(0);
      OUT_BATCH(GEN7_3DPRIM_START_INSTANCE);
      OUT_BATCH(0);
      ADVANCE_BATCH();
   } else if (prim->is_indirect) {
      struct gl_buffer_object *indirect_buffer = brw->ctx.DrawIndirectBuffer;
      struct brw_bo *bo = intel_bufferobj_buffer(brw,
            intel_buffer_object(indirect_buffer),
            prim->indirect_offset, 5 * sizeof(GLuint), false);

      indirect_flag = GEN7_3DPRIM_INDIRECT_PARAMETER_ENABLE;

      brw_load_register_mem(brw, GEN7_3DPRIM_VERTEX_COUNT, bo,
                            prim->indirect_offset + 0);
      brw_load_register_mem(brw, GEN7_3DPRIM_INSTANCE_COUNT, bo,
                            prim->indirect_offset + 4);

      brw_load_register_mem(brw, GEN7_3DPRIM_START_VERTEX, bo,
                            prim->indirect_offset + 8);
      if (prim->indexed) {
         brw_load_register_mem(brw, GEN7_3DPRIM_BASE_VERTEX, bo,
                               prim->indirect_offset + 12);
         brw_load_register_mem(brw, GEN7_3DPRIM_START_INSTANCE, bo,
                               prim->indirect_offset + 16);
      } else {
         brw_load_register_mem(brw, GEN7_3DPRIM_START_INSTANCE, bo,
                               prim->indirect_offset + 12);
         BEGIN_BATCH(3);
         OUT_BATCH(MI_LOAD_REGISTER_IMM | (3 - 2));
         OUT_BATCH(GEN7_3DPRIM_BASE_VERTEX);
         OUT_BATCH(0);
         ADVANCE_BATCH();
      }
   } else {
      indirect_flag = 0;
   }

   BEGIN_BATCH(devinfo->gen >= 7 ? 7 : 6);

   if (devinfo->gen >= 7) {
      const int predicate_enable =
         (brw->predicate.state == BRW_PREDICATE_STATE_USE_BIT)
         ? GEN7_3DPRIM_PREDICATE_ENABLE : 0;

      OUT_BATCH(CMD_3D_PRIM << 16 | (7 - 2) | indirect_flag | predicate_enable);
      OUT_BATCH(hw_prim | vertex_access_type);
   } else {
      OUT_BATCH(CMD_3D_PRIM << 16 | (6 - 2) |
                hw_prim << GEN4_3DPRIM_TOPOLOGY_TYPE_SHIFT |
                vertex_access_type);
   }
   OUT_BATCH(verts_per_instance);
   OUT_BATCH(start_vertex_location);
   OUT_BATCH(prim->num_instances);
   OUT_BATCH(prim->base_instance);
   OUT_BATCH(base_vertex_location);
   ADVANCE_BATCH();

   if (brw->always_flush_cache)
      brw_emit_mi_flush(brw);
}


static void
brw_merge_inputs(struct brw_context *brw,
                 const struct gl_vertex_array *arrays[])
{
   const struct gen_device_info *devinfo = &brw->screen->devinfo;
   const struct gl_context *ctx = &brw->ctx;
   GLuint i;

   for (i = 0; i < brw->vb.nr_buffers; i++) {
      brw_bo_unreference(brw->vb.buffers[i].bo);
      brw->vb.buffers[i].bo = NULL;
   }
   brw->vb.nr_buffers = 0;

   for (i = 0; i < VERT_ATTRIB_MAX; i++) {
      brw->vb.inputs[i].buffer = -1;
      brw->vb.inputs[i].glarray = arrays[i];
   }

   if (devinfo->gen < 8 && !devinfo->is_haswell) {
      uint64_t mask = ctx->VertexProgram._Current->info.inputs_read;
      /* Prior to Haswell, the hardware can't natively support GL_FIXED or
       * 2_10_10_10_REV vertex formats.  Set appropriate workaround flags.
       */
      while (mask) {
         uint8_t wa_flags = 0;

         i = u_bit_scan64(&mask);

         switch (brw->vb.inputs[i].glarray->Type) {

         case GL_FIXED:
            wa_flags = brw->vb.inputs[i].glarray->Size;
            break;

         case GL_INT_2_10_10_10_REV:
            wa_flags |= BRW_ATTRIB_WA_SIGN;
            /* fallthough */

         case GL_UNSIGNED_INT_2_10_10_10_REV:
            if (brw->vb.inputs[i].glarray->Format == GL_BGRA)
               wa_flags |= BRW_ATTRIB_WA_BGRA;

            if (brw->vb.inputs[i].glarray->Normalized)
               wa_flags |= BRW_ATTRIB_WA_NORMALIZE;
            else if (!brw->vb.inputs[i].glarray->Integer)
               wa_flags |= BRW_ATTRIB_WA_SCALE;

            break;
         }

         if (brw->vb.attrib_wa_flags[i] != wa_flags) {
            brw->vb.attrib_wa_flags[i] = wa_flags;
            brw->ctx.NewDriverState |= BRW_NEW_VS_ATTRIB_WORKAROUNDS;
         }
      }
   }
}

static bool
intel_disable_rb_aux_buffer(struct brw_context *brw, const struct brw_bo *bo)
{
   const struct gl_framebuffer *fb = brw->ctx.DrawBuffer;
   bool found = false;

   for (unsigned i = 0; i < fb->_NumColorDrawBuffers; i++) {
      const struct intel_renderbuffer *irb =
         intel_renderbuffer(fb->_ColorDrawBuffers[i]);

      if (irb && irb->mt->bo == bo) {
         found = brw->draw_aux_buffer_disabled[i] = true;
      }
   }

   return found;
}

/**
 * \brief Resolve buffers before drawing.
 *
 * Resolve the depth buffer's HiZ buffer, resolve the depth buffer of each
 * enabled depth texture, and flush the render cache for any dirty textures.
 */
void
brw_predraw_resolve_inputs(struct brw_context *brw)
{
   const struct gen_device_info *devinfo = &brw->screen->devinfo;
   struct gl_context *ctx = &brw->ctx;
   struct intel_texture_object *tex_obj;

   memset(brw->draw_aux_buffer_disabled, 0,
          sizeof(brw->draw_aux_buffer_disabled));

   /* Resolve depth buffer and render cache of each enabled texture. */
   int maxEnabledUnit = ctx->Texture._MaxEnabledTexImageUnit;
   for (int i = 0; i <= maxEnabledUnit; i++) {
      if (!ctx->Texture.Unit[i]._Current)
	 continue;
      tex_obj = intel_texture_object(ctx->Texture.Unit[i]._Current);
      if (!tex_obj || !tex_obj->mt)
	 continue;

      struct gl_sampler_object *sampler = _mesa_get_samplerobj(ctx, i);
      enum isl_format view_format =
         translate_tex_format(brw, tex_obj->_Format, sampler->sRGBDecode);

      bool aux_supported;
      intel_miptree_prepare_texture(brw, tex_obj->mt, view_format,
                                    &aux_supported);

      if (!aux_supported && devinfo->gen >= 9 &&
          intel_disable_rb_aux_buffer(brw, tex_obj->mt->bo)) {
         perf_debug("Sampling renderbuffer with non-compressible format - "
                    "turning off compression\n");
      }

      brw_render_cache_set_check_flush(brw, tex_obj->mt->bo);

      if (tex_obj->base.StencilSampling ||
          tex_obj->mt->format == MESA_FORMAT_S_UINT8) {
         intel_update_r8stencil(brw, tex_obj->mt);
      }
   }

   /* Resolve color for each active shader image. */
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++) {
      const struct gl_program *prog = ctx->_Shader->CurrentProgram[i];

      if (unlikely(prog && prog->info.num_images)) {
         for (unsigned j = 0; j < prog->info.num_images; j++) {
            struct gl_image_unit *u =
               &ctx->ImageUnits[prog->sh.ImageUnits[j]];
            tex_obj = intel_texture_object(u->TexObj);

            if (tex_obj && tex_obj->mt) {
               intel_miptree_prepare_image(brw, tex_obj->mt);

               if (tex_obj->mt->aux_usage == ISL_AUX_USAGE_CCS_E &&
                   intel_disable_rb_aux_buffer(brw, tex_obj->mt->bo)) {
                  perf_debug("Using renderbuffer as shader image - turning "
                             "off lossless compression\n");
               }

               brw_render_cache_set_check_flush(brw, tex_obj->mt->bo);
            }
         }
      }
   }
}

static void
brw_predraw_resolve_framebuffer(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   struct intel_renderbuffer *depth_irb;

   /* Resolve the depth buffer's HiZ buffer. */
   depth_irb = intel_get_renderbuffer(ctx->DrawBuffer, BUFFER_DEPTH);
   if (depth_irb && depth_irb->mt) {
      intel_miptree_prepare_depth(brw, depth_irb->mt,
                                  depth_irb->mt_level,
                                  depth_irb->mt_layer,
                                  depth_irb->layer_count);
   }

   /* Resolve color buffers for non-coherent framebuffer fetch. */
   if (!ctx->Extensions.MESA_shader_framebuffer_fetch &&
       ctx->FragmentProgram._Current &&
       ctx->FragmentProgram._Current->info.outputs_read) {
      const struct gl_framebuffer *fb = ctx->DrawBuffer;

      for (unsigned i = 0; i < fb->_NumColorDrawBuffers; i++) {
         const struct intel_renderbuffer *irb =
            intel_renderbuffer(fb->_ColorDrawBuffers[i]);

         if (irb) {
            intel_miptree_prepare_fb_fetch(brw, irb->mt, irb->mt_level,
                                           irb->mt_layer, irb->layer_count);
         }
      }
   }

   struct gl_framebuffer *fb = ctx->DrawBuffer;
   for (int i = 0; i < fb->_NumColorDrawBuffers; i++) {
      struct intel_renderbuffer *irb =
         intel_renderbuffer(fb->_ColorDrawBuffers[i]);

      if (irb == NULL || irb->mt == NULL)
         continue;

      intel_miptree_prepare_render(brw, irb->mt, irb->mt_level,
                                   irb->mt_layer, irb->layer_count,
                                   ctx->Color.sRGBEnabled,
                                   ctx->Color.BlendEnabled & (1 << i));
   }
}

/**
 * \brief Call this after drawing to mark which buffers need resolving
 *
 * If the depth buffer was written to and if it has an accompanying HiZ
 * buffer, then mark that it needs a depth resolve.
 *
 * If the color buffer is a multisample window system buffer, then
 * mark that it needs a downsample.
 *
 * Also mark any render targets which will be textured as needing a render
 * cache flush.
 */
static void
brw_postdraw_set_buffers_need_resolve(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   struct gl_framebuffer *fb = ctx->DrawBuffer;

   struct intel_renderbuffer *front_irb = NULL;
   struct intel_renderbuffer *back_irb = intel_get_renderbuffer(fb, BUFFER_BACK_LEFT);
   struct intel_renderbuffer *depth_irb = intel_get_renderbuffer(fb, BUFFER_DEPTH);
   struct intel_renderbuffer *stencil_irb = intel_get_renderbuffer(fb, BUFFER_STENCIL);
   struct gl_renderbuffer_attachment *depth_att = &fb->Attachment[BUFFER_DEPTH];

   if (_mesa_is_front_buffer_drawing(fb))
      front_irb = intel_get_renderbuffer(fb, BUFFER_FRONT_LEFT);

   if (front_irb)
      front_irb->need_downsample = true;
   if (back_irb)
      back_irb->need_downsample = true;
   if (depth_irb) {
      bool depth_written = brw_depth_writes_enabled(brw);
      if (depth_att->Layered) {
         intel_miptree_finish_depth(brw, depth_irb->mt,
                                    depth_irb->mt_level,
                                    depth_irb->mt_layer,
                                    depth_irb->layer_count,
                                    depth_written);
      } else {
         intel_miptree_finish_depth(brw, depth_irb->mt,
                                    depth_irb->mt_level,
                                    depth_irb->mt_layer, 1,
                                    depth_written);
      }
      if (depth_written)
         brw_render_cache_set_add_bo(brw, depth_irb->mt->bo);
   }

   if (ctx->Extensions.ARB_stencil_texturing &&
       stencil_irb && brw->stencil_write_enabled) {
      brw_render_cache_set_add_bo(brw, stencil_irb->mt->bo);
   }

   for (unsigned i = 0; i < fb->_NumColorDrawBuffers; i++) {
      struct intel_renderbuffer *irb =
         intel_renderbuffer(fb->_ColorDrawBuffers[i]);

      if (!irb)
         continue;

      brw_render_cache_set_add_bo(brw, irb->mt->bo);
      intel_miptree_finish_render(brw, irb->mt, irb->mt_level,
                                  irb->mt_layer, irb->layer_count,
                                  ctx->Color.sRGBEnabled,
                                  ctx->Color.BlendEnabled & (1 << i));
   }
}

static void
intel_renderbuffer_move_temp_back(struct brw_context *brw,
                                  struct intel_renderbuffer *irb)
{
   if (irb->align_wa_mt == NULL)
      return;

   brw_render_cache_set_check_flush(brw, irb->align_wa_mt->bo);

   intel_miptree_copy_slice(brw, irb->align_wa_mt, 0, 0,
                            irb->mt,
                            irb->Base.Base.TexImage->Level, irb->mt_layer);

   intel_miptree_reference(&irb->align_wa_mt, NULL);

   /* Finally restore the x,y to correspond to full miptree. */
   intel_renderbuffer_set_draw_offset(irb);

   /* Make sure render surface state gets re-emitted with updated miptree. */
   brw->NewGLState |= _NEW_BUFFERS;
}

static void
brw_postdraw_reconcile_align_wa_slices(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   struct gl_framebuffer *fb = ctx->DrawBuffer;

   struct intel_renderbuffer *depth_irb =
      intel_get_renderbuffer(fb, BUFFER_DEPTH);
   struct intel_renderbuffer *stencil_irb =
      intel_get_renderbuffer(fb, BUFFER_STENCIL);

   if (depth_irb && depth_irb->align_wa_mt)
      intel_renderbuffer_move_temp_back(brw, depth_irb);

   if (stencil_irb && stencil_irb->align_wa_mt)
      intel_renderbuffer_move_temp_back(brw, stencil_irb);

   for (unsigned i = 0; i < fb->_NumColorDrawBuffers; i++) {
      struct intel_renderbuffer *irb =
         intel_renderbuffer(fb->_ColorDrawBuffers[i]);

      if (!irb || irb->align_wa_mt == NULL)
         continue;

      intel_renderbuffer_move_temp_back(brw, irb);
   }
}

/* May fail if out of video memory for texture or vbo upload, or on
 * fallback conditions.
 */
static void
brw_try_draw_prims(struct gl_context *ctx,
                   const struct gl_vertex_array *arrays[],
                   const struct _mesa_prim *prims,
                   GLuint nr_prims,
                   const struct _mesa_index_buffer *ib,
                   bool index_bounds_valid,
                   GLuint min_index,
                   GLuint max_index,
                   struct brw_transform_feedback_object *xfb_obj,
                   unsigned stream,
                   struct gl_buffer_object *indirect)
{
   struct brw_context *brw = brw_context(ctx);
   const struct gen_device_info *devinfo = &brw->screen->devinfo;
   GLuint i;
   bool fail_next = false;

   if (ctx->NewState)
      _mesa_update_state(ctx);

   /* We have to validate the textures *before* checking for fallbacks;
    * otherwise, the software fallback won't be able to rely on the
    * texture state, the firstLevel and lastLevel fields won't be
    * set in the intel texture object (they'll both be 0), and the
    * software fallback will segfault if it attempts to access any
    * texture level other than level 0.
    */
   brw_validate_textures(brw);

   /* Find the highest sampler unit used by each shader program.  A bit-count
    * won't work since ARB programs use the texture unit number as the sampler
    * index.
    */
   brw->wm.base.sampler_count =
      util_last_bit(ctx->FragmentProgram._Current->SamplersUsed);
   brw->gs.base.sampler_count = ctx->GeometryProgram._Current ?
      util_last_bit(ctx->GeometryProgram._Current->SamplersUsed) : 0;
   brw->tes.base.sampler_count = ctx->TessEvalProgram._Current ?
      util_last_bit(ctx->TessEvalProgram._Current->SamplersUsed) : 0;
   brw->tcs.base.sampler_count = ctx->TessCtrlProgram._Current ?
      util_last_bit(ctx->TessCtrlProgram._Current->SamplersUsed) : 0;
   brw->vs.base.sampler_count =
      util_last_bit(ctx->VertexProgram._Current->SamplersUsed);

   intel_prepare_render(brw);

   /* This workaround has to happen outside of brw_upload_render_state()
    * because it may flush the batchbuffer for a blit, affecting the state
    * flags.
    */
   brw_workaround_depthstencil_alignment(brw, 0);

   /* Resolves must occur after updating renderbuffers, updating context state,
    * and finalizing textures but before setting up any hardware state for
    * this draw call.
    */
   brw_predraw_resolve_inputs(brw);
   brw_predraw_resolve_framebuffer(brw);

   /* Bind all inputs, derive varying and size information:
    */
   brw_merge_inputs(brw, arrays);

   brw->ib.ib = ib;
   brw->ctx.NewDriverState |= BRW_NEW_INDICES;

   brw->vb.index_bounds_valid = index_bounds_valid;
   brw->vb.min_index = min_index;
   brw->vb.max_index = max_index;
   brw->ctx.NewDriverState |= BRW_NEW_VERTICES;

   for (i = 0; i < nr_prims; i++) {
      int estimated_max_prim_size;
      const int sampler_state_size = 16;

      estimated_max_prim_size = 512; /* batchbuffer commands */
      estimated_max_prim_size += BRW_MAX_TEX_UNIT *
         (sampler_state_size + sizeof(struct gen5_sampler_default_color));
      estimated_max_prim_size += 1024; /* gen6 VS push constants */
      estimated_max_prim_size += 1024; /* gen6 WM push constants */
      estimated_max_prim_size += 512; /* misc. pad */

      /* Flag BRW_NEW_DRAW_CALL on every draw.  This allows us to have
       * atoms that happen on every draw call.
       */
      brw->ctx.NewDriverState |= BRW_NEW_DRAW_CALL;

      /* Flush the batch if it's approaching full, so that we don't wrap while
       * we've got validated state that needs to be in the same batch as the
       * primitives.
       */
      intel_batchbuffer_require_space(brw, estimated_max_prim_size, RENDER_RING);
      intel_batchbuffer_save_state(brw);

      if (brw->num_instances != prims[i].num_instances ||
          brw->basevertex != prims[i].basevertex ||
          brw->baseinstance != prims[i].base_instance) {
         brw->num_instances = prims[i].num_instances;
         brw->basevertex = prims[i].basevertex;
         brw->baseinstance = prims[i].base_instance;
         if (i > 0) { /* For i == 0 we just did this before the loop */
            brw->ctx.NewDriverState |= BRW_NEW_VERTICES;
            brw_merge_inputs(brw, arrays);
         }
      }

      /* Determine if we need to flag BRW_NEW_VERTICES for updating the
       * gl_BaseVertexARB or gl_BaseInstanceARB values. For indirect draw, we
       * always flag if the shader uses one of the values. For direct draws,
       * we only flag if the values change.
       */
      const int new_basevertex =
         prims[i].indexed ? prims[i].basevertex : prims[i].start;
      const int new_baseinstance = prims[i].base_instance;
      const struct brw_vs_prog_data *vs_prog_data =
         brw_vs_prog_data(brw->vs.base.prog_data);
      if (i > 0) {
         const bool uses_draw_parameters =
            vs_prog_data->uses_basevertex ||
            vs_prog_data->uses_baseinstance;

         if ((uses_draw_parameters && prims[i].is_indirect) ||
             (vs_prog_data->uses_basevertex &&
              brw->draw.params.gl_basevertex != new_basevertex) ||
             (vs_prog_data->uses_baseinstance &&
              brw->draw.params.gl_baseinstance != new_baseinstance))
            brw->ctx.NewDriverState |= BRW_NEW_VERTICES;
      }

      brw->draw.params.gl_basevertex = new_basevertex;
      brw->draw.params.gl_baseinstance = new_baseinstance;
      brw_bo_unreference(brw->draw.draw_params_bo);

      if (prims[i].is_indirect) {
         /* Point draw_params_bo at the indirect buffer. */
         brw->draw.draw_params_bo =
            intel_buffer_object(ctx->DrawIndirectBuffer)->buffer;
         brw_bo_reference(brw->draw.draw_params_bo);
         brw->draw.draw_params_offset =
            prims[i].indirect_offset + (prims[i].indexed ? 12 : 8);
      } else {
         /* Set draw_params_bo to NULL so brw_prepare_vertices knows it
          * has to upload gl_BaseVertex and such if they're needed.
          */
         brw->draw.draw_params_bo = NULL;
         brw->draw.draw_params_offset = 0;
      }

      /* gl_DrawID always needs its own vertex buffer since it's not part of
       * the indirect parameter buffer. If the program uses gl_DrawID we need
       * to flag BRW_NEW_VERTICES. For the first iteration, we don't have
       * valid vs_prog_data, but we always flag BRW_NEW_VERTICES before
       * the loop.
       */
      brw->draw.gl_drawid = prims[i].draw_id;
      brw_bo_unreference(brw->draw.draw_id_bo);
      brw->draw.draw_id_bo = NULL;
      if (i > 0 && vs_prog_data->uses_drawid)
         brw->ctx.NewDriverState |= BRW_NEW_VERTICES;

      if (devinfo->gen < 6)
         brw_set_prim(brw, &prims[i]);
      else
         gen6_set_prim(brw, &prims[i]);

retry:

      /* Note that before the loop, brw->ctx.NewDriverState was set to != 0, and
       * that the state updated in the loop outside of this block is that in
       * *_set_prim or intel_batchbuffer_flush(), which only impacts
       * brw->ctx.NewDriverState.
       */
      if (brw->ctx.NewDriverState) {
         brw->no_batch_wrap = true;
         brw_upload_render_state(brw);
      }

      brw_emit_prim(brw, &prims[i], brw->primitive, xfb_obj, stream);

      brw->no_batch_wrap = false;

      if (!brw_batch_has_aperture_space(brw, 0)) {
         if (!fail_next) {
            intel_batchbuffer_reset_to_saved(brw);
            intel_batchbuffer_flush(brw);
            fail_next = true;
            goto retry;
         } else {
            int ret = intel_batchbuffer_flush(brw);
            WARN_ONCE(ret == -ENOSPC,
                      "i965: Single primitive emit exceeded "
                      "available aperture space\n");
         }
      }

      /* Now that we know we haven't run out of aperture space, we can safely
       * reset the dirty bits.
       */
      if (brw->ctx.NewDriverState)
         brw_render_state_finished(brw);
   }

   if (brw->always_flush_batch)
      intel_batchbuffer_flush(brw);

   brw_program_cache_check_size(brw);
   brw_postdraw_reconcile_align_wa_slices(brw);
   brw_postdraw_set_buffers_need_resolve(brw);

   return;
}

void
brw_draw_prims(struct gl_context *ctx,
               const struct _mesa_prim *prims,
               GLuint nr_prims,
               const struct _mesa_index_buffer *ib,
               GLboolean index_bounds_valid,
               GLuint min_index,
               GLuint max_index,
               struct gl_transform_feedback_object *gl_xfb_obj,
               unsigned stream,
               struct gl_buffer_object *indirect)
{
   struct brw_context *brw = brw_context(ctx);
   const struct gl_vertex_array **arrays = ctx->Array._DrawArrays;
   struct brw_transform_feedback_object *xfb_obj =
      (struct brw_transform_feedback_object *) gl_xfb_obj;

   if (!brw_check_conditional_render(brw))
      return;

   /* Handle primitive restart if needed */
   if (brw_handle_primitive_restart(ctx, prims, nr_prims, ib, indirect)) {
      /* The draw was handled, so we can exit now */
      return;
   }

   /* Do GL_SELECT and GL_FEEDBACK rendering using swrast, even though it
    * won't support all the extensions we support.
    */
   if (ctx->RenderMode != GL_RENDER) {
      perf_debug("%s render mode not supported in hardware\n",
                 _mesa_enum_to_string(ctx->RenderMode));
      _swsetup_Wakeup(ctx);
      _tnl_wakeup(ctx);
      _tnl_draw_prims(ctx, prims, nr_prims, ib,
                      index_bounds_valid, min_index, max_index, NULL, 0, NULL);
      return;
   }

   /* If we're going to have to upload any of the user's vertex arrays, then
    * get the minimum and maximum of their index buffer so we know what range
    * to upload.
    */
   if (!index_bounds_valid && !vbo_all_varyings_in_vbos(arrays)) {
      perf_debug("Scanning index buffer to compute index buffer bounds.  "
                 "Use glDrawRangeElements() to avoid this.\n");
      vbo_get_minmax_indices(ctx, prims, ib, &min_index, &max_index, nr_prims);
      index_bounds_valid = true;
   }

   /* Try drawing with the hardware, but don't do anything else if we can't
    * manage it.  swrast doesn't support our featureset, so we can't fall back
    * to it.
    */
   brw_try_draw_prims(ctx, arrays, prims, nr_prims, ib, index_bounds_valid,
                      min_index, max_index, xfb_obj, stream, indirect);
}

void
brw_draw_init(struct brw_context *brw)
{
   struct gl_context *ctx = &brw->ctx;
   struct vbo_context *vbo = vbo_context(ctx);

   /* Register our drawing function:
    */
   vbo->draw_prims = brw_draw_prims;

   for (int i = 0; i < VERT_ATTRIB_MAX; i++)
      brw->vb.inputs[i].buffer = -1;
   brw->vb.nr_buffers = 0;
   brw->vb.nr_enabled = 0;
}

void
brw_draw_destroy(struct brw_context *brw)
{
   unsigned i;

   for (i = 0; i < brw->vb.nr_buffers; i++) {
      brw_bo_unreference(brw->vb.buffers[i].bo);
      brw->vb.buffers[i].bo = NULL;
   }
   brw->vb.nr_buffers = 0;

   for (i = 0; i < brw->vb.nr_enabled; i++) {
      brw->vb.enabled[i]->buffer = -1;
   }
   brw->vb.nr_enabled = 0;

   brw_bo_unreference(brw->ib.bo);
   brw->ib.bo = NULL;
}
