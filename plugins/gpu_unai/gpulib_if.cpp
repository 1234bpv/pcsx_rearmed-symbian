/***************************************************************************
*   Copyright (C) 2010 PCSX4ALL Team                                      *
*   Copyright (C) 2010 Unai                                               *
*   Copyright (C) 2011 notaz                                              *
*   Copyright (C) 2016 Senquack (dansilsby <AT> gmail <DOT> com)          *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
***************************************************************************/

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arm_features.h"
#include "../gpulib/gpu.h"
#include "old/if.h"

//#include "port.h"
#include "gpu_unai.h"

// GPU fixed point math
#include "gpu_fixedpoint.h"

// Inner loop driver instantiation file
#ifdef UNAI_DRAWSTATS
// The probe skips the whole polygon so the command list can be priced alone.
#define UNAI_POLY_PROBE if (unai_spanOff < 2)
#else
#define UNAI_POLY_PROBE
#endif
#include "gpu_inner.h"

// GPU internal image drawing functions
#include "gpu_raster_image.h"

// GPU internal line drawing functions
#include "gpu_raster_line.h"

// GPU internal polygon drawing functions
#include "gpu_raster_polygon.h"

// GPU internal sprite drawing functions
#include "gpu_raster_sprite.h"

// GPU command buffer execution/store
#include "gpu_command.h"

/////////////////////////////////////////////////////////////////////////////

#ifndef GPU_UNAI_NO_OLD
#define IS_OLD_RENDERER() gpu_unai.config.old_renderer
#else
#define IS_OLD_RENDERER() false
#endif

// Rasterizer configuration for frontends that do not call GPUrearmedCallbacks.
// Without this, gpu_unai_config_ext remains zeroed: not only hacks are disabled, but
// also lighting and semi-transparency, meaning textures are drawn without modulation.
// Call BEFORE renderer_init - that is where the config is copied into the working struct.
extern "C" void gpu_unai_set_config(int pixel_skip, int ilace_force,
                                    int lighting, int fast_lighting, int blending)
{
  gpu_unai_config_ext.pixel_skip    = pixel_skip ? 1 : 0;
  gpu_unai_config_ext.ilace_force   = ilace_force & 7;
  gpu_unai_config_ext.lighting      = lighting ? 1 : 0;
  gpu_unai_config_ext.fast_lighting = fast_lighting ? 1 : 0;
  gpu_unai_config_ext.blending      = blending ? 1 : 0;
}

// The MBX rasterizer lives in the frontend because the GL context is there; hence weak symbols -
// a regular build links without it. It receives the same packets that gpu_unai just
// rendered, rather than replacing them: software VRAM remains authoritative for
// reads, copies, and display while the branch is work-in-progress, and at any moment
// both images can be compared on the same frame.
extern "C" {
  int  glrEnabled(void) __attribute__((weak));
  void glrSetVram(const void *vram) __attribute__((weak));
  void glrInvalidate(void) __attribute__((weak));
  void glrInvalidateRect(int x, int y, int w, int h) __attribute__((weak));
  void glrPrim(unsigned cmd, const unsigned *packet, const int *state)
    __attribute__((weak));
}

// Switch for disabling the software pass. The frontend sets it to 1 alongside the
// "swoff" argument: the scene is then rendered only by MBX. VRAM after this contains only what
// the CPU itself wrote (texture uploads, copies), so in a game that
// renders to VRAM and reads the result back, the image will be incomplete - this is a known
// benchmark tradeoff, not a bug.
int gpu_unai_sw_off;

#ifdef GPU_UNAI_PRIMSTATS
gpu_unai_primstats_t gpu_unai_primstats;
u32 gpu_unai_primstats_cur;

// Set of texture states of the current frame, open addressing. Instead of
// clearing the table at frame boundaries, each entry carries a frame generation number, so transition to
// the next frame is just a counter increment.
#define PRIMSTAT_TEXSLOTS 512
static u32 ps_tex_key[PRIMSTAT_TEXSLOTS], ps_tex_gen[PRIMSTAT_TEXSLOTS];
static u32 ps_tp_key[PRIMSTAT_TEXSLOTS], ps_tp_gen[PRIMSTAT_TEXSLOTS];
static u32 ps_gen = 1;
static u32 ps_cnt_tex, ps_cnt_tp;
static u32 ps_last_tex, ps_last_tp;

// 1 if the key has already been seen in this frame; otherwise remembers it and returns 0.
static int primstat_seen(u32 *tab, u32 *gen, u32 key)
{
  u32 h = (key * 0x9E3779B1u) >> 23;
  for (u32 n = 0; n < PRIMSTAT_TEXSLOTS; n++) {
    u32 s = (h + n) & (PRIMSTAT_TEXSLOTS - 1);
    if (gen[s] != ps_gen) { gen[s] = ps_gen; tab[s] = key; return 0; }
    if (tab[s] == key) return 1;
  }
  return 1;  // table full: do not count as new to avoid overcounting
}

// Frame boundary. Only the frontend knows it, so it calls this.
extern "C" void gpu_unai_primstats_frame(void)
{
  gpu_unai_primstats.sum_tex += ps_cnt_tex;
  gpu_unai_primstats.sum_tp  += ps_cnt_tp;
  if (ps_cnt_tex > gpu_unai_primstats.max_tex)
    gpu_unai_primstats.max_tex = ps_cnt_tex;
  ps_cnt_tex = ps_cnt_tp = 0;
  ps_last_tex = ps_last_tp = 0;
  ps_gen++;
}

// Returns counters as a u32 array in declaration order of struct fields; formatted by
// the frontend. This avoids needing snprintf - Symbian libc does not declare it.
extern "C" int gpu_unai_get_primstats(unsigned *out, int max)
{
  const u32 *p = (const u32 *)&gpu_unai_primstats;
  int n = (int)(sizeof(gpu_unai_primstats) / sizeof(u32));
  if (n > max) n = max;
  for (int i = 0; i < n; i++)
    out[i] = p[i];
  return n;
}
#endif

// Same, but after renderer_init: copies config into working struct so that
// menu changes take effect without a restart. Mirrors renderer_init lines below -
// both places must be updated.
extern "C" void gpu_unai_apply_config(void)
{
  gpu_unai.config = gpu_unai_config_ext;
  gpu_unai.inn.ilace_mask = gpu_unai.config.ilace_force;
}

int renderer_init(void)
{
  memset((void*)&gpu_unai, 0, sizeof(gpu_unai));
  gpu_unai.vram = (le16_t *)gpu.vram;
  // The MBX rasterizer decodes texture pages itself, so it needs the same
  // VRAM. Pass it once - it does not relocate during the process.
  if (glrSetVram)
    glrSetVram(gpu.vram);

  // Original standalone gpu_unai initialized TextureWindow[]. I added the
  //  same behavior here, since it seems unsafe to leave [2],[3] unset when
  //  using HLE and Rearmed gpu_neon sets this similarly on init. -senquack
  gpu_unai.TextureWindow[0] = 0;
  gpu_unai.TextureWindow[1] = 0;
  gpu_unai.TextureWindow[2] = 255;
  gpu_unai.TextureWindow[3] = 255;
  //senquack - new vars must be updated whenever texture window is changed:
  //           (used for polygon-drawing in gpu_inner.h, gpu_raster_polygon.h)
  gpu_unai.inn.mask_v00u =
    ((u32)gpu_unai.TextureWindow[3] << 24) | gpu_unai.TextureWindow[2];
  if (unai_texFlat) gpu_unai.inn.mask_v00u = 0;

  // Configuration options
  gpu_unai.config = gpu_unai_config_ext;
  //senquack - disabled, not sure this is needed and would require modifying
  // sprite-span functions, perhaps unnecessarily. No Abe Oddysey hack was
  // present in latest PCSX4ALL sources we were using.
  //gpu_unai.config.enableAbbeyHack = gpu_unai_config_ext.abe_hack;
  gpu_unai.inn.ilace_mask = gpu_unai.config.ilace_force;

#if defined(GPU_UNAI_USE_INT_DIV_MULTINV) || (!defined(GPU_UNAI_NO_OLD) && !defined(GPU_UNAI_USE_FLOATMATH))
  // s_invTable
  for(int i=1;i<=(1<<TABLE_BITS);++i)
  {
    double v = 1.0 / double(i);
#ifdef GPU_TABLE_10_BITS
    v *= double(0xffffffff>>1);
#else
    v *= double(0x80000000);
#endif
    s_invTable[i-1]=s32(v);
  }
#endif

#ifdef GPU_UNAI_USE_YDIV_TABLE
  // ceil(2^29/d); [0] is not addressed due to guard checks against zero height
  s_yRecip[0] = 0;
  for (int d = 1; d < CHKMAX_Y; ++d)
    s_yRecip[d] = (s32)((((u32)1 << GPU_YDIV_BITS) + d - 1) / (u32)d);
#endif

  SetupLightLUT();
  SetupDitheringConstants();

  return 0;
}

void renderer_finish(void)
{
}

void renderer_notify_screen_change(const struct psx_gpu_screen *screen)
{
  gpu_unai.inn.ilace_mask = gpu_unai.config.ilace_force;

  if (gpu.state.downscale_enable)
  {
    gpu_unai.inn.ilace_mask |= !!(gpu.status & PSX_GPU_STATUS_INTERLACE);
  }

  /*
  printf("res change hres: %d   vres: %d   depth: %d   ilace_mask: %d\n",
      gpu.screen.hres, gpu.screen.vres, (gpu.status & PSX_GPU_STATUS_RGB24) ? 24 : 15,
      gpu_unai.ilace_mask);
  */
}

#ifdef USE_GPULIB
// Handles GP0 draw settings commands 0xE1...0xE6
static void gpuGP0Cmd_0xEx(gpu_unai_t &gpu_unai, u32 cmd_word)
{
  // Assume incoming GP0 command is 0xE1..0xE6, convert to 1..6
  u8 num = (cmd_word >> 24) & 7;
  switch (num) {
    case 1: {
      // GP0(E1h) - Draw Mode setting (aka "Texpage")
      u32 cur_texpage = gpu_unai.GPU_GP1 & 0x7FF;
      u32 new_texpage = cmd_word & 0x7FF;
      if (cur_texpage != new_texpage) {
        gpu_unai.GPU_GP1 = (gpu_unai.GPU_GP1 & ~0x7FF) | new_texpage;
        gpuSetTexture(gpu_unai.GPU_GP1);
      }
    } break;

    case 2: {
      // GP0(E2h) - Texture Window setting
      if (cmd_word != gpu_unai.TextureWindowCur) {
        static const u8 TextureMask[32] = {
          255, 7, 15, 7, 31, 7, 15, 7, 63, 7, 15, 7, 31, 7, 15, 7,
          127, 7, 15, 7, 31, 7, 15, 7, 63, 7, 15, 7, 31, 7, 15, 7
        };
        gpu_unai.TextureWindowCur = cmd_word;
        gpu_unai.TextureWindow[0] = ((cmd_word >> 10) & 0x1F) << 3;
        gpu_unai.TextureWindow[1] = ((cmd_word >> 15) & 0x1F) << 3;
        gpu_unai.TextureWindow[2] = TextureMask[(cmd_word >> 0) & 0x1F];
        gpu_unai.TextureWindow[3] = TextureMask[(cmd_word >> 5) & 0x1F];
        gpu_unai.TextureWindow[0] &= ~gpu_unai.TextureWindow[2];
        gpu_unai.TextureWindow[1] &= ~gpu_unai.TextureWindow[3];

        // Inner loop vars must be updated whenever texture window is changed:
        gpu_unai.inn.mask_v00u =
          ((u32)gpu_unai.TextureWindow[3] << 24) | gpu_unai.TextureWindow[2];
  if (unai_texFlat) gpu_unai.inn.mask_v00u = 0;

        gpuSetTexture(gpu_unai.GPU_GP1);
      }
    } break;

    case 3: {
      // GP0(E3h) - Set Drawing Area top left (X1,Y1)
      gpu_unai.DrawingArea[0] = cmd_word         & 0x3FF;
      gpu_unai.DrawingArea[1] = (cmd_word >> 10) & 0x3FF;
    } break;

    case 4: {
      // GP0(E4h) - Set Drawing Area bottom right (X2,Y2)
      gpu_unai.DrawingArea[2] = (cmd_word         & 0x3FF) + 1;
      gpu_unai.DrawingArea[3] = ((cmd_word >> 10) & 0x3FF) + 1;
    } break;

    case 5: {
      // GP0(E5h) - Set Drawing Offset (X,Y)
      gpu_unai.DrawingOffset[0] = GPU_EXPANDSIGN(cmd_word);
      gpu_unai.DrawingOffset[1] = GPU_EXPANDSIGN(cmd_word >> 11);
    } break;

    case 6: {
      // GP0(E6h) - Mask Bit Setting
      gpu_unai.Masking  = (cmd_word & 0x2) <<  1;
      gpu_unai.PixelMSB = (cmd_word & 0x1) <<  8;
    } break;
  }
}
#endif

#include "../gpulib/gpu_timing.h"

// Strip lower 3 bits of each color and determine if lighting should be used:
static inline bool need_lighting(u32 rgb_raw)
{
  return (rgb_raw & HTOLE32(0xF8F8F8)) != HTOLE32(0x808080);
}

static inline void textured_sprite(int &cpu_cycles_sum, int &cpu_cycles)
{
  u32 PRIM = le32_to_u32(gpu_unai.PacketBuffer.U4[0]) >> 24;
  gpuSetCLUT(le32_to_u32(gpu_unai.PacketBuffer.U4[2]) >> 16);
  u32 driver_idx = Blending_Mode | gpu_unai.TEXT_MODE | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>1);
  s32 w = 0, h = 0;

  //senquack - Only color 808080h-878787h allows skipping lighting calculation:
  // This fixes Silent Hill running animation on loading screens:
  // (On PSX, color values 0x00-0x7F darken the source texture's color,
  //  0x81-FF lighten textures (ultimately clamped to 0x1F),
  //  0x80 leaves source texture color unchanged, HOWEVER,
  //   gpu_unai uses a simple lighting LUT whereby only the upper
  //   5 bits of an 8-bit color are used, so 0x80-0x87 all behave as
  //   0x80.
  //
  // NOTE: I've changed all textured sprite draw commands here and
  //  elsewhere to use proper behavior, but left poly commands
  //  alone, I don't want to slow rendering down too much. (TODO)
  if (need_lighting(le32_raw(gpu_unai.PacketBuffer.U4[0])))
    driver_idx |= Lighting;
  PS driver = gpuSpriteDrivers[driver_idx];
  // designated initialisers are not available in C++ before GCC 4.7
  PtrUnion packet;
  packet.ptr = (void*)&gpu_unai.PacketBuffer;
  gpuDrawS(packet, driver, &w, &h);
  gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(w, h));
}

extern const unsigned char cmd_lengths[256];


// MBX rasterizer hook. Extracted separately because primitive must be submitted not
// only from the end of the dispatcher: quad-to-sprite simplification (prim_try_simplify_quad_*)
// overwrites PacketBuffer with a sprite structure and exits the switch, so after
// it there is NO longer the incoming packet, while command code remains a quad
// command code. Parsed as a quad, such a buffer produced vertices from width and
// height fields - on hardware this appeared as banners squished into the screen corner.
// Field order mirrors enum TGlrState in glrender.cpp.
static void glr_send(u32 cmd)
{
  int st[12];
  st[0] = gpu_unai.DrawingOffset[0];
  st[1] = gpu_unai.DrawingOffset[1];
  st[2] = gpu_unai.DrawingArea[0];
  st[3] = gpu_unai.DrawingArea[1];
  st[4] = gpu_unai.DrawingArea[2];
  st[5] = gpu_unai.DrawingArea[3];
  st[6] = (gpu_unai.BLEND_MODE >> 3) & 3;
  st[7] = gpu_unai.TEXT_MODE;
  st[8] = (int)(gpu_unai.inn.TBA - gpu_unai.vram);
  st[9] = (int)((gpu_unai.inn.CBA - gpu_unai.vram) >> 4);
  // TBA already contains texture window origin (gpuSetTexture), and window masks
  // are embedded by the rasterizer into the unpacked page itself.
  st[10] = gpu_unai.TextureWindow[2];
  st[11] = gpu_unai.TextureWindow[3];
  glrPrim(cmd, (const unsigned *)&gpu_unai.PacketBuffer, st);
}

int renderer_do_cmd_list(u32 *list_, int list_len, uint32_t *ex_regs,
 int *cycles_sum_out, int *cycles_last, int *last_cmd)
{
  int cpu_cycles_sum = 0, cpu_cycles = *cycles_last;
  u32 cmd = 0, len, i;
  le32_t *list = (le32_t *)list_;
  le32_t *list_start = list;
  le32_t *list_end = list + list_len;

  if (IS_OLD_RENDERER()) {
    return oldunai_do_cmd_list(list_, list_len, ex_regs,
             cycles_sum_out, cycles_last, last_cmd);
  }

  for (; list < list_end; list += 1 + len)
  {
    cmd = le32_to_u32(*list) >> 24;
    len = cmd_lengths[cmd];
    if (list + 1 + len > list_end) {
      cmd = -1;
      break;
    }

    #define PRIM cmd
    gpu_unai.PacketBuffer.U4[0] = list[0];
    for (i = 1; i <= len; i++)
      gpu_unai.PacketBuffer.U4[i] = list[i];

    PtrUnion packet;
    packet.ptr = (void*)&gpu_unai.PacketBuffer;
    // Set where primitive has already been submitted to rasterizer from inside the case.
    int glr_sent = 0;

    switch (cmd)
    {
      case 0x02:
        gpuClearImage(packet);
        // Fill also overwrites VRAM, and thus might affect a texture page
        // or palette: the unpacked page cache must be notified about it.
        if (glrInvalidateRect)
          glrInvalidateRect(le16_to_u16(packet.U2[2]) & 0x3f0,
            le16_to_u16(packet.U2[3]) & 0x1ff,
            ((le16_to_u16(packet.U2[4]) & 0x3ff) + 15) & ~15,
            le16_to_u16(packet.U2[5]) & 0x1ff);
        gput_sum(cpu_cycles_sum, cpu_cycles,
           gput_fill(le16_to_s16(packet.U2[4]) & 0x3ff, le16_to_s16(packet.U2[5]) & 0x1ff));
        break;

      case 0x20:
      case 0x21:
      case 0x22:
      case 0x23: {          // Monochrome 3-pt poly
        PP driver = gpuPolySpanDrivers[
          Blending_Mode |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB
        ];
#ifdef UNAI_DRAWSTATS
        unai_drawCnt[10]++;
#endif
        UNAI_POLY_PROBE gpuDrawPolyF(packet, driver, false);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base());
      } break;

      case 0x24:
      case 0x25:
      case 0x26:
      case 0x27: {          // Textured 3-pt poly
        gpuSetCLUT   (le32_to_u32(gpu_unai.PacketBuffer.U4[2]) >> 16);
        gpuSetTexture(le32_to_u32(gpu_unai.PacketBuffer.U4[4]) >> 16);

        u32 driver_idx =
          Dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB;

        if (!FastLightingEnabled()) {
          driver_idx |= Lighting;
        } else {
          if (!((gpu_unai.PacketBuffer.U1[0]>0x5F) && (gpu_unai.PacketBuffer.U1[1]>0x5F) && (gpu_unai.PacketBuffer.U1[2]>0x5F)))
            driver_idx |= Lighting;
        }

        PP driver = gpuPolySpanDrivers[driver_idx];
#ifdef UNAI_DRAWSTATS
        unai_drawCnt[10]++;
#endif
        UNAI_POLY_PROBE gpuDrawPolyFT(packet, driver, false);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_t());
      } break;

      case 0x28:
      case 0x29:
      case 0x2A:
      case 0x2B: {          // Monochrome 4-pt poly
        PP driver = gpuPolySpanDrivers[
          Blending_Mode |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB
        ];
#ifdef UNAI_DRAWSTATS
        unai_drawCnt[10]++;
#endif
        UNAI_POLY_PROBE gpuDrawPolyF(packet, driver, true); // is_quad = true
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base());
      } break;

      case 0x2C:
      case 0x2D:
      case 0x2E:
      case 0x2F: {          // Textured 4-pt poly
        u32 dithering = Dithering;
        u32 simplified_count;
        gpuSetTexture(le32_to_u32(gpu_unai.PacketBuffer.U4[4]) >> 16);
        if (!dithering &&
            (simplified_count = prim_try_simplify_quad_t(gpu_unai.PacketBuffer.U4,
              gpu_unai.PacketBuffer.U4)))
        {
          for (i = 0;; ) {
            textured_sprite(cpu_cycles_sum, cpu_cycles);
            // Submit each simplified sprite separately with ITS own command code:
            // after the loop only the last one remains in the buffer.
            if (glrEnabled && glrPrim && glrEnabled())
              glr_send(le32_to_u32(gpu_unai.PacketBuffer.U4[0]) >> 24);
            glr_sent = 1;
            if (++i >= simplified_count)
              break;
            memcpy(&gpu_unai.PacketBuffer.U4[0], &gpu_unai.PacketBuffer.U4[i * 4], 16);
          }
          break;
        }
        gpuSetCLUT(le32_to_u32(gpu_unai.PacketBuffer.U4[2]) >> 16);

        u32 driver_idx =
          dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gpu_unai.PixelMSB;

        if (!FastLightingEnabled()) {
          driver_idx |= Lighting;
        } else {
          if (!((gpu_unai.PacketBuffer.U1[0]>0x5F) && (gpu_unai.PacketBuffer.U1[1]>0x5F) && (gpu_unai.PacketBuffer.U1[2]>0x5F)))
            driver_idx |= Lighting;
        }

        PP driver = gpuPolySpanDrivers[driver_idx];
#ifdef UNAI_DRAWSTATS
        unai_drawCnt[10]++;
#endif
        UNAI_POLY_PROBE gpuDrawPolyFT(packet, driver, true); // is_quad = true
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_t());
      } break;

      case 0x30:
      case 0x31:
      case 0x32:
      case 0x33: {          // Gouraud-shaded 3-pt poly
        u32 dithering = Dithering;
        //NOTE: The '129' here is CF_GOURAUD | CF_LIGHT, however
        // this is an untextured poly, so CF_LIGHT (texture blend)
        // shouldn't apply. Until the original array of template
        // instantiation ptrs is fixed, we're stuck with this. (TODO)
        u8 gouraud = 129;
        if (!dithering) {
          u32 xor_ = 0, rgb0 = le32_raw(gpu_unai.PacketBuffer.U4[0]);
          for (i = 1; i < 3; i++)
            xor_ |= rgb0 ^ le32_raw(gpu_unai.PacketBuffer.U4[i * 2]);
          if ((xor_ & HTOLE32(0xf8f8f8)) == 0)
            gouraud = 0;
        }
        PP driver = gpuPolySpanDrivers[
          dithering |
          Blending_Mode |
          gpu_unai.Masking | Blending | gouraud | gpu_unai.PixelMSB
        ];
        if (gouraud)
            {
#ifdef UNAI_DRAWSTATS
          unai_drawCnt[10]++;
#endif
          UNAI_POLY_PROBE gpuDrawPolyG(packet, driver, false); }
        else
            {
#ifdef UNAI_DRAWSTATS
          unai_drawCnt[10]++;
#endif
          UNAI_POLY_PROBE gpuDrawPolyF(packet, driver, false, POLYTYPE_G); }
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_g());
      } break;

      case 0x34:
      case 0x35:
      case 0x36:
      case 0x37: {          // Gouraud-shaded, textured 3-pt poly
        gpuSetCLUT    (le32_to_u32(gpu_unai.PacketBuffer.U4[2]) >> 16);
        gpuSetTexture (le32_to_u32(gpu_unai.PacketBuffer.U4[5]) >> 16);
        u32 dithering = Dithering;
        u8 lighting = Lighting;
        u8 gouraud = lighting ? (1<<7) : 0;
        if (lighting) {
          u32 xor_ = 0, rgb0 = le32_raw(gpu_unai.PacketBuffer.U4[0]);
          for (i = 1; i < 3; i++)
            xor_ |= rgb0 ^ le32_raw(gpu_unai.PacketBuffer.U4[i * 3]);
          if ((xor_ & HTOLE32(0xf8f8f8)) == 0) {
            gouraud = 0;
            if (!dithering && !need_lighting(rgb0))
              lighting = 0;
          }
        }
        PP driver = gpuPolySpanDrivers[
          dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gouraud | lighting | gpu_unai.PixelMSB
        ];
        if (gouraud)
            {
#ifdef UNAI_DRAWSTATS
          unai_drawCnt[10]++;
#endif
          UNAI_POLY_PROBE gpuDrawPolyGT(packet, driver, false); } // is_quad = true
        else
            {
#ifdef UNAI_DRAWSTATS
          unai_drawCnt[10]++;
#endif
          UNAI_POLY_PROBE gpuDrawPolyFT(packet, driver, false, POLYTYPE_GT); }
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_gt());
      } break;

      case 0x38:
      case 0x39:
      case 0x3A:
      case 0x3B: {          // Gouraud-shaded 4-pt poly
        u32 dithering = Dithering;
        // See notes regarding '129' for 0x30..0x33 further above -senquack
        u8 gouraud = 129;
        if (!dithering) {
          u32 xor_ = 0, rgb0 = le32_raw(gpu_unai.PacketBuffer.U4[0]);
          for (i = 1; i < 4; i++)
            xor_ |= rgb0 ^ le32_raw(gpu_unai.PacketBuffer.U4[i * 2]);
          if ((xor_ & HTOLE32(0xf8f8f8)) == 0)
            gouraud = 0;
        }
        PP driver = gpuPolySpanDrivers[
          dithering |
          Blending_Mode |
          gpu_unai.Masking | Blending | gouraud | gpu_unai.PixelMSB
        ];
        if (gouraud)
            {
#ifdef UNAI_DRAWSTATS
          unai_drawCnt[10]++;
#endif
          UNAI_POLY_PROBE gpuDrawPolyG(packet, driver, true); } // is_quad = true
        else
            {
#ifdef UNAI_DRAWSTATS
          unai_drawCnt[10]++;
#endif
          UNAI_POLY_PROBE gpuDrawPolyF(packet, driver, true, POLYTYPE_G); }
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_g());
      } break;

      case 0x3C:
      case 0x3D:
      case 0x3E:
      case 0x3F: {          // Gouraud-shaded, textured 4-pt poly
        u32 dithering = Dithering;
        u32 simplified_count;
        gpuSetTexture(le32_to_u32(gpu_unai.PacketBuffer.U4[5]) >> 16);
        if (!dithering &&
             (simplified_count = prim_try_simplify_quad_gt(gpu_unai.PacketBuffer.U4,
              gpu_unai.PacketBuffer.U4)))
        {
          for (i = 0;; ) {
            textured_sprite(cpu_cycles_sum, cpu_cycles);
            // Submit each simplified sprite separately with ITS own command code:
            // after the loop only the last one remains in the buffer.
            if (glrEnabled && glrPrim && glrEnabled())
              glr_send(le32_to_u32(gpu_unai.PacketBuffer.U4[0]) >> 24);
            glr_sent = 1;
            if (++i >= simplified_count)
              break;
            memcpy(&gpu_unai.PacketBuffer.U4[0], &gpu_unai.PacketBuffer.U4[i * 4], 16);
          }
          break;
        }
        gpuSetCLUT(le32_to_u32(gpu_unai.PacketBuffer.U4[2]) >> 16);
        u8 lighting = Lighting;
        u8 gouraud = lighting ? (1<<7) : 0;
        if (lighting) {
          u32 xor_ = 0, rgb0 = le32_raw(gpu_unai.PacketBuffer.U4[0]);
          for (i = 1; i < 4; i++)
            xor_ |= rgb0 ^ le32_raw(gpu_unai.PacketBuffer.U4[i * 3]);
          if ((xor_ & HTOLE32(0xf8f8f8)) == 0) {
            gouraud = 0;
            if (!dithering && !need_lighting(rgb0))
              lighting = 0;
          }
        }
        PP driver = gpuPolySpanDrivers[
          dithering |
          Blending_Mode | gpu_unai.TEXT_MODE |
          gpu_unai.Masking | Blending | gouraud | lighting | gpu_unai.PixelMSB
        ];
        if (gouraud)
            {
#ifdef UNAI_DRAWSTATS
          unai_drawCnt[10]++;
#endif
          UNAI_POLY_PROBE gpuDrawPolyGT(packet, driver, true); } // is_quad = true
        else
            {
#ifdef UNAI_DRAWSTATS
          unai_drawCnt[10]++;
#endif
          UNAI_POLY_PROBE gpuDrawPolyFT(packet, driver, true, POLYTYPE_GT); }
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_gt());
      } break;

      case 0x40:
      case 0x41:
      case 0x42:
      case 0x43: {          // Monochrome line
        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineF(packet, driver);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));
      } break;

      case 0x48 ... 0x4F: { // Monochrome line strip
        u32 num_vertexes = 1;
        le32_t *list_position = &list[2];

        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineF(packet, driver);

        while(1)
        {
          gpu_unai.PacketBuffer.U4[1] = gpu_unai.PacketBuffer.U4[2];
          gpu_unai.PacketBuffer.U4[2] = *list_position++;
          gpuDrawLineF(packet, driver);
          gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));

          num_vertexes++;
          if(list_position >= list_end) {
            cmd = -1;
            goto breakloop;
          }
          if((le32_raw(*list_position) & HTOLE32(0xf000f000)) == HTOLE32(0x50005000))
            break;
        }

        len += (num_vertexes - 2);
      } break;

      case 0x50:
      case 0x51:
      case 0x52:
      case 0x53: {          // Gouraud-shaded line
        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        // Index MSB selects Gouraud-shaded PixelSpanDriver:
        driver_idx |= (1 << 5);
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineG(packet, driver);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));
      } break;

      case 0x58 ... 0x5F: { // Gouraud-shaded line strip
        u32 num_vertexes = 1;
        le32_t *list_position = &list[2];

        // Shift index right by one, as untextured prims don't use lighting
        u32 driver_idx = (Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1;
        // Index MSB selects Gouraud-shaded PixelSpanDriver:
        driver_idx |= (1 << 5);
        PSD driver = gpuPixelSpanDrivers[driver_idx];
        gpuDrawLineG(packet, driver);

        while(1)
        {
          gpu_unai.PacketBuffer.U4[0] = gpu_unai.PacketBuffer.U4[2];
          gpu_unai.PacketBuffer.U4[1] = gpu_unai.PacketBuffer.U4[3];
          gpu_unai.PacketBuffer.U4[2] = *list_position++;
          gpu_unai.PacketBuffer.U4[3] = *list_position++;
          gpuDrawLineG(packet, driver);
          gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));

          num_vertexes++;
          if(list_position >= list_end) {
            cmd = -1;
            goto breakloop;
          }
          if((le32_raw(*list_position) & HTOLE32(0xf000f000)) == HTOLE32(0x50005000))
            break;
        }

        len += (num_vertexes - 2) * 2;
      } break;

      case 0x60:
      case 0x61:
      case 0x62:
      case 0x63: {          // Monochrome rectangle (variable size)
        PT driver = gpuTileDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        s32 w = 0, h = 0;
        gpuDrawT(packet, driver, &w, &h);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(w, h));
      } break;

      case 0x64:
      case 0x65:
      case 0x66:
      case 0x67:            // Textured rectangle (variable size)
        textured_sprite(cpu_cycles_sum, cpu_cycles);
        break;

      case 0x68:
      case 0x69:
      case 0x6A:
      case 0x6B: {          // Monochrome rectangle (1x1 dot)
        gpu_unai.PacketBuffer.U4[2] = u32_to_le32(0x00010001);
        PT driver = gpuTileDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        s32 w = 0, h = 0;
        gpuDrawT(packet, driver, &w, &h);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(1, 1));
      } break;

      case 0x70:
      case 0x71:
      case 0x72:
      case 0x73: {          // Monochrome rectangle (8x8)
        gpu_unai.PacketBuffer.U4[2] = u32_to_le32(0x00080008);
        PT driver = gpuTileDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        s32 w = 0, h = 0;
        gpuDrawT(packet, driver, &w, &h);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(w, h));
      } break;

      case 0x74:
      case 0x75:
      case 0x76:
      case 0x77: {          // Textured rectangle (8x8)
        gpu_unai.PacketBuffer.U4[3] = u32_to_le32(0x00080008);
        textured_sprite(cpu_cycles_sum, cpu_cycles);
      } break;

      case 0x78:
      case 0x79:
      case 0x7A:
      case 0x7B: {          // Monochrome rectangle (16x16)
        gpu_unai.PacketBuffer.U4[2] = u32_to_le32(0x00100010);
        PT driver = gpuTileDrivers[(Blending_Mode | gpu_unai.Masking | Blending | (gpu_unai.PixelMSB>>3)) >> 1];
        s32 w = 0, h = 0;
        gpuDrawT(packet, driver, &w, &h);
        gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(w, h));
      } break;

      case 0x7C:
      case 0x7D:
      case 0x7E:
      case 0x7F: {          // Textured rectangle (16x16)
        gpu_unai.PacketBuffer.U4[3] = u32_to_le32(0x00100010);
        textured_sprite(cpu_cycles_sum, cpu_cycles);
      } break;

#ifdef TEST
      case 0x80:          //  vid -> vid
        gpuMoveImage(packet);
        break;

      case 0xA0:          //  sys -> vid
      {
        u32 load_width = list[2] & 0xffff;
        u32 load_height = list[2] >> 16;
        u32 load_size = load_width * load_height;

        len += load_size / 2;
      } break;

      case 0xC0:
        break;
#else
      case 0x1F:                   //  irq?
      case 0x80 ... 0x9F:          //  vid -> vid
      case 0xA0 ... 0xBF:          //  sys -> vid
      case 0xC0 ... 0xDF:          //  vid -> sys
        // Handled by gpulib
        goto breakloop;
#endif
      case 0xE1 ... 0xE6: { // Draw settings
        u32 cmd_word = le32_to_u32(gpu_unai.PacketBuffer.U4[0]);
        ex_regs[(cmd_word >> 24) & 7] = cmd_word;
        gpuGP0Cmd_0xEx(gpu_unai, cmd_word);
      } break;
    }
    // The same primitive to the MBX rasterizer. Handled here rather than in each case:
    // page and CLUT are set by packet handlers, and at this point all state
    // is already current. Field order mirrors enum TGlrState in glrender.cpp.
    if (!glr_sent && glrEnabled && glrPrim && glrEnabled())
      glr_send(cmd);
#ifdef GPU_UNAI_PRIMSTATS
    // The rasterizer no longer sees the command code, so the area accumulated in
    // gpu_unai_primstats_cur is classified here. Lines are not accounted by
    // area - they have a dozen driver call sites and a negligible share;
    // we only count primitive instances.
    {
      u32 px = gpu_unai_primstats_cur;
      gpu_unai_primstats_cur = 0;
      if (cmd >= 0x20 && cmd <= 0x3f) {
        // The class is determined by only two code bits: 0x04 - textured,
        // 0x10 - Gouraud. Bit 0x08 (quad vs triangle) does not affect class.
        switch (cmd & 0x14) {
        case 0x00: gpu_unai_primstats.pix_poly_f  += px;
                   gpu_unai_primstats.n_poly_f++;  break;
        case 0x04: gpu_unai_primstats.pix_poly_ft += px;
                   gpu_unai_primstats.n_poly_ft++; break;
        case 0x10: gpu_unai_primstats.pix_poly_g  += px;
                   gpu_unai_primstats.n_poly_g++;  break;
        default:   gpu_unai_primstats.pix_poly_gt += px;
                   gpu_unai_primstats.n_poly_gt++; break;
        }
        u32 b = 0;
        if      (px >= 4096) b = 5;
        else if (px >= 1024) b = 4;
        else if (px >=  256) b = 3;
        else if (px >=   64) b = 2;
        else if (px >=   16) b = 1;
        gpu_unai_primstats.area_bucket[b]++;
      }
      else if (cmd >= 0x60 && cmd <= 0x7f) {
        gpu_unai_primstats.pix_sprite += px;
        gpu_unai_primstats.n_sprite++;
      }
      else if (cmd >= 0x40 && cmd <= 0x5f)
        gpu_unai_primstats.n_line++;
      else if (cmd == 0x02) {
        gpu_unai_primstats.pix_fill += (u32)(le16_to_s16(packet.U2[4]) & 0x3ff)
                                     * (u32)(le16_to_s16(packet.U2[5]) & 0x1ff);
        gpu_unai_primstats.n_fill++;
      }
      // Semi-transparency mode is counted only for primitives that have it enabled
      // (bit 1 of command code) - the rest in GL do not need blend at all.
      if ((cmd & 2) && cmd >= 0x20 && cmd <= 0x7f)
        gpu_unai_primstats.blend_mode[(gpu_unai.BLEND_MODE >> 3) & 3]++;
      // Texture state. Read right here, after the packet handler: page and
      // CLUT are set by gpuSetTexture/gpuSetCLUT inside it.
      // Lines are excluded from range intentionally: in them bit 2 means nothing rather
      // than texture, and counting them as textured would be an error.
      if ((cmd & 4) && ((cmd >= 0x20 && cmd <= 0x3f) || (cmd >= 0x60 && cmd <= 0x7f))) {
        u32 tba = (u32)(gpu_unai.inn.TBA - gpu_unai.vram);
        u32 tp  = (tba * 0x9E3779B1u) ^ ((u32)gpu_unai.TEXT_MODE * 0xC2B2AE35u);
        u32 tk  = tp;
        // In 16-bit mode CLUT is not read, and its current value does not belong
        // to the texture - otherwise the counter would inflate for no reason.
        if (gpu_unai.TEXT_MODE != (3 << 5)) {
          u32 cba = (u32)(gpu_unai.inn.CBA - gpu_unai.vram) >> 4;
          tk ^= cba * 0x85EBCA6Bu;
        }
        gpu_unai_primstats.n_textured++;
        if (tk != ps_last_tex) { gpu_unai_primstats.n_texchange++; ps_last_tex = tk; }
        if (tp != ps_last_tp)  { gpu_unai_primstats.n_tpchange++;  ps_last_tp = tp; }
        if (!primstat_seen(ps_tex_key, ps_tex_gen, tk)) ps_cnt_tex++;
        if (!primstat_seen(ps_tp_key, ps_tp_gen, tp))   ps_cnt_tp++;
      }
    }
#endif
  }

breakloop:
  ex_regs[1] &= ~0x1ff;
  ex_regs[1] |= gpu_unai.GPU_GP1 & 0x1ff;

  *cycles_sum_out += cpu_cycles_sum;
  *cycles_last = cpu_cycles;
  *last_cmd = cmd;
  return list - list_start;
}

void renderer_sync_ecmds(u32 *ecmds_)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  // the funcs below expect LE
  uint32_t i, ecmds[8];
  for (i = 1; i <= 6; i++)
    ecmds[i] = HTOLE32(ecmds_[i]);
#else
  uint32_t *ecmds = ecmds_;
#endif
  if (!IS_OLD_RENDERER()) {
    int dummy;
    renderer_do_cmd_list(&ecmds[1], 6, ecmds, &dummy, &dummy, &dummy);
  }
  else
    oldunai_renderer_sync_ecmds(ecmds);
}

// Upstream hook: the game overwrote a chunk of VRAM. gpu_unai has no caches, so
// it is empty here, but the MBX rasterizer keeps unpacked pages in GPU memory,
// and they may become stale. We invalidate only those touched by the write:
// flushing the whole cache cost 4.5 ms per frame because after each write
// the entire working set had to be unpacked anew.
void renderer_update_caches(int x, int y, int w, int h, int state_changed)
{
  if (glrInvalidateRect)
    glrInvalidateRect(x, y, w, h);
}

void renderer_flush_queues(void)
{
}

void renderer_set_interlace(int enable, int is_odd)
{
  renderer_notify_screen_change(&gpu.screen);
}

#include "../../frontend/plugin_lib.h"
// Handle any gpulib settings applicable to gpu_unai:
void renderer_set_config(const struct rearmed_cbs *cbs)
{
  gpu_unai.vram = (le16_t *)gpu.vram;
  // The MBX rasterizer decodes texture pages itself, so it needs the same
  // VRAM. Pass it once - it does not relocate during the process.
  if (glrSetVram)
    glrSetVram(gpu.vram);
  gpu_unai.config.old_renderer  = cbs->gpu_unai.old_renderer;
  gpu_unai.config.ilace_force   = cbs->gpu_unai.ilace_force;
  gpu_unai.config.lighting      = cbs->gpu_unai.lighting;
  gpu_unai.config.fast_lighting = cbs->gpu_unai.fast_lighting;
  gpu_unai.config.blending      = cbs->gpu_unai.blending;
  gpu_unai.config.dithering     = cbs->dithering != 0;
  gpu_unai.config.force_dithering = cbs->dithering >> 1;

  renderer_notify_screen_change(&gpu.screen);
  oldunai_renderer_set_config(cbs);
}

// vim:shiftwidth=2:expandtab

// Draw-shape counters for the host shell: primitives and pixels by kind.
// Zero unless the library was built with UNAI_DRAWSTATS.
extern unsigned int unai_drawCnt[12];
extern int unai_spanOff;
extern int unai_texFlat;
#ifndef PS1_NO_DIAG
extern int unai_dstFlat;
#endif

extern "C" void gpuUnaiDrawStats(unsigned int *out, int n)
{
  int i;
  for (i = 0; i < n && i < 12; i++)
    out[i] = unai_drawCnt[i];
}

extern "C" void gpuUnaiSetSpanOff(int on)
{
  unai_spanOff = on;
}

extern "C" void gpuUnaiSetTexFlat(int on)
{
  unai_texFlat = on;
}

extern "C" void gpuUnaiSetDstFlat(int on)
{
#ifndef PS1_NO_DIAG
  unai_dstFlat = on;
#else
  (void)on;
#endif
}

// Full CF/length histogram is opt-in: the normal bench and release keep no
// per-span counter writes. Direct C++ table entries and ASM wrappers both feed it.
extern "C" int gpuUnaiCfEnabled(void)
{
#ifdef UNAI_CF_PROFILE
  return 1;
#else
  return 0;
#endif
}

extern "C" void gpuUnaiCfReset(void)
{
#ifdef UNAI_CF_PROFILE
  memset(unai_cf_stats, 0, sizeof(unai_cf_stats));
#endif
}

extern "C" int gpuUnaiCfGet(unsigned cf, int route, unsigned *out)
{
#ifdef UNAI_CF_PROFILE
  if (!out || cf >= 1024 || route < 0 || route > 1) return 0;
  const UnaiCfStat &s = unai_cf_stats[route][cf];
  out[0] = s.calls; out[1] = s.pixels;
  for (int i = 0; i < 5; i++) out[i + 2] = s.lengths[i];
  return 1;
#else
  (void)cf; (void)route; (void)out;
  return 0;
#endif
}
