#!/usr/bin/env bash
# Compila o APK do Nightfire PC para Android (arm64-v8a): o jogo (libmain.so, o CMakeLists.txt da raiz com o NDK, o
# SDL2 e o renderizador Vulkan) e o lançador (android/app, Kotlin). O jogo não vem no APK: o jogador aponta o lançador
# para o ISO do próprio disco (docs/android.md).
#
# Uso:
#   android/build.sh              o APK em android/build/out/Nightfire-android-arm64.apk
#   android/build.sh --native     só as bibliotecas nativas (android/app/src/main/jniLibs)
#
# Ambiente: ANDROID_HOME (SDK com a plataforma 35, build-tools 35 e o NDK 27.2.12479018), JDK 17+, CMake 3.24+ e
# Ninja no PATH, rede para a fonte do SDL2 e para o Gradle. Assinatura opcional: NF_KEYSTORE, NF_STORE_PASSWORD,
# NF_KEY_ALIAS, NF_KEY_PASSWORD (sem elas o APK sai com a chave de depuração).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
NATIVE_ONLY=0
for a in "$@"; do
    case "$a" in
        --native) NATIVE_ONLY=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "Opção desconhecida: $a" >&2; exit 2 ;;
    esac
done

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/opt/android-sdk}}"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/27.2.12479018}"
if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "NDK não encontrado em $NDK (ANDROID_NDK_HOME)" >&2
    exit 1
fi

SDL2_VERSION=2.30.12
SDL2_SHA256=ac356ea55e8b9dd0b2d1fa27da40ef7e238267ccf9324704850d5d47375b48ea
DEPS="$HERE/deps"
if [ ! -f "$DEPS/SDL2/CMakeLists.txt" ]; then
    mkdir -p "$DEPS"
    tarball="$DEPS/SDL2-$SDL2_VERSION.tar.gz"
    echo "==> SDL2 $SDL2_VERSION"
    curl -fsSL --retry 4 -o "$tarball" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz"
    # O arquivo é conferido pelo hash antes de ser aberto.
    if ! echo "$SDL2_SHA256  $tarball" | sha256sum -c --status; then
        echo "hash diferente em $tarball; não uso." >&2
        rm -f "$tarball"
        exit 1
    fi
    rm -rf "$DEPS/SDL2" "$DEPS/SDL2-$SDL2_VERSION"
    tar xzf "$tarball" -C "$DEPS"
    rm -f "$tarball"
    mv "$DEPS/SDL2-$SDL2_VERSION" "$DEPS/SDL2"
fi

ABI=arm64-v8a
BUILD="$HERE/build/native-$ABI"
echo "==> Jogo e SDL2 ($ABI, NDK)"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DNF_SDL2_SOURCE="$DEPS/SDL2" \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384"
cmake --build "$BUILD" --target nightfire nfload
JNI="$HERE/app/src/main/jniLibs/$ABI"
mkdir -p "$JNI"
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
# As tabelas de linha ficam em build/native-*/libmain.so (o relatório de falha dá módulo+deslocamento).
"$STRIP" --strip-debug -o "$JNI/libmain.so" "$BUILD/libmain.so"
"$STRIP" --strip-debug -o "$JNI/libnfload.so" "$BUILD/libnfload.so"
# O SDL2 e o libc++ estão dentro da libmain.so (CMakeLists.txt); as cópias de uma build antiga saem.
rm -f "$JNI/libSDL2.so" "$JNI/libc++_shared.so"
echo "bibliotecas em $JNI"
[ "$NATIVE_ONLY" = 1 ] && exit 0

echo "==> APK"
if [ ! -f "$HERE/local.properties" ]; then echo "sdk.dir=$SDK" > "$HERE/local.properties"; fi
(cd "$HERE" && ./gradlew --no-daemon :app:assembleRelease)
mkdir -p "$HERE/build/out"
cp "$HERE/app/build/outputs/apk/release/app-release.apk" "$HERE/build/out/Nightfire-android-arm64.apk"
echo "APK: android/build/out/Nightfire-android-arm64.apk"
