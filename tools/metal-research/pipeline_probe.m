// pipeline_probe (research round 10, milestone 3b): how Metal turns a compute
// function into a pipeline on a GPU driver. Lists Metal's compiler/pipeline
// classes and methods, and what a compiled MTLFunction object holds (class,
// ivars, methods). Text output only, for interoperability research.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>

static void dumpClass(Class c, BOOL methods) {
    printf("@interface %s", class_getName(c));
    for (Class s = class_getSuperclass(c); s; s = class_getSuperclass(s)) printf(" : %s", class_getName(s));
    printf("  (size %zu)\n", class_getInstanceSize(c));
    unsigned n = 0;
    Ivar *iv = class_copyIvarList(c, &n);
    for (unsigned i = 0; i < n; i++)
        printf("  ivar +0x%03lx %s %.120s\n", (long)ivar_getOffset(iv[i]), ivar_getName(iv[i]), ivar_getTypeEncoding(iv[i]) ?: "");
    free(iv);
    if (!methods) return;
    Method *m = class_copyMethodList(c, &n);
    for (unsigned i = 0; i < n; i++) printf("  - %s  %s\n", sel_getName(method_getName(m[i])), method_getTypeEncoding(m[i]) ?: "");
    free(m);
    m = class_copyMethodList(object_getClass(c), &n);
    for (unsigned i = 0; i < n; i++) printf("  + %s  %s\n", sel_getName(method_getName(m[i])), method_getTypeEncoding(m[i]) ?: "");
    free(m);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        Dl_info di = {0};
        dladdr((void *)MTLCreateSystemDefaultDevice, &di);
        printf("Metal image: %s\n", di.dli_fname ?: "?");
        const char *re = argc > 1 ? argv[1] : "Compil|Function|Library|Pipeline|Variant|Binary|Archive";
        NSRegularExpression *rx = [NSRegularExpression regularExpressionWithPattern:@(re) options:0 error:nil];
        unsigned n = 0;
        const char **names = objc_copyClassNamesForImage(di.dli_fname, &n);
        printf("%u classes in Metal\n\n", n);
        for (unsigned i = 0; i < n; i++) {
            NSString *s = @(names[i]);
            if ([rx numberOfMatchesInString:s options:0 range:NSMakeRange(0, s.length)])
                dumpClass(objc_getClass(names[i]), YES);
        }
        free(names);

        printf("\n==== a compute function from source on this machine's GPU\n");
        NSArray *devs = MTLCopyAllDevices();
        for (id<MTLDevice> d in devs) printf("device: %s (%s)\n", d.name.UTF8String, class_getName([(id)d class]));
        id<MTLDevice> d = devs.firstObject;
        if (!d) { printf("no Metal device on this runner\n"); return 0; }
        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void add1(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],"
            " uint i [[thread_position_in_grid]]) { out[i] = in[i] + 1; }\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [d newLibraryWithSource:src options:nil error:&err];
        printf("library: %s %s\n", lib ? class_getName([(id)lib class]) : "nil", err ? err.description.UTF8String : "");
        if (!lib) return 0;
        dumpClass([(id)lib class], YES);
        id<MTLFunction> f = [lib newFunctionWithName:@"add1"];
        printf("function: %s\n", f ? class_getName([(id)f class]) : "nil");
        if (f) dumpClass([(id)f class], YES);
        for (NSString *k in @[@"bitcodeData", @"functionData", @"libraryData", @"bitcodeHash"]) {
            SEL s = NSSelectorFromString(k);
            if ([(id)f respondsToSelector:s]) {
                id v = ((id (*)(id, SEL))objc_msgSend)((id)f, s);
                printf("  [function %s] -> %s, %lu bytes\n", k.UTF8String, v ? class_getName([v class]) : "nil",
                       [v isKindOfClass:[NSData class]] ? (unsigned long)[(NSData *)v length] : 0UL);
            }
        }
        id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:f error:&err];
        printf("pipeline: %s %s\n", ps ? class_getName([(id)ps class]) : "nil", err ? err.description.UTF8String : "");
        if (ps) dumpClass([(id)ps class], YES);
    }
    return 0;
}
