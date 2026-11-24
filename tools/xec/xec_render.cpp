// local test (milestone 5b): compile one vertex or fragment function of an AIR
// bitcode file, write the machine code and print the shader info
#include "../../src/compiler/xe_compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: xec_render in.bc function out.bin\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    std::vector<char> b; char t[4096]; size_t n;
    while ((n = fread(t, 1, sizeof t, f)) > 0) b.insert(b.end(), t, t + n);
    fclose(f);
    uint32_t maxDW = getenv("XEC_MAXDW") ? (uint32_t)atoi(getenv("XEC_MAXDW")) : 1048576;
    std::vector<uint32_t> outv(maxDW); uint32_t *out = outv.data(); uint32_t dw = 0; char log[512];
    XeShaderInfo si;
    int r = irisxe_compile_air_render(b.data(), b.size(), argv[2], out, maxDW, &dw, &si, log, sizeof log);
    printf("%s: %d %s\n", argv[2], r, log);
    if (r) return 1;
    printf("  stage %u grfStart %u push %u flags 0x%x textures %u\n", si.stage, si.grfStart, si.pushRegs, si.flags, si.usesTextures);
    if (si.stage == XE_STAGE_VERTEX) {
        printf("  inputs %u (read length %u), sys 0x%x in slot %u, VUE slots %u\n", si.numInputs, si.urbReadLength, si.sysValues, si.sysSlot, si.vueSlots);
        for (unsigned i = 0; i < si.numInputs; i++) printf("    slot %u: location %d, %u components\n", i, si.inputLocation[i], si.inputComps[i]);
    } else printf("  render targets 0x%x (type of 0: %u)\n", si.rtMask, si.rtType[0]);
    for (unsigned i = 0; i < si.numVaryings; i++)
        printf("    varying %u: %s, %u components, interp %u\n", i, si.varyingName[i], si.varyingComps[i], si.varyingInterp[i]);
    f = fopen(argv[3], "wb"); fwrite(out, 4, dw, f); fclose(f);
    return 0;
}
