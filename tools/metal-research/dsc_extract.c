// dsc_extract: extract selected dylibs from the dyld shared cache using
// Xcode's dsc_extractor.bundle (same tool Apple ships for symbolication).
#include <stdio.h>
#include <dlfcn.h>
typedef int (*extract_fn)(const char *cache, const char *dst, void (^progress)(unsigned, unsigned));
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: dsc_extract <dsc_extractor.bundle> <cache> <outdir>\n"); return 1; }
    void *h = dlopen(argv[1], RTLD_LAZY);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    extract_fn f = (extract_fn)dlsym(h, "dyld_shared_cache_extract_dylibs_progress");
    if (!f) { fprintf(stderr, "no extractor symbol\n"); return 1; }
    int r = f(argv[2], argv[3], ^(unsigned c, unsigned t) { if (c % 200 == 0) fprintf(stderr, "%u/%u\n", c, t); });
    printf("extract result %d\n", r);
    return r;
}
