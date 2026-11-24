// local test: compile one kernel of an AIR bitcode file, write the machine code
#include "../../src/compiler/xe_compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: xec_test in.bc function out.bin\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    std::vector<char> b; char t[4096]; size_t n;
    while ((n = fread(t, 1, sizeof t, f)) > 0) b.insert(b.end(), t, t + n);
    fclose(f);
    static uint32_t out[65536]; uint32_t dw = 0; char log[512];
    uint32_t info = 0; uint64_t smp[4];
    int r = irisxe_compile_air(b.data(), b.size(), argv[2], out, 65536, &dw, &info, smp, log, sizeof log);
    printf("%s: %d %s\n", argv[2], r, log);
    if (r) return 1;
    f = fopen(argv[3], "wb"); fwrite(out, 4, dw, f); fclose(f);
    return 0;
}
