#!/bin/sh
# Run gc-guard-check over CRuby sources.
#
# Usage: tool/run-cruby.sh BUILD_DIR [OUT_DIR]
#
#   BUILD_DIR  A CRuby build directory configured with CC=clang and built
#              with make (generated headers and ext Makefiles are needed).
#   OUT_DIR    Where to write LLVM IR and reports (default: ./cruby-out).
#
# Environment:
#   LLVM_BIN   Directory of clang and opt (default: /usr/lib/llvm-21/bin)
#   PLUGIN     Path to GCGuardCheck.so (default: build/GCGuardCheck.so)
#   JOBS       Parallel jobs (default: nproc)
#   WITH_EXT   Set to 0 to skip ext/ (default: 1)
#
# Reports are collected into OUT_DIR/reports.txt, ordered by origin
# (call, loaded, argument, other).

set -eu

if [ $# -lt 1 ]; then
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$(cd "$1" && pwd)
OUT=$(mkdir -p "${2:-cruby-out}" && cd "${2:-cruby-out}" && pwd)
LLVM_BIN=${LLVM_BIN:-/usr/lib/llvm-21/bin}
PLUGIN=${PLUGIN:-$ROOT/build/GCGuardCheck.so}
JOBS=${JOBS:-$(nproc)}
WITH_EXT=${WITH_EXT:-1}

[ -f "$PLUGIN" ] || { echo "plugin not found: $PLUGIN (run cmake --build build)" >&2; exit 1; }
[ -f "$BUILD/Makefile" ] || { echo "not a build directory: $BUILD" >&2; exit 1; }

# Print a make variable of the Makefile in the given directory.
make_var() {
    make -s -C "$1" --eval "print-var: ; @echo \$($2)" print-var 2>/dev/null
}

SRCDIR=$(make_var "$BUILD" srcdir)
case $SRCDIR in /*) ;; *) SRCDIR=$BUILD/$SRCDIR ;; esac
export LLVM_BIN PLUGIN OUT

rm -f "$OUT"/*.txt
tab=$(printf '\t')

# Jobs, one per line: NAME<TAB>DIR<TAB>SOURCE<TAB>FLAGS
{
    # Core sources
    core_flags=$(make_var "$BUILD" 'XCFLAGS) $(CPPFLAGS')
    for obj in $(make_var "$BUILD" COMMONOBJS); do
        src=$SRCDIR/${obj%.o}.c
        [ -f "$src" ] || continue
        echo "$(echo "${obj%.o}" | tr / _)$tab$BUILD$tab$src$tab$core_flags"
    done

    # Extensions
    if [ "$WITH_EXT" != 0 ]; then
        find "$BUILD/ext" -name Makefile -not -path '*/-test-/*' | while read -r mk; do
            dir=$(dirname "$mk")
            srcdir=$(make_var "$dir" srcdir)
            flags=$(make_var "$dir" 'INCFLAGS) $(CPPFLAGS) $(DEFS')
            # Only sources actually built (OBJS), not platform fallbacks
            for obj in $(make_var "$dir" OBJS | tr ' ' '\n' | sort -u); do
                f=${obj%.o}.c
                # Some sources (ripper.c, sizes.c, ...) are generated in the
                # build directory
                src=$srcdir/$f
                [ -f "$src" ] || src=$dir/$f
                [ -f "$src" ] || continue
                name="ext_$(echo "${dir#"$BUILD"/ext/}_${f%.c}" | tr / _)"
                echo "$name$tab$dir$tab$src$tab$flags"
            done
        done
    fi
} | xargs -d '\n' -P "$JOBS" -n 1 "$ROOT/tool/analyze-one.sh" ||
    echo "warning: some files could not be analyzed (see $OUT/*.err)" >&2

# Collect reports, highest priority first
for origin in call loaded argument other; do
    cat "$OUT"/*.txt 2>/dev/null | grep "\[origin=$origin\]\$" | sort || true
done >"$OUT/reports.txt"

echo "$(wc -l <"$OUT/reports.txt") reports in $OUT/reports.txt" >&2
for origin in call loaded argument other; do
    printf '  %-8s %s\n' "$origin" "$(grep -c "\[origin=$origin\]\$" "$OUT/reports.txt" || true)" >&2
done
