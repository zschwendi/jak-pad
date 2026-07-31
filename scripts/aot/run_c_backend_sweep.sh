#!/bin/sh
# Measure AOT C backend coverage over every Jak 1 GOAL source the game's own build definition
# reaches, then compile all of the generated C for arm64-apple-ios.
#
# Usage: scripts/aot/run_c_backend_sweep.sh [OUTPUT_DIR]
# Run from the project root, after building goalc-cbackend-sweep.

set -e
root=$(pwd)
out=${1:-out/c-backend-sweep}
mkdir -p "$out/c" "$out/ios-obj" "$out/ios-log"

"$root/build/goalc/goalc-cbackend-sweep" \
  --project-path "$root" \
  --c-output-dir "$out/c" \
  --report "$out/sweep.tsv"

: > "$out/ios-compile-failures.txt"
for c in "$out"/c/*.c; do
  name=$(basename "$c" .c)
  if ! xcrun -sdk iphoneos clang -c -target arm64-apple-ios15.0 -std=c11 -fno-strict-aliasing \
       -Wno-unused-variable -Wno-unused-but-set-variable -I"$root" \
       "$c" -o "$out/ios-obj/$name.o" > "$out/ios-log/$name.log" 2>&1; then
    echo "$name" >> "$out/ios-compile-failures.txt"
  fi
done

echo "arm64-apple-ios objects: $(ls "$out/ios-obj" | wc -l)"
echo "arm64-apple-ios failures: $(wc -l < "$out/ios-compile-failures.txt")"
python3 "$root/scripts/aot/summarize_c_backend_sweep.py" "$out/sweep.tsv"
