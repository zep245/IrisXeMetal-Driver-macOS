# shadergen

How `src/IrisXeCompute.inc` was made (Mesa 25.2.8 source, MIT licence):

1. Build Mesa's Intel tools: `meson setup b -Dgallium-drivers= -Dvulkan-drivers= -Dtools=intel -Dllvm=disabled ...`
   then `ninja src/intel/compiler/brw_asm src/intel/compiler/brw_disasm src/intel/genxml/gen120_pack.h`
2. Shader: `brw_asm -g tgl -t bin -o test.bin test.asm` (check with `brw_disasm --gen=tgl --input-path=test.bin`)
3. Commands: compile `gen.c` against `b/src/intel/genxml` (`-DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0`) and run it.

The shader writes `0xC0DE0000 + lane` for 8 lanes to GPU address 0x20001000, then ends the thread.

## Milestone 3a: `add1.asm` (compute kernel used by `irisxe-metaltest --compute`)

SIMD8 kernel for our dispatch command: `out[i] = in[i] + 1` for `i < gridX`.
On entry g1 = { buffer 0, buffer 1, .., gridX (g1.6), threads per group (g1.7) },
g2.0 = first lane of this thread inside its thread group, g0.1 = thread group X.
`brw_asm -g tgl -t bin -o add1.bin add1.asm`, then turned into `tools/metaltest/xe_kernels.h`.

## Milestone 5a: `render.c` + `render_ps.asm` (first 3D draw, `src/IrisXeRender.inc`)

The 3D command sequence follows Mesa blorp (`blorp_genX_exec_brw.h`) and
`iris_init_render_context`, with TGL GT2 values from Mesa devinfo (8086:9a49):
L3 all 88 / URB 32 ways, VS URB 2 x 64 bytes, 3576 entries, start 4, 63 PS threads per PSD.
The vertex shader is off (the vertex fetcher writes the VUE), clipping and the
viewport transform are off, and `render_ps.asm` writes one colour with a SIMD8
render target write (headerless, descriptor 0x08031400, EOT).

1. `brw_asm -g tgl -t bin -o X.bin X.asm` for `render_ps.asm`, `render_vs.asm`, `render_ps2.asm`
2. compile `render.c` like `gen.c` (also `-I<mesa>/src/intel/genxml -I<mesa>/src/intel`), then
   `./render render_ps.bin render_vs.bin render_ps2.bin > ../../src/IrisXeRender.inc`

v0.31.1 (milestone 5b) adds draw B: the real pipeline. `render_vs.asm` (SIMD8: g1 =
URB handles, inputs from g2) writes the VUE with two URB SIMD8 writes (header +
position, then the colour; the last with EOT from g112+). Clipper, perspective
divide and viewport transform are on (D3D depth range, y flipped like Metal).
`render_ps2.asm` interpolates the colour: perspective barycentrics in g2-g3, plane
parameters from g4, 16 bytes per component (a1-a0, a2-a0, -, a0), as Mesa
`nir_lower_interpolation` + `remap_attr_reg` use them on Gfx11/12 (no PLN).
`render.c` also writes `kRender2Expect`, the CPU reference image for draw B.

Mesa's batch decoder (`libintel_decoder.a`) was used to check the packed commands.

## Milestone 5c: Metal draws (`kDrawCmds`, `kClearCmds`)

`render.c` also writes the templates IrisXeCore builds every Metal draw from
(`IrisXeGPU.cpp` `mtlRunDraw3D`): `kDrawCmds` (the full 3D pipeline, vertex
shader on, push constants with absolute addresses via CS_DEBUG_MODE2 like iris),
with `RD_*` = { dword, shift } of every field the kext fills in (they are 0 in
the template), the field positions inside VERTEX_BUFFER_STATE / VERTEX_ELEMENT_STATE /
3DSTATE_VF_INSTANCING (packets the kext builds), and `kClearCmds` (blorp's
RECTLIST with `render_clear.asm`, colour from push constants) for render pass clears.
Input files: `./render render_ps.bin render_vs.bin render_ps2.bin render_clear.bin`.

## v0.33.0: blending, scissor, indexed draws
The Metal draw template now has 3DSTATE_SCISSOR_STATE_POINTERS (SCISSOR_RECT at
`DYN_SCISSOR`, scissor always on), 3DSTATE_INDEX_BUFFER before 3DPRIMITIVE, and
patch positions for 3DSTATE_PS_BLEND (`RD_PSB_*`), 3DSTATE_VF cut index
(`RD_VF_CUT*`), the index buffer (`RD_IB_*`) and 3DPRIMITIVE random access / base
vertex. `BLE_*`, `BLS_*`, `SCR_*`, `CCS_*` are field positions inside BLEND_STATE,
SCISSOR_RECT and COLOR_CALC_STATE; `kGenBlendFactor` / `kGenBlendOp` map Metal's
enums to Gen's. Clears use their own blend state (`DYN_BLENDCLR`), so a pipeline's
blending or write mask never touches them.

## v0.34.0: depth and stencil
3DSTATE_DEPTH_BUFFER, 3DSTATE_STENCIL_BUFFER and 3DSTATE_WM_DEPTH_STENCIL are
patched in both the draw (`RD_DB_*`, `RD_SB_*`, `RD_DS_*`) and the clear
(`RC_*`) templates; their surface type is 0 in the template, so IrisXeCore always
writes NULL or 2D. 3DSTATE_RASTER's z clip tests and depth bias, and
3DSTATE_PS_EXTRA's computed depth, are patched too. The clear rectangle is now
R32G32B32_FLOAT: a depth / stencil clear is the clear template drawn with z = the
clear depth, depth test ALWAYS + write and stencil ALWAYS / REPLACE, into a null
render target (`SRF_BTNULL` -> `SRF_SSNULL`). Depth and stencil planes are Y-tiled
(Gen12 has no W-tiled stencil; v0.34.1). The bundle lays them out and untiles them for getBytes.
`kGenCompare` / `kGenStencilOp` map Metal's enums.
