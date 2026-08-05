#!/bin/sh

# Builds ffmpeg, or one of the libraries that ffmpeg depends on, from source as a
# static library with lto. Only the components that gpu-screen-recorder uses are enabled.
# This is run by meson when the "ffmpeg_static" option is enabled.

set -eu

if [ $# -ne 5 ]; then
    echo "usage: $0 <library> <source-dir> <build-dir> <install-prefix> <c-compiler>" >&2
    exit 1
fi

library=$1
source_dir=$2
build_dir=$3
prefix=$4
compiler=$5
log_file=$build_dir/build.log
stamp_file=$prefix/$library.stamp
jobs=$(nproc 2>/dev/null || echo 4)

export CC=$compiler
export PKG_CONFIG_PATH=$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}

require_program() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: $1 is required to build $library from source" >&2
        exit 1
    fi
}

require_program pkg-config
require_program make

case $library in
    nv-codec-headers)
        set -- PREFIX="$prefix"
        build() {
            make -C "$source_dir" "$@" install
        }
        ;;
    x264)
        require_program nasm
        set -- --prefix="$prefix" \
            --enable-static \
            --enable-pic \
            --enable-lto \
            --disable-cli \
            --disable-opencl
        build() {
            "$source_dir/configure" "$@"
            make -j"$jobs"
            make install
        }
        ;;
    opus)
        set -- --prefix="$prefix" \
            --disable-shared \
            --enable-static \
            --with-pic \
            --disable-doc \
            --disable-extra-programs \
            CFLAGS="-O3 -flto -fPIC" \
            LDFLAGS="-flto"
        build() {
            "$source_dir/configure" "$@"
            make -j"$jobs"
            make install
        }
        ;;
    openssl)
        require_program perl
        # The certificate directory of the system openssl is used so that certificate
        # verification keeps working the same way as it does with the system openssl.
        openssl_dir=$(openssl version -d 2>/dev/null | sed -n 's/^OPENSSLDIR: "\(.*\)"$/\1/p')
        if [ -z "$openssl_dir" ]; then
            openssl_dir=/etc/ssl
        fi
        set -- --prefix="$prefix" \
            --libdir=lib \
            --openssldir="$openssl_dir" \
            no-shared \
            no-apps \
            no-docs \
            no-tests \
            no-legacy \
            no-engine \
            no-comp \
            no-quic \
            no-ssl3 \
            no-weak-ssl-ciphers \
            no-cms \
            no-ct \
            no-ts \
            no-ocsp \
            no-srp \
            no-psk \
            no-dsa \
            no-ec2m \
            no-gost \
            no-idea \
            no-md2 \
            no-md4 \
            no-mdc2 \
            no-rc2 \
            no-rc4 \
            no-rc5 \
            no-bf \
            no-cast \
            no-seed \
            no-camellia \
            no-whirlpool \
            no-rmd160 \
            no-sm2 \
            no-sm3 \
            no-sm4 \
            no-siphash \
            -O3 -flto -fPIC
        build() {
            "$source_dir/Configure" "$@"
            make -j"$jobs"
            make install_sw
        }
        ;;
    srt)
        require_program cmake
        set -- -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$prefix" \
            -DCMAKE_INSTALL_LIBDIR=lib \
            -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
            -DENABLE_SHARED=OFF \
            -DENABLE_STATIC=ON \
            -DENABLE_APPS=OFF \
            -DENABLE_EXAMPLES=OFF \
            -DENABLE_TESTING=OFF \
            -DENABLE_UNITTESTS=OFF \
            -DUSE_ENCLIB=openssl \
            -DOPENSSL_ROOT_DIR="$prefix" \
            -DOPENSSL_USE_STATIC_LIBS=ON
        build() {
            cmake -S "$source_dir" -B "$build_dir" "$@"
            cmake --build "$build_dir" -j"$jobs"
            cmake --install "$build_dir"
        }
        ;;
    ffmpeg)
        require_program nasm

        encoders=aac,flac,libx264,libopus,h264_nvenc,hevc_nvenc,av1_nvenc,h264_vaapi,hevc_vaapi,vp8_vaapi,vp9_vaapi,h264_vulkan,hevc_vulkan
        muxers=mp4,mov,matroska,webm,flv,mpegts,hls,whip
        protocols=file,pipe,tcp,udp,http,https,tls,rtmp,rtmps,libsrt
        filters=abuffer,abuffersink,amix,aresample,aformat,anull

        if pkg-config --atleast-version=1.16 libva; then
            encoders=$encoders,av1_vaapi
        else
            echo "warning: libva is too old for av1 vaapi encoding" >&2
        fi

        if pkg-config --atleast-version=1.4.317 vulkan; then
            encoders=$encoders,av1_vulkan
        else
            echo "warning: the vulkan headers are too old for av1 vulkan encoding" >&2
        fi

        # ffmpeg ignores $CC and the same compiler as gpu-screen-recorder has to be
        # used because lto objects are not compatible between compilers.
        set -- --prefix="$prefix" \
            --cc="$compiler" \
            --pkg-config-flags=--static \
            --disable-debug \
            --disable-everything \
            --disable-autodetect \
            --disable-programs \
            --disable-doc \
            --disable-avdevice \
            --disable-swscale \
            --disable-shared \
            --enable-static \
            --enable-pic \
            --enable-lto \
            --enable-gpl \
            --enable-version3 \
            --enable-network \
            --enable-encoder="$encoders" \
            --enable-muxer="$muxers" \
            --enable-protocol="$protocols" \
            --enable-filter="$filters" \
            --enable-bsf=extract_extradata \
            --enable-libx264 \
            --enable-libopus \
            --enable-libsrt \
            --enable-openssl \
            --enable-ffnvcodec \
            --enable-nvenc \
            --enable-cuda \
            --enable-vaapi \
            --enable-vulkan
        build() {
            "$source_dir/configure" "$@"
            make -j"$jobs"
            make install
        }
        ;;
    *)
        echo "error: unknown library $library" >&2
        exit 1
        ;;
esac

# Rebuilding takes a long time, only do it when the source or the configuration changed.
stamp="$source_dir $compiler $*"
if [ -f "$stamp_file" ] && [ "$(cat "$stamp_file")" = "$stamp" ]; then
    exit 0
fi

rm -rf "$build_dir"
mkdir -p "$build_dir"

echo "Building $library, this takes a while. Output is written to $log_file" >&2

(
    cd "$build_dir"
    build "$@"
) >"$log_file" 2>&1 || {
    echo "error: failed to build $library, see $log_file for details:" >&2
    tail -n 30 "$log_file" >&2
    exit 1
}

printf '%s' "$stamp" > "$stamp_file"
