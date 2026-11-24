# xec - tests for our shader compiler (src/compiler)

The IrisXe Metal driver compiles Metal compute kernels itself (milestone 3c):
Metal's AIR (LLVM bitcode, from `-[MTLFunction bitcodeData]`) is read with
LLVM's bitcode reader (LLVM-C API, linked in statically) and turned into
Iris Xe (Gen12) SIMD8 machine code by `src/compiler/xe_compile.cpp`; the
instruction encoder is `src/compiler/xe_enc.h`.

- `test_encoder.py` + `enc_cases.cpp`: every instruction form the encoder
  makes must match Mesa's `brw_asm -g tgl` bit for bit (Mesa: MIT licence;
  only used as a reference, nothing of it is linked in).
- `xec_test.cpp`: compile one kernel of a bitcode file on any machine with
  LLVM (e.g. `g++ -std=c++17 -I$(llvm-config --includedir) src/compiler/xe_compile.cpp
  tools/xec/xec_test.cpp $(llvm-config --ldflags --libs bitreader core)`), then check
  the output with `brw_disasm --gen=tgl`.

- `xesim.py` + `sim_tests.py` (milestone 3d): a small SIMD8 simulator for the
  code we emit (ALU, data-port reads/writes, IF/ELSE/ENDIF/BREAK/WHILE with
  per-lane masks, the walker's right mask); runs compiled kernels against
  Python references.

Milestone 3e adds the math unit (sqrt, rsqrt, sin, cos, exp/exp2, log/log2,
pow, division via inverse), integer division/remainder, floor/ceil/trunc/rint/
round/fract, popcount/clz/ctz/reverse_bits, vectors (2-4 components; loads and
stores of 32-bit vectors in one message), 8/16-bit integers and half (byte
scattered messages), dot/length/distance/normalize/cross, uint2/uint3 positions.

Milestone 3d adds if/else (nested), loops (nested, data-dependent, with
break) through the GPU's structured control flow, and phi nodes. Still not:
switch, early return, continue, break inside an if.

First version: straight-line kernels (one basic block) - integer and float
arithmetic, compares, select, conversions, getelementptr, 32-bit loads and
stores from device/constant buffers 0..5, the thread position built-ins.
Not yet: control flow (if/loops), threadgroup memory, textures, vectors,
8/16/64-bit types, division.

`sim_apple.py <dir>` runs the Apple-compiled 3e kernels (math_f, div_mod,
bit_ops from irisxe-kernels.ll, compiled with xec_test) in the simulator with
the same inputs and checks as `irisxe-metaltest --types`. The simulator refuses
math POW: Gfx12 has no POW and it hangs the GPU, so pow is compiled as
exp2(y * log2(x)).

## Milestone 3f (v0.28.0)

`sim_3f.py <dir>` runs tg_reverse, tg_sum, atomic_hist and tg_count (compiled
with xec_test) in the simulator. The simulator now runs the hardware threads
of a thread group together: each runs to its next `sync bar`, and a thread
that ends while others wait at a barrier is reported as a GPU hang (that is
what happens on the real GPU). Threadgroup memory (surface 254) is separate
for every group; untyped atomics and memory fences are simulated; an IF or
ELSE with no lanes left jumps by its JIP like the hardware does.
The last check runs tg_count without IrisXeCore's split of a partly filled
last group and expects the hang.

## Register reuse and big kernels (v0.29.0)

The compiler now gives registers back after a value's last use (values made
before a loop and used in it are kept until the loop ends; loop-header phis
too), so kernels are limited by values live at the same time (about 114),
not by their length. Kernels bigger than 3968 bytes go in their own GPU
buffer (IrisXeCore 0.29+, up to 256 KB).

- `gen_stress.py out.ll` writes three generated stress kernels (hundreds of
  values, values live across loops, loop phis, ifs in loops) and a Python
  reference (`out_ref.py`); `sim_stress.py <dir>` checks them. Breaking the
  loop rule on purpose makes this test fail (checked once by hand).
- `sim_big.py <dir>` checks big_mix and wide_live (tools/metaltest/kernels.metal),
  compiled from clang -O2 IR of the same code.

## switch, private arrays (v0.29.1)

- `switch` becomes a chain of if/else (one branch per distinct target; cases
  with the same target are or'ed), done on the LLVM IR before compiling.
- Private arrays (`alloca`, e.g. `uint a[64]` indexed at run time) live in a
  scratch buffer that the Metal driver binds at buffer slot 5: each lane has
  its own area at slot5 + (hardware thread id * 8 + lane) * size, the
  hardware thread id packed from the sr0 state register (Gfx12 layout as in
  Mesa's load_topology_id). Up to 1008 bytes per thread.
- `c_vs_sim.sh file.c function` compiles a C kernel natively (the reference)
  and through our compiler (clang -O2, loops kept), then compares in the
  simulator. The simulator now also jumps over an IF/ELSE when no lane takes
  a side, like the hardware (that case is common when a condition is the
  same in every lane).
- v0.29.2: LLVM 22 stopped storing switch case values as operands (the build
  uses Homebrew's LLVM); the compiler reads them with LLVMGetSwitchCaseValue
  there, and with the old operand layout on older LLVM.

## Milestone 4: textures (v0.30.0)

- `sim_tex.py <dir>` runs the Apple-compiled tex_* kernels (irisxe-kernels.ll)
  in the simulator, which now models textures (`xesim.Tex`), sampler
  messages (ld_lz, sample_lz with nearest / linear filtering, resinfo) and
  typed surface reads / writes, plus 2D grids (`groups_y`).
- `statecheck/run.sh` checks IrisXeCore's RENDER_SURFACE_STATE and
  SAMPLER_STATE packing against Mesa's generated genxml packers.
- How it works: textures are linear buffers; IrisXeCore writes a binding table
  (entry k = texture slot k), surface states and sampler states into its state
  page (GPU VA 0x20002000, also the surface state base). Shaders read with the
  sampler's ld_lz, sample with sample_lz (compute has no derivatives: level 0),
  ask the size with resinfo, and write (or read read_write textures) with
  typed surface messages, as Mesa does on Gfx12.

## Milestone 5b: vertex and fragment functions

- `xec_render.cpp`: like `xec_test.cpp`, for `irisxe_compile_air_render`
  (vertex / fragment functions); prints the `XeShaderInfo` the pipeline needs
  (GRF start, inputs, varyings by AIR name, interpolation, render targets).
- Apple's render AIR (irisxe-kernels.ll, typed pointers) converts to LLVM 18
  opaque pointers with a few regex passes (see the v0.31.2 notes); all render
  shaders of kernels.metal compile except rf_mrt (depth output comes in 5c).
- Compute kernels compile bit-identical to v0.31.1 (39 kernels compared).
