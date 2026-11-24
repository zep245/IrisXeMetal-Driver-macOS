// irisxe-vtdecode (v0.68.0, decoder list v0.68.3): hardware video decoding test. Makes a short H.264 clip itself (VideoToolbox's software
// encoder, 30 frames of a moving colour pattern, 640x480), then decodes it with the HARDWARE decoder required, and
// reports what happened: whether VideoToolbox offers hardware H.264 decoding, whether a hardware decoder session
// opens, and per frame the decode status and whether a picture came back. Never waits forever: the decoding runs on
// a second thread; after the time limit the tool reports and exits.
//   ./irisxe-vtdecode [seconds]
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreVideo/CoreVideo.h>
#include <stdatomic.h>

// v0.68.3: VideoToolbox's decoder list (exported, not in the public headers)
extern OSStatus VTCopyVideoDecoderList(CFDictionaryRef options, CFArrayRef *listOfVideoDecodersOut);
extern const CFStringRef kVTVideoDecoderList_CodecType, kVTVideoDecoderList_DecoderID, kVTVideoDecoderList_DisplayName,
    kVTVideoDecoderList_IsHardwareAccelerated, kVTVideoDecoderList_GPURegistryID;

static NSMutableArray *gSamples;
static CMFormatDescriptionRef gFormat;
static atomic_int gDecoded, gDecodeErrors, gPictures;
static double gMean = -1;

static void encoded(void *ref, void *src, OSStatus st, VTEncodeInfoFlags f, CMSampleBufferRef sb) {
    if (st != noErr || !sb) { printf("  encode error %d\n", (int)st); return; }
    if (!gFormat) { gFormat = CMSampleBufferGetFormatDescription(sb); CFRetain(gFormat); }
    [gSamples addObject:(__bridge id)sb];
}
static void decoded(void *ref, void *src, OSStatus st, VTDecodeInfoFlags f, CVImageBufferRef img, CMTime pts, CMTime dur) {
    atomic_fetch_add(&gDecoded, 1);
    if (st != noErr) { atomic_fetch_add(&gDecodeErrors, 1); printf("  frame %lld: decode status %d\n", pts.value, (int)st); return; }
    if (!img) { printf("  frame %lld: no picture\n", pts.value); return; }
    atomic_fetch_add(&gPictures, 1);
    if (gMean < 0 && CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess) {
        const uint8_t *y = CVPixelBufferIsPlanar(img) ? CVPixelBufferGetBaseAddressOfPlane(img, 0) : CVPixelBufferGetBaseAddress(img);
        size_t bpr = CVPixelBufferIsPlanar(img) ? CVPixelBufferGetBytesPerRowOfPlane(img, 0) : CVPixelBufferGetBytesPerRow(img);
        size_t h = CVPixelBufferGetHeight(img), w = CVPixelBufferGetWidth(img);
        double sum = 0; size_t n = 0;
        for (size_t r = 0; y && r < h; r += 8) for (size_t c = 0; c < w && c < bpr; c += 8) { sum += y[r * bpr + c]; n++; }
        gMean = n ? sum / n : 0;
        OSType pf = CVPixelBufferGetPixelFormatType(img);
        printf("  first picture: %zux%zu, format %c%c%c%c, mean luma %.1f (a black or empty picture is ~0-16)\n", w, h,
               (char)(pf >> 24), (char)(pf >> 16), (char)(pf >> 8), (char)pf, gMean);
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        int limit = argc > 1 ? atoi(argv[1]) : 20;
        if (limit < 3) limit = 3;
        printf("irisxe-vtdecode: VTIsHardwareDecodeSupported(H.264) = %d, (HEVC) = %d\n",
               VTIsHardwareDecodeSupported(kCMVideoCodecType_H264), VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC));
        // v0.68.3: every decoder VideoToolbox lists on this boot (a hardware decoder whose IORegistry dependency is not
        // met is left out of this list - round 42: plug-in decoders name an IORegistryRequiredKey, e.g. IOGVAXDecode)
        CFArrayRef list = NULL;
        if (VTCopyVideoDecoderList(NULL, &list) == noErr && list) {
            printf("decoders listed: %ld\n", (long)CFArrayGetCount(list));
            for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
                NSDictionary *d = (__bridge NSDictionary *)CFArrayGetValueAtIndex(list, i);
                NSNumber *ct = d[(id)kVTVideoDecoderList_CodecType];
                uint32_t c = ct.unsignedIntValue;
                printf("  %c%c%c%c  %-55s %-40s hw %s gpu %s\n", (char)(c >> 24), (char)(c >> 16), (char)(c >> 8), (char)c,
                       [[d[(id)kVTVideoDecoderList_DecoderID] description] UTF8String] ?: "?",
                       [[d[(id)kVTVideoDecoderList_DisplayName] description] UTF8String] ?: "?",
                       [[d[(id)kVTVideoDecoderList_IsHardwareAccelerated] description] UTF8String] ?: "-",
                       [[d[(id)kVTVideoDecoderList_GPURegistryID] description] UTF8String] ?: "-");
            }
            CFRelease(list);
        } else printf("decoders listed: (VTCopyVideoDecoderList failed)\n");
        // 1. a clip, made with the software encoder
        gSamples = [NSMutableArray array];
        const int W = 640, H = 480, N = 30;
        NSDictionary *enc = @{ (id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @NO };
        VTCompressionSessionRef cs = NULL;
        OSStatus st = VTCompressionSessionCreate(NULL, W, H, kCMVideoCodecType_H264, (__bridge CFDictionaryRef)enc, NULL, NULL,
                                                 encoded, NULL, &cs);
        if (st != noErr) { printf("software H.264 encoder: error %d\n", (int)st); return 1; }
        VTSessionSetProperty(cs, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
        VTSessionSetProperty(cs, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel);
        VTSessionSetProperty(cs, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
        for (int i = 0; i < N; i++) {
            CVPixelBufferRef pb = NULL;
            CVPixelBufferCreate(NULL, W, H, kCVPixelFormatType_32BGRA, NULL, &pb);
            CVPixelBufferLockBaseAddress(pb, 0);
            uint8_t *p = CVPixelBufferGetBaseAddress(pb); size_t bpr = CVPixelBufferGetBytesPerRow(pb);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                uint8_t *q = p + y * bpr + x * 4;
                q[0] = (uint8_t)(x + i * 8); q[1] = (uint8_t)(y + i * 4); q[2] = (uint8_t)((x ^ y) + i * 2); q[3] = 255;
            }
            CVPixelBufferUnlockBaseAddress(pb, 0);
            VTCompressionSessionEncodeFrame(cs, pb, CMTimeMake(i, 30), CMTimeMake(1, 30), NULL, NULL, NULL);
            CVPixelBufferRelease(pb);
        }
        VTCompressionSessionCompleteFrames(cs, kCMTimeInvalid);
        VTCompressionSessionInvalidate(cs); CFRelease(cs);
        printf("clip: %lu H.264 frames (software encoder)\n", (unsigned long)gSamples.count);
        if (!gFormat || !gSamples.count) return 1;
        // 2. decode with the hardware decoder required (on a second thread: a stuck driver must not hang this tool)
        __block int sessionStatus = 1;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSDictionary *spec = @{ (id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES };
            NSDictionary *out = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) };
            VTDecompressionOutputCallbackRecord cb = { decoded, NULL };
            VTDecompressionSessionRef ds = NULL;
            OSStatus s = VTDecompressionSessionCreate(NULL, gFormat, (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)out, &cb, &ds);
            sessionStatus = s;
            printf("hardware decoder session: %s (%d)\n", s == noErr ? "OPENED" : "refused", (int)s);
            if (s == noErr) {
                CFBooleanRef hw = NULL;
                VTSessionCopyProperty(ds, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hw);
                printf("  using hardware decoder: %s\n", hw == kCFBooleanTrue ? "yes" : "no / unknown");
                if (hw) CFRelease(hw);
                for (id o in gSamples) {
                    VTDecodeInfoFlags info = 0;
                    OSStatus d = VTDecompressionSessionDecodeFrame(ds, (__bridge CMSampleBufferRef)o, 0, NULL, &info);
                    if (d != noErr) { printf("  DecodeFrame returned %d\n", (int)d); break; }
                }
                VTDecompressionSessionWaitForAsynchronousFrames(ds);
                VTDecompressionSessionInvalidate(ds); CFRelease(ds);
            }
            dispatch_semaphore_signal(done);
        });
        long r = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)limit * NSEC_PER_SEC));
        printf("result: %s - %d of %lu frames came back, %d with errors, %d pictures\n",
               r ? "TIMED OUT (the decoder did not finish)" : "finished", atomic_load(&gDecoded), (unsigned long)gSamples.count,
               atomic_load(&gDecodeErrors), atomic_load(&gPictures));
        fflush(stdout);
        _exit(sessionStatus == noErr && !r && atomic_load(&gPictures) ? 0 : 2);
    }
}
