/*
 * Copyright 2003 VMware, Inc.
 * Copyright 2009, 2012 Intel Corporation.
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

#include "main/mtypes.h"
#include "main/condrender.h"
#include "swrast/swrast.h"
#include "drivers/common/meta.h"

#include "intel_batchbuffer.h"
#include "intel_blit.h"
#include "intel_fbo.h"
#include "intel_mipmap_tree.h"

#include "brw_context.h"
#include "brw_blorp.h"
#include "brw_defines.h"

#define FILE_DEBUG_FLAG DEBUG_BLIT

static const char *buffer_names[] = {
   [BUFFER_FRONT_LEFT] = "front",
   [BUFFER_BACK_LEFT] = "back",
   [BUFFER_FRONT_RIGHT] = "front right",
   [BUFFER_BACK_RIGHT] = "back right",
   [BUFFER_DEPTH] = "depth",
   [BUFFER_STENCIL] = "stencil",
   [BUFFER_ACCUM] = "accum",
   [BUFFER_AUX0] = "aux0",
   [BUFFER_COLOR0] = "color0",
   [BUFFER_COLOR1] = "color1",
   [BUFFER_COLOR2] = "color2",
   [BUFFER_COLOR3] = "color3",
   [BUFFER_COLOR4] = "color4",
   [BUFFER_COLOR5] = "color5",
   [BUFFER_COLOR6] = "color6",
   [BUFFER_COLOR7] = "color7",
};

static void
debug_mask(const char *name, GLbitfield mask)
{
   GLuint i;

   if (unlikely(INTEL_DEBUG & DEBUG_BLIT)) {
      DBG("%s clear:", name);
      for (i = 0; i < BUFFER_COUNT; i++) {
	 if (mask & (1 << i))
	    DBG(" %s", buffer_names[i]);
      }
      DBG("\n");
   }
}

/**
 * Returns true if the scissor is a noop (cuts out nothing).
 */
static bool
noop_scissor(struct gl_framebuffer *fb)
{
   return fb->_Xmin <= 0 &&
          fb->_Ymin <= 0 &&
          fb->_Xmax >= fb->Width &&
          fb->_Ymax >= fb->Height;
}

/**
 * Implements fast depth clears on gen6+.
 *
 * Fast clears basically work by setting a flag in each of the subspans
 * represented in the HiZ buffer that says "When you need the depth values for
 * this subspan, it's the hardware's current clear value."  Then later rendering
 * can just use the static clear value instead of referencing memory.
 *
 * The tricky part of the implementation is that you have to have the clear
 * value that was used on the depth buffer in place for all further rendering,
 * at least until a resolve to the real depth buffer happens.
 */
static bool
brw_fast_clear_depth(struct gl_context *ctx)
{
   struct brw_context *brw = brw_context(ctx);
   struct gl_framebuffer *fb = ctx->DrawBuffer;
   struct intel_renderbuffer *depth_irb =
      intel_get_renderbuffer(fb, BUFFER_DEPTH);
   struct intel_mipmap_tree *mt = depth_irb->mt;
   struct gl_renderbuffer_attachment *depth_att = &fb->Attachment[BUFFER_DEPTH];

   if (brw->gen < 6)
      return false;

   if (!intel_renderbuffer_has_hiz(depth_irb))
      return false;

   /* We only handle full buffer clears -- otherwise you'd have to track whether
    * a previous clear had happened at a different clear value and resolve it
    * first.
    */
   if ((ctx->Scissor.EnableFlags & 1) && !noop_scissor(fb)) {
      perf_debug("Failed to fast clear %dx%d depth because of scissors.  "
                 "Possible 5%% performance win if avoided.\n",
                 mt->surf.logical_level0_px.width,
                 mt->surf.logical_level0_px.height);
      return false;
   }

   switch (mt->format) {
   case MESA_FORMAT_Z32_FLOAT_S8X24_UINT:
   case MESA_FORMAT_Z24_UNORM_S8_UINT:
      /* From the Sandy Bridge PRM, volume 2 part 1, page 314:
       *
       *     "[DevSNB+]: Several cases exist where Depth Buffer Clear cannot be
       *      enabled (the legacy method of clearing must be performed):
       *
       *      - If the depth buffer format is D32_FLOAT_S8X24_UINT or
       *        D24_UNORM_S8_UINT.
       */
      return false;

   case MESA_FORMAT_Z_UNORM16:
      /* From the Sandy Bridge PRM, volume 2 part 1, page 314:
       *
       *     "[DevSNB+]: Several cases exist where Depth Buffer Clear cannot be
       *      enabled (the legacy method of clearing must be performed):
       *
       *      - DevSNB{W/A}]: When depth buffer format is D16_UNORM and the
       *        width of the map (LOD0) is not multiple of 16, fast clear
       *        optimization must be disabled.
       */
      if (brw->gen == 6 &&
          (minify(mt->surf.phys_level0_sa.width,
                  depth_irb->mt_level - mt->first_level) % 16) != 0)
	 return false;
      break;

   default:
      break;
   }

   const uint32_t num_layers = depth_att->Layered ? depth_irb->layer_count : 1;

   /* If we're clearing to a new clear value, then we need to resolve any clear
    * flags out of the HiZ buffer into the real depth buffer.
    */
   if (mt->fast_clear_color.f32[0] != ctx->Depth.Clear) {
      intel_miptree_prepare_access(brw, mt, 0, INTEL_REMAINING_LEVELS,
                                   0, INTEL_REMAINING_LAYERS,
                                   ISL_AUX_USAGE_HIZ, false);
      mt->fast_clear_color.f32[0] = ctx->Depth.Clear;
   }

   intel_hiz_exec(brw, mt, depth_irb->mt_level,
                  depth_irb->mt_layer, num_layers,
                  BLORP_HIZ_OP_DEPTH_CLEAR);

   /* Now, the HiZ buffer contains data that needs to be resolved to the depth
    * buffer.
    */
   intel_miptree_set_aux_state(brw, mt, depth_irb->mt_level,
                               depth_irb->mt_layer, num_layers,
                               ISL_AUX_STATE_CLEAR);

   return true;
}

/**
 * Called by ctx->Driver.Clear.
 */
static void
brw_clear(struct gl_context *ctx, GLbitfield mask)
{
   struct brw_context *brw = brw_context(ctx);
   struct gl_framebuffer *fb = ctx->DrawBuffer;
   bool partial_clear = ctx->Scissor.EnableFlags && !noop_scissor(fb);

   if (!_mesa_check_conditional_render(ctx))
      return;

   if (mask & (BUFFER_BIT_FRONT_LEFT | BUFFER_BIT_FRONT_RIGHT)) {
      brw->front_buffer_dirty = true;
   }

   intel_prepare_render(brw);
   brw_workaround_depthstencil_alignment(brw, partial_clear ? 0 : mask);

   if (mask & BUFFER_BIT_DEPTH) {
      if (brw_fast_clear_depth(ctx)) {
	 DBG("fast clear: depth\n");
	 mask &= ~BUFFER_BIT_DEPTH;
      }
   }

   if (mask & BUFFER_BIT_STENCIL) {
      struct intel_renderbuffer *stencil_irb =
         intel_get_renderbuffer(fb, BUFFER_STENCIL);
      struct intel_mipmap_tree *mt = stencil_irb->mt;
      if (mt && mt->stencil_mt)
         mt->stencil_mt->r8stencil_needs_update = true;
   }

   if (mask & BUFFER_BITS_COLOR) {
      brw_blorp_clear_color(brw, fb, mask, partial_clear,
                            ctx->Color.sRGBEnabled);
      debug_mask("blorp color", mask & BUFFER_BITS_COLOR);
      mask &= ~BUFFER_BITS_COLOR;
   }

   if (brw->gen >= 6 && (mask & BUFFER_BITS_DEPTH_STENCIL)) {
      brw_blorp_clear_depth_stencil(brw, fb, mask, partial_clear);
      debug_mask("blorp depth/stencil", mask & BUFFER_BITS_DEPTH_STENCIL);
      mask &= ~BUFFER_BITS_DEPTH_STENCIL;
   }

   GLbitfield tri_mask = mask & (BUFFER_BIT_STENCIL |
                                 BUFFER_BIT_DEPTH);

   if (tri_mask) {
      debug_mask("tri", tri_mask);
      mask &= ~tri_mask;
      _mesa_meta_glsl_Clear(&brw->ctx, tri_mask);
   }

   /* Any strange buffers get passed off to swrast.  The only thing that
    * should be left at this point is the accumulation buffer.
    */
   assert((mask & ~BUFFER_BIT_ACCUM) == 0);
   if (mask) {
      debug_mask("swrast", mask);
      _swrast_Clear(ctx, mask);
   }
}


void
intelInitClearFuncs(struct dd_function_table *functions)
{
   functions->Clear = brw_clear;
}
