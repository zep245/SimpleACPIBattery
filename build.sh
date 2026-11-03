#!/bin/bash
# build.sh <out dir> - builds SimpleACPIBattery.kext (x86_64) with the SDK's Kernel.framework headers
set -euo pipefail
OUT=${1:?usage: build.sh <out dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
SDK=$(xcrun --sdk macosx --show-sdk-path)
KF=$SDK/System/Library/Frameworks/Kernel.framework/Headers
K="$OUT/SimpleACPIBattery.kext/Contents"; mkdir -p "$K/MacOS"
T=$(mktemp -d)
xcrun clang++ -arch x86_64 -mmacosx-version-min=${MIN_OS:-14.0} -mkernel -fapple-kext -nostdinc -isysroot "$SDK" -I"$KF" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common \
  -O2 -g0 -Wall -Wno-unused-parameter -Wno-deprecated-declarations -Wno-inconsistent-missing-override \
  -c "$HERE/SimpleACPIBattery.cpp" -o "$T/SimpleACPIBattery.o"
xcrun clang -arch x86_64 -mmacosx-version-min=${MIN_OS:-14.0} -fapple-kext -nostdlib -Xlinker -kext \
  "$T/SimpleACPIBattery.o" -lkmodc++ -lkmod -lcc_kext -o "$K/MacOS/SimpleACPIBattery"
cp "$HERE/Info.plist" "$K/Info.plist"
echo "SimpleACPIBattery.kext: $(stat -f %z "$K/MacOS/SimpleACPIBattery") bytes"
