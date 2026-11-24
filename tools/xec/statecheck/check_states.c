#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#define UTIL_ARCH_LITTLE_ENDIAN 1
#define UTIL_ARCH_BIG_ENDIAN 0
#define __gen_address_type uint64_t
#define __gen_user_data void
static inline uint64_t __gen_combine_address(void *d, void *loc, uint64_t a, uint32_t delta) { return a + delta; }
#define __gen_combine_address __gen_combine_address
#include "gen120_pack.h"
typedef uint32_t UInt32; typedef uint64_t UInt64; typedef uint8_t UInt8;
#include "abi_part.h"
#include "pack_part.h"
int main(void) {
  int bad = 0;
  XeTexDesc ts[3] = { {0x123000, 64, 32, 1, 256, 32, 199, 0, 0}, {0x400000, 100, 7, 5, 448, 8, 132, 1, 0}, {0x9000, 1, 1, 1, 64, 4, 216, 0, 0} };
  for (int k = 0; k < 3; k++) {
    XeTexDesc t = ts[k];
    struct GFX12_RENDER_SURFACE_STATE s = { 0 };
    s.SurfaceType = SURFTYPE_2D; s.SurfaceArray = t.array; s.SurfaceFormat = t.format;
    s.SurfaceVerticalAlignment = VALIGN_4; s.SurfaceHorizontalAlignment = HALIGN_4; s.TileMode = LINEAR;
    s.MOCS = 4; s.SurfaceQPitch = t.array ? t.qpitch >> 2 : 0;
    s.Width = t.width - 1; s.Height = t.height - 1; s.SurfacePitch = t.pitch - 1; s.Depth = t.layers - 1;
    s.RenderTargetViewExtent = t.layers - 1;
    s.ShaderChannelSelectRed = SCS_RED; s.ShaderChannelSelectGreen = SCS_GREEN; s.ShaderChannelSelectBlue = SCS_BLUE; s.ShaderChannelSelectAlpha = SCS_ALPHA;
    s.SurfaceBaseAddress = t.va;
    uint32_t ref[16] = {0}, ours[16] = {0};
    GFX12_RENDER_SURFACE_STATE_pack(NULL, ref, &s);
    packSurfaceState(ours, t);
    for (int i = 0; i < 16; i++) if (ref[i] != ours[i]) { printf("surface %d DW%d: mesa %08x ours %08x\n", k, i, ref[i], ours[i]); bad++; }
  }
  XeSampDesc ss[2] = { {1, 1, 1, 0, 2, 2, 2, 1}, {1, 0, 0, 0, 0, 1, 4, 0} };
  for (int k = 0; k < 2; k++) {
    XeSampDesc d = ss[k];
    struct GFX12_SAMPLER_STATE s = { 0 };
    s.MinModeFilter = d.minFilter; s.MagModeFilter = d.magFilter; s.MipModeFilter = d.mipFilter;
    s.LODPreClampMode = CLAMP_MODE_OGL; s.MaxLOD = 14.0; s.MinLOD = 0;
    s.BorderColorPointer = 0x20002600;
    s.TCXAddressControlMode = d.addrU; s.TCYAddressControlMode = d.addrV; s.TCZAddressControlMode = d.addrW;
    s.NonnormalizedCoordinateEnable = !d.normalized;
    bool r = d.minFilter || d.magFilter;
    s.RAddressMinFilterRoundingEnable = r; s.RAddressMagFilterRoundingEnable = r;
    s.VAddressMinFilterRoundingEnable = r; s.VAddressMagFilterRoundingEnable = r;
    s.UAddressMinFilterRoundingEnable = r; s.UAddressMagFilterRoundingEnable = r;
    uint32_t ref[4] = {0}, ours[4] = {0};
    GFX12_SAMPLER_STATE_pack(NULL, ref, &s);
    packSamplerState(ours, d, 0x20002600);
    for (int i = 0; i < 4; i++) if (ref[i] != ours[i]) { printf("sampler %d DW%d: mesa %08x ours %08x\n", k, i, ref[i], ours[i]); bad++; }
  }
  printf("%s\n", bad ? "MISMATCH" : "surface + sampler states match Mesa's packers");
  return bad != 0;
}
