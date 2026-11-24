#!/bin/bash
# Round 10 (milestone 3b): how Metal compiles a compute function for a GPU
# driver - which driver methods Metal calls (function key, compiler output ->
# variant -> pipeline), where the vendor compiler backend lives, and how
# Apple's ICL driver encodes a compute dispatch (API usage only).
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server > "$OUT/40-build.txt" 2>&1
xcrun clang -fobjc-arc -O1 -framework Foundation -framework Metal pipeline_probe.m -o pipeline_probe >> "$OUT/40-build.txt" 2>&1
ICL=/System/Library/Extensions/AppleIntelICLGraphicsMTLDriver.bundle/Contents/MacOS/AppleIntelICLGraphicsMTLDriver

# 1. where compilers live (file names only)
{
  echo "== MTLCompilerService"; find /System/Library/Frameworks/Metal.framework -maxdepth 4 -iname "*Compiler*" 2>/dev/null
  echo "== GPUCompiler"; find /System/Library/PrivateFrameworks/GPUCompiler.framework -maxdepth 5 2>/dev/null | head -100
  echo "== MTLCompiler"; find /System/Library/PrivateFrameworks/MTLCompiler.framework -maxdepth 5 2>/dev/null | head -50
  echo "== *Intel* / *IG* compiler-looking files under /System/Library"
  find /System/Library -maxdepth 6 \( -iname "*igc*" -o -iname "*IntelCompiler*" -o -iname "*GPUCompilerImpl*" -o -iname "*MTLCompilerPlugin*" \) 2>/dev/null | head -50
  echo "== Extensions *MTL*"; ls -d /System/Library/Extensions/*MTL* 2>/dev/null
} > "$OUT/70-compiler-files.txt" 2>&1

# 2. Metal's own classes + a function/pipeline made on this runner
./pipeline_probe > "$OUT/71-metal-pipeline-classes.txt" 2>&1

# 3. strings of the compiler service that look like plugin paths / names
for b in /System/Library/Frameworks/Metal.framework/Versions/A/XPCServices/MTLCompilerService.xpc/Contents/MacOS/MTLCompilerService; do
  [ -f "$b" ] && strings -a "$b" | grep -iE "plugin|backend|\.dylib|\.bundle|intel|deviceType|GPUCompiler|AIR" | sort -u | head -300
done > "$OUT/72-compilerservice-strings.txt" 2>&1

# 4. the driver side of compiling (Apple ICL driver, API usage only)
python3 udis.py "$OUT/73-icl-compile.txt" "load:$ICL" \
  'm:MTLIGAccelDevice:computeFunctionKeyWithComputePipelineDescriptor:options:keySize:' \
  'm:MTLIGAccelDevice:computeVariantWithCompilerOutput:pipelineStatisticsOutput:' \
  'm:MTLIGAccelDevice:newComputePipelineWithDescriptor:variant:' \
  'm:MTLIGAccelDevice:compiler' \
  'm:MTLIGAccelDevice:targetDeviceInfo' \
  'm:MTLIGAccelDevice:isVendorSliceCompatibleWithDeploymentTarget:platform:sdkVersion:compilerPluginVersion:' \
  'ivars:MTLIGComputePipelineState' 'ivars:MTLIGFunctionVariant' 'ivars:MTLIOAccelComputeCommandEncoder' \
  --follow 'Variant|FunctionKey|Compiler|ComputePipeline|KernelBinary|Decode|Chal' --depth 1 > "$OUT/73-log.txt" 2>&1
# 5. Metal's generic path from newComputePipelineStateWithFunction to the driver
python3 udis.py "$OUT/74-metal-compute-pipeline.txt" \
  'm:_MTLDevice:newComputePipelineStateWithFunction:error:' \
  'm:_MTLDevice:newComputePipelineStateWithDescriptor:options:reflection:error:' \
  'm:MTLIOAccelDevice:newComputePipelineStateWithDescriptor:options:reflection:error:' \
  --follow 'ComputePipeline|Compiler|Variant|FunctionKey|compute' --depth 2 > "$OUT/74-log.txt" 2>&1
# 6. how the ICL driver encodes a dispatch (API usage only)
python3 udis.py "$OUT/75-icl-dispatch.txt" "load:$ICL" \
  'm:MTLIGAccelCommandBuffer:computeCommandEncoder' \
  'm:MTLIGAccelComputeCommandEncoder:setComputePipelineState:' \
  'm:MTLIGAccelComputeCommandEncoder:dispatchThreadgroups:threadsPerThreadgroup:' \
  'm:MTLIGAccelComputeCommandEncoder:endEncoding' \
  --follow 'IGAccelCompute|Dispatch|Walker|Segment|KernelCommand' --depth 1 > "$OUT/75-log.txt" 2>&1
ls -la "$OUT"
exit 0
