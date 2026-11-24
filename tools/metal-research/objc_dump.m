// objc_dump: load a framework/bundle and list its Objective-C classes,
// superclasses, protocols, instance variables and methods (with type
// encodings) using the Objective-C runtime. Interoperability research:
// how Metal finds and talks to a GPU driver bundle.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>

static void dumpProtocol(Protocol *p) {
    printf("@protocol %s\n", protocol_getName(p));
    unsigned n = 0;
    Protocol * __unsafe_unretained *inh = protocol_copyProtocolList(p, &n);
    for (unsigned i = 0; i < n; i++) printf("  <%s>\n", protocol_getName(inh[i]));
    free(inh);
    for (int req = 1; req >= 0; req--)
        for (int inst = 1; inst >= 0; inst--) {
            unsigned m = 0;
            struct objc_method_description *d = protocol_copyMethodDescriptionList(p, req, inst, &m);
            for (unsigned i = 0; i < m; i++)
                printf("  %s%c %s  %s\n", req ? "" : "@optional ", inst ? '-' : '+',
                       sel_getName(d[i].name), d[i].types ? d[i].types : "");
            free(d);
        }
    printf("@end\n\n");
}

static void dumpClass(Class c) {
    printf("@interface %s", class_getName(c));
    for (Class s = class_getSuperclass(c); s; s = class_getSuperclass(s)) printf(" : %s", class_getName(s));
    unsigned n = 0;
    Protocol * __unsafe_unretained *pl = class_copyProtocolList(c, &n);
    if (n) { printf(" <"); for (unsigned i = 0; i < n; i++) printf("%s%s", i ? ", " : "", protocol_getName(pl[i])); printf(">"); }
    free(pl);
    printf("  (instance size %zu)\n", class_getInstanceSize(c));
    Ivar *iv = class_copyIvarList(c, &n);
    for (unsigned i = 0; i < n; i++)
        printf("  ivar +0x%03tx %s %s\n", ivar_getOffset(iv[i]), ivar_getName(iv[i]), ivar_getTypeEncoding(iv[i]) ?: "");
    free(iv);
    objc_property_t *pr = class_copyPropertyList(c, &n);
    for (unsigned i = 0; i < n; i++) printf("  @property %s  %s\n", property_getName(pr[i]), property_getAttributes(pr[i]));
    free(pr);
    for (int meta = 1; meta >= 0; meta--) {
        Class k = meta ? object_getClass((id)c) : c;
        Method *ml = class_copyMethodList(k, &n);
        for (unsigned i = 0; i < n; i++)
            printf("  %c %s  %s\n", meta ? '+' : '-', sel_getName(method_getName(ml[i])), method_getTypeEncoding(ml[i]) ?: "");
        free(ml);
    }
    printf("@end\n\n");
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: objc_dump <image path> [--protocols PREFIX]\n"); return 1; }
        const char *path = argv[1];
        void *h = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
        if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
        // classes whose defining image path contains the framework/bundle name
        // (images inside the shared cache are reported under their Versions/A path)
        const char *key = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        if (argc >= 3 && strcmp(argv[2], "--protocols")) key = argv[2];
        unsigned n = 0, shown = 0;
        Class *all = objc_copyClassList(&n);
        for (unsigned i = 0; i < n; i++) {
            const char *img = class_getImageName(all[i]);
            if (img && strstr(img, key)) { dumpClass(all[i]); shown++; }
        }
        free(all);
        printf("// %u classes from images matching '%s'\n\n", shown, key);
        int pi = (argc >= 4 && !strcmp(argv[2], "--protocols")) ? 3 : (argc >= 5 && !strcmp(argv[3], "--protocols")) ? 4 : 0;
        if (pi) {
            unsigned pn = 0;
            Protocol * __unsafe_unretained *allp = objc_copyProtocolList(&pn);
            for (unsigned i = 0; i < pn; i++)
                if (!strncmp(protocol_getName(allp[i]), argv[pi], strlen(argv[pi]))) dumpProtocol(allp[i]);
            free(allp);
        }
    }
    return 0;
}
