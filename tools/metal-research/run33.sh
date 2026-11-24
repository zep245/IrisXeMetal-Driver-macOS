#!/bin/bash
# Round 33 (v0.66.1): hardware video decoding. How does VideoToolbox reach an Intel GPU's media engine on an Intel
# Mac? VideoToolbox -> AppleGVA (+ AppleGVACore) -> a per-GPU "VA driver" bundle (AppleIntel<gen>GraphicsVADriver)
# -> the GPU kext's video user clients -> the media engine (VDBox). This round collects, as text:
#   1. which VA / GVA bundles and kexts Sequoia has, and their Info.plists (what properties select them)
#   2. the IOKit properties AppleGVA / the VA drivers look for (strings), and their exported / imported symbols
#   3. the Intel kexts' video side: classes, user clients, symbols and strings (IGAccelVideoContext*, MFX, HuC ..)
#   4. how AppleGVA picks the VA driver for a GPU (disassembly of the functions loading those property names)
#   5. what VideoToolbox reports on this runner (vtprobe)
# Text output only (no binaries uploaded). Round 32 is kept as run32.sh.
set -x
OUT="$PWD/out"; mkdir -p "$OUT"
sw_vers > "$OUT/00-versions.txt" 2>&1
pip3 install --break-system-packages capstone > "$OUT/01-capstone.txt" 2>&1 || pip3 install capstone >> "$OUT/01-capstone.txt" 2>&1
# 1. inventory
{ ls -la /System/Library/Extensions | grep -iE "VA|GVA|Video|Intel|Media|HEVC|AVD|AppleGraphics"; echo; ls -la /System/Library/PrivateFrameworks | grep -iE "GVA|Video|VA\.|Media|AVD"; } > "$OUT/v00-inventory.txt" 2>&1
for b in /System/Library/Extensions/*VADriver*.bundle /System/Library/Extensions/*GVA*.bundle /System/Library/Extensions/AppleIntel*Graphics.kext /System/Library/Extensions/AppleIntel*Framebuffer*.kext /System/Library/Extensions/AppleIntel*MTLDriver.bundle; do
    [ -e "$b/Contents/Info.plist" ] || continue
    echo "=== $b"; plutil -p "$b/Contents/Info.plist"; ls -la "$b/Contents" "$b/Contents/MacOS" "$b/Contents/Resources" 2>/dev/null | head -40
done > "$OUT/v01-plists.txt" 2>&1
# 2. user-space binaries out of the shared cache, then strings / symbols (text only; binaries are deleted)
xcrun clang -O1 dsc_extract.c -o dsc_extract > "$OUT/v02-build.txt" 2>&1
DSCX=$(ls -d "$(xcode-select -p)"/Platforms/MacOSX.platform/usr/lib/dsc_extractor.bundle /usr/lib/dsc_extractor.bundle 2>/dev/null | head -1)
CACHE=$(ls /System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h /System/Library/dyld/dyld_shared_cache_x86_64h 2>/dev/null | head -1)
X="$PWD/dsc"; mkdir -p "$X"
echo "extractor $DSCX cache $CACHE" >> "$OUT/v02-build.txt"
./dsc_extract "$DSCX" "$CACHE" "$X" >> "$OUT/v02-build.txt" 2>&1
find "$X" -iname "*GVA*" -o -iname "*VADriver*" -o -iname "VideoToolbox" -o -iname "*AVD*" -o -iname "MediaToolbox" -o -iname "CoreMedia" -o -iname "*VideoProcessing*" | grep -v "\.dSYM" > "$OUT/v03-found.txt" 2>&1
while read -r f; do
    [ -f "$f" ] || continue
    n=$(echo "$f" | sed "s|$X/||; s|/|_|g")
    case "$n" in *MediaToolbox*|*CoreMedia*) ;; *)
        strings -a "$f" | grep -iE "IOVA|IOGVA|VADriver|GVA|Intel|IGAccel|IOAccel|VDBox|MFX|HuC|HEVC|AVC|VP9|AV1|codec|Codec|IOService|Matching|Renderer|AppleIntel|plugin|Plugin|\.bundle" | sort -u > "$OUT/v10-strings-$n.txt" 2>&1 ;;
    esac
    nm -m "$f" 2>/dev/null | grep -iE "GVA|VA[A-Z]|Intel|Decode|decode|Codec|IOService|IOConnect|Plugin|Factory" | head -3000 > "$OUT/v11-symbols-$n.txt" 2>&1
    otool -L "$f" > "$OUT/v12-links-$n.txt" 2>&1
done < "$OUT/v03-found.txt"
# objc classes of the VA drivers / GVA (some are Objective-C)
xcrun clang -fobjc-arc -framework Foundation -o objc_dump objc_dump.m >> "$OUT/v02-build.txt" 2>&1
for f in /System/Library/PrivateFrameworks/AppleGVA.framework/AppleGVA /System/Library/PrivateFrameworks/AppleGVACore.framework/AppleGVACore /System/Library/Extensions/AppleIntelICLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelICLGraphicsVADriver /System/Library/Extensions/AppleIntelKBLGraphicsVADriver.bundle/Contents/MacOS/AppleIntelKBLGraphicsVADriver; do
    [ -e "$f" ] || [ -e "$(dirname "$f")" ] || continue
    ./objc_dump "$f" "$(basename "$f")" > "$OUT/v13-objc-$(basename "$f").txt" 2>&1
done
# 3. the Intel kexts' video side
KC=/System/Library/KernelCollections
A="$KC/BootKernelExtensions.kc $KC/SystemKernelExtensions.kc"
IDS="com.apple.driver.AppleIntelICLGraphics com.apple.driver.AppleIntelKBLGraphics com.apple.driver.AppleIntelICLLPGraphicsFramebuffer com.apple.driver.AppleGVA"
python3 kcdump.py "$OUT" $A -- $IDS > "$OUT/v20-kcdump-stdout.txt" 2>&1
for f in "$OUT"/12-*-vtables.txt "$OUT"/10-*-symbols.txt; do [ -f "$f" ] && c++filt < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
rm -f "$OUT"/11-*-segments.txt
grep -a "^vtable for" "$OUT"/12-*-vtables.txt | sed 's/ @.*//' | grep -iE "Video|Media|VA|MFX|HuC|Codec|Decode|Encode|VDBox|VCS|Context|UserClient" > "$OUT/v21-video-classes.txt" 2>&1
grep -a -iE "video|media|mfx|huc|guc|vdbox|vcs|hevc|avc|vp9|codec|decode|encode|IOVA|GVA|VAD" "$OUT"/13-*-strings.txt > "$OUT/v22-video-strings.txt" 2>&1
grep -a -iE "Video|Media|MFX|HuC|VDBox|VCS|Codec|Decode|Encode|IOVA|GVA" "$OUT"/10-*-symbols.txt > "$OUT/v23-video-symbols.txt" 2>&1
rm -f "$OUT"/13-*-strings.txt
# 4. how AppleGVA picks a decoder / VA driver: functions that load the property names
xcrun clang -fno-objc-arc -O1 -framework Foundation -framework Metal -framework IOKit udis_server.m -o udis_server >> "$OUT/v02-build.txt" 2>&1
GVA=/System/Library/PrivateFrameworks/AppleGVA.framework/AppleGVA
GVAC=/System/Library/PrivateFrameworks/AppleGVACore.framework/AppleGVACore
VT=/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox
python3 udis.py "$OUT/v30-gva-select.txt" "load:$GVAC" "load:$GVA" "load:$VT" \
  'x:AppleGVA:IOVARendererID' 'x:AppleGVA:VADriver' 'x:AppleGVA:IOGVA' 'x:AppleGVA:GVA' 'x:AppleGVACore:IOVARendererID' 'x:AppleGVACore:VADriver' \
  'x:AppleGVACore:IOGVA' 'x:VideoToolbox:AppleGVA' 'x:VideoToolbox:HardwareDecode' \
  'y:AppleGVA:([Ss]upported|[Cc]apab|[Rr]enderer|[Pp]lugin|[Ll]oad|[Ff]actory|[Cc]reate[A-Z])' > "$OUT/v30-log.txt" 2>&1
# 5. VideoToolbox here
xcrun clang -fobjc-arc -framework Foundation -framework VideoToolbox -framework CoreMedia vtprobe.m -o vtprobe >> "$OUT/v02-build.txt" 2>&1
./vtprobe > "$OUT/v40-vtprobe.txt" 2>&1
ioreg -l -w0 | grep -iE "IOVA|IOGVA|VARenderer|GVA" | head -100 > "$OUT/v41-ioreg-va.txt" 2>&1
rm -rf "$X"
ls -la "$OUT"
exit 0
