#!/bin/sh
# Worker for tool/run-cruby.sh.
#
# Usage: tool/analyze-one.sh 'NAME<TAB>DIR<TAB>SOURCE<TAB>FLAGS'
#
# Compiles SOURCE in DIR to LLVM IR with FLAGS and runs gc-guard-check on it,
# writing $OUT/NAME.{ll,txt,err}. LLVM_BIN, PLUGIN and OUT come from the
# environment.

set -eu

tab=$(printf '\t')
IFS=$tab read -r name dir src flags <<EOF
$1
EOF

ll=$OUT/$name.ll
# FLAGS is split into words on purpose; quotes in it (e.g.
# -DRUBY_EXTCONF_H="extconf.h") are kept literally, as make passes them.
# shellcheck disable=SC2086
if (cd "$dir" && "$LLVM_BIN/clang" -O2 -g -S -emit-llvm -w $flags "$src" -o "$ll") 2>"$OUT/$name.err"; then
    "$LLVM_BIN/opt" -load-pass-plugin="$PLUGIN" -passes=gc-guard-check \
        -disable-output "$ll" >"$OUT/$name.txt" 2>>"$OUT/$name.err"
else
    echo "compile failed: $src (see $OUT/$name.err)" >&2
fi
