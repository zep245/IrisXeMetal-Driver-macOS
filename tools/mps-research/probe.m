// Loads Metal and Metal Performance Shaders so lldb can disassemble MPSCore / MPSImage (research only)
#import <Metal/Metal.h>
#import <MetalPerformanceShaders/MetalPerformanceShaders.h>
#include <stdio.h>
int main(void) {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    printf("device %s, MPSSupportsMTLDevice %d\n", d ? [[d name] UTF8String] : "(none)", d ? (int)MPSSupportsMTLDevice(d) : -1);
    return 0;
}
