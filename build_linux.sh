#!/bin/bash
# Builds MXS Remastered for Linux (x86-64) into bin/Linux/: libmxs-remastered.so and the
# mxs-remastered.sh launcher. Needs a C++17 compiler (g++ or clang++; CXX picks one). The GitHub
# Actions workflow (.github/workflows/linux.yml) runs this on every push.
#
# Built for glibc 2.17 and later (any 64-bit Linux of the last ten years) when the compiler can
# target it (Zig: CXX="zig c++ -target x86_64-linux-gnu.2.17"); with the system's own compiler, for
# that system's glibc and later. The C++ runtime is linked in, so nothing else is needed at run time.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
out=bin/Linux
obj=obj/Linux
mkdir -p "$out" "$obj"

sources=(
    camera replay terrain bike lobby_wheels timeofday log config postfx proc_hooks texfilter hooks
    overlay gamepad dab brake sound_packs
    linux/win32_compat linux/input_linux linux/paths_linux linux/fonts_linux linux/stb_impl linux/entry_linux
)
flags=(-std=c++17 -O2 -fPIC -fvisibility=hidden -Isrc/linux/include -Wall -Wno-unused-function -Wno-missing-braces)

objects=()
for s in "${sources[@]}"; do
    o="$obj/$(basename "$s").o"
    echo "  $s.cpp"
    $CXX "${flags[@]}" -c -o "$o" "src/$s.cpp"
    objects+=("$o")
done

link=(-shared -fPIC -s -Wl,--no-undefined -Wl,--version-script=src/linux/exports.map)
if [[ "$CXX" != *zig* ]]; then
    link+=(-static-libstdc++ -static-libgcc)
fi
$CXX "${link[@]}" -o "$out/libmxs-remastered.so" "${objects[@]}" -ldl -lpthread
cp src/linux/mxs-remastered.sh "$out/mxs-remastered.sh"
chmod +x "$out/mxs-remastered.sh"

echo
echo "Built:"
echo "  $out/libmxs-remastered.so"
echo "  $out/mxs-remastered.sh"
