# accelgen

Generates C++ headers for Apple's IOAcceleratorFamily2 base classes (no public
headers exist) from the vtable listings produced by
`tools/metal-research/kcdump.py` on a macOS 15 Intel runner.

- `data/*.vtable.txt` - slot order + demangled signatures (names only)
- `data/IOAcceleratorFamily2.exports.txt` - exported symbol names (demangled)
- `check_links.sh` - CI check: every family symbol our kext uses exists in Apple's kext

Regenerate:
    python3 tools/accelgen/gen_family_headers.py src/accel/apple
    python3 tools/accelgen/gen_subclasses.py src/accel/apple/SUMMARY.txt \
        src/accel/IrisXeAccelClasses.hpp src/accel/IrisXeAccelClasses.cpp src/accel/IrisXeVtCheck.inc

v0.16.1 (step 1b):
- `gen_family_headers.py src/accel/apple` - headers for 13 Apple classes
  (parent chains: OSObject slots end at 36, IOService at 267, IOUserClient at 301)
  and `src/accel/apple/SUMMARY.txt` (what each subclass must implement)
- `gen_subclasses.py` - our stub subclasses + the vtable check table
- `data/family.vtables.txt` - vtable listings (names only), `data/IOAcceleratorFamily2.all.txt` - all symbol names
