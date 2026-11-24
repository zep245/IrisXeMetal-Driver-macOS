// vtprobe (round 33): what VideoToolbox offers on this Mac - its decoder list and hardware decode per codec.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
int main(void) {
    @autoreleasepool {
        CFArrayRef list = NULL;
        OSStatus s = VTCopyVideoDecoderList(NULL, &list);
        printf("VTCopyVideoDecoderList: %d, %ld decoders\n", (int)s, list ? (long)CFArrayGetCount(list) : 0L);
        for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
            NSDictionary *d = (__bridge NSDictionary *)CFArrayGetValueAtIndex(list, i);
            printf("  %s\n", [[d description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "].UTF8String);
        }
        if (list) CFRelease(list);
        struct { const char *n; CMVideoCodecType t; } c[] = {
            { "H.264 avc1", kCMVideoCodecType_H264 }, { "HEVC hvc1", kCMVideoCodecType_HEVC },
            { "VP9 vp09", 'vp09' }, { "AV1 av01", 'av01' }, { "ProRes apcn", kCMVideoCodecType_AppleProRes422 },
            { "JPEG jpeg", kCMVideoCodecType_JPEG } };
        for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++)
            printf("VTIsHardwareDecodeSupported %-12s %d\n", c[i].n, VTIsHardwareDecodeSupported(c[i].t));
    }
    return 0;
}
