// air_probe (research round 11, milestone 3c): save the AIR (LLVM bitcode)
// Metal keeps for a compute function at run time, so we can check whether a
// stock LLVM can read it. Text/bitcode of our own test shader only.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        const char *outPath = argc > 1 ? argv[1] : "runtime.air";
        id<MTLDevice> d = MTLCopyAllDevices().firstObject;
        if (!d) { printf("no Metal device\n"); return 0; }
        printf("device %s\n", d.name.UTF8String);
        NSString *src = [NSString stringWithContentsOfFile:@"kernels.metal" encoding:NSUTF8StringEncoding error:nil];
        NSError *err = nil;
        id<MTLLibrary> lib = [d newLibraryWithSource:src options:nil error:&err];
        if (!lib) { printf("library failed: %s\n", err.description.UTF8String); return 0; }
        printf("functions: %s\n", [[lib.functionNames componentsJoinedByString:@" "] UTF8String]);
        for (NSString *name in lib.functionNames) {
            id<MTLFunction> f = [lib newFunctionWithName:name];
            id bc = [(id)f respondsToSelector:@selector(bitcodeData)] ? ((id (*)(id, SEL))objc_msgSend)((id)f, @selector(bitcodeData)) : nil;
            printf("%s: bitcodeData %s", name.UTF8String, bc ? class_getName([bc class]) : "nil");
            if ([bc isKindOfClass:[NSData class]]) {
                NSData *data = (NSData *)bc;      // dispatch_data_t bridges to NSData
                NSString *p = [NSString stringWithFormat:@"%s.%@.bc", outPath, name];
                [data writeToFile:p atomically:YES];
                const uint8_t *b = data.bytes;
                printf(" %lu bytes, starts %02x %02x %02x %02x -> %s", (unsigned long)data.length, b[0], b[1], b[2], b[3], p.UTF8String);
            }
            printf("\n");
            for (MTLArgument *a in [(id)f respondsToSelector:@selector(arguments)] ? ((id (*)(id, SEL))objc_msgSend)((id)f, @selector(arguments)) : nil)
                printf("   argument %s index %lu type %lu\n", a.name.UTF8String, (unsigned long)a.index, (unsigned long)a.type);
        }
    }
    return 0;
}
