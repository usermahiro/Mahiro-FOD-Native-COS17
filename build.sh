#!/bin/sh
set -e
: "${NDK:?set NDK=/path/to/android-ndk}"

cd "$(dirname "$0")"

CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android31-clang++"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [ -z "$ANDROID_JAR" ] && [ -n "$SDK" ]; then
    ANDROID_JAR=$(ls "$SDK"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -1)
fi
if [ -z "$D8" ] && [ -n "$SDK" ]; then
    D8=$(ls "$SDK"/build-tools/*/d8 2>/dev/null | sort -V | tail -1)
fi

if [ -z "$ANDROID_JAR" ] || [ ! -f "$ANDROID_JAR" ]; then
    echo "error: android.jar not found (set ANDROID_JAR=...)" >&2
    exit 1
fi
if [ -z "$D8" ] || [ ! -x "$D8" ]; then
    echo "error: d8 not found (set D8=... or ANDROID_SDK_ROOT)" >&2
    exit 1
fi

mkdir -p rom/vendor/bin/hw rom/system/lib64 build

echo "Compiling embedded UI helper..."
rm -rf build/helper_classes build/helper_dex
mkdir -p build/helper_classes build/helper_dex
javac -source 8 -target 8 -Xlint:-options \
    -classpath "$ANDROID_JAR" \
    -d build/helper_classes \
    src/helper/mahiro/fod/NativeUi.java
"$D8" --lib "$ANDROID_JAR" --min-api 26 --output build/helper_dex build/helper_classes

python3 - "$PWD/build/helper_dex/classes.dex" "$PWD/build/helper_dex.h" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
with open(dst, 'w') as f:
    f.write('// generated from classes.dex\n')
    f.write('static const unsigned char kHelperDex[] = {\n')
    for i in range(0, len(data), 12):
        f.write(','.join('0x%02x' % b for b in data[i:i+12]) + ',\n')
    f.write('};\n')
    f.write('static const unsigned long kHelperDexLen = %dUL;\n' % len(data))
print('embedded helper dex: %d bytes' % len(data))
PY

echo "Compiling bridge daemon..."
$CXX -O2 -fPIE -pie -static-libstdc++ src/fodbridged.cpp \
    -o rom/vendor/bin/hw/fodbridged \
    -llog

echo "Compiling injector..."
$CXX -O2 -fPIE -pie -static-libstdc++ src/fodinject.cpp \
    -o rom/vendor/bin/hw/fodinject \
    -llog -ldl

echo "Compiling hook library..."
$CXX -O2 -shared -fPIC -static-libstdc++ -I build src/libfodhook.cpp \
    -o rom/system/lib64/libfodhook.so \
    -llog -ldl

echo "Done"
