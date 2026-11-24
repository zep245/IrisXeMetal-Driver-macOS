#!/bin/bash
# Prints the system libraries LLVM needs, for linking LLVM statically into our
# shader compiler without any Homebrew dylib dependency (v0.25.1: Homebrew's
# llvm-config lists /usr/local/opt/z3/.../libz3.dylib, which no Mac has).
#   macOS system libs (-lm -lz -lcurses -lxml2 ...) are kept,
#   zstd is replaced by Homebrew's static libzstd.a,
#   z3 (LLVM's optional SMT solver, which we never call) is dropped,
#   any other Homebrew dylib: its static .a if there is one, else dropped (warned).
LC="$1"
ZSTD_A="$(brew --prefix zstd 2>/dev/null)/lib/libzstd.a"
for t in $("$LC" --link-static --system-libs); do
  case "$t" in
    *z3*) echo "llvm-syslibs: dropping $t" >&2 ;;
    -lzstd|*libzstd*) [ -f "$ZSTD_A" ] && echo -n "$ZSTD_A " || echo "llvm-syslibs: no static zstd" >&2 ;;
    /usr/local/*|/opt/homebrew/*)
      a="${t%.dylib}"; a="${a%%.[0-9]*}.a"
      if [ -f "$a" ]; then echo -n "$a "; else echo "llvm-syslibs: dropping $t (no static library)" >&2; fi ;;
    *) echo -n "$t " ;;
  esac
done
echo
