#!/bin/zsh
# Builds the static FFmpeg and dav1d that `make package` links into a plugin that runs on any
# Apple silicon Mac with macOS 13 or later, without Homebrew. Only what the importer uses is
# enabled (AV1/VP9 video, the audio found next to it, Matroska/WebM/MP4), all LGPL.
# Usage: tools/build-deps.sh   (sources are downloaded into build/deps/src if missing)
set -euo pipefail
cd "${0:A:h}/.."

FFMPEG=ffmpeg-9.0.2
DAV1D=dav1d-1.5.4
MIN_MACOS=13.0
deps=$PWD/build/deps
prefix=$deps/out
mkdir -p $deps/src
cd $deps/src

[[ -f $FFMPEG.tar.xz ]] || curl -fSLO https://ffmpeg.org/releases/$FFMPEG.tar.xz
[[ -f $DAV1D.tar.xz ]] || curl -fSLO https://downloads.videolan.org/pub/videolan/dav1d/${DAV1D#dav1d-}/$DAV1D.tar.xz
rm -rf $FFMPEG $DAV1D
tar xf $FFMPEG.tar.xz
tar xf $DAV1D.tar.xz

flags="-mmacosx-version-min=$MIN_MACOS -arch arm64"

# Software AV1 for Macs whose media engine has no AV1 decoder (M1, M2).
(cd $DAV1D &&
    meson setup build --prefix=$prefix --libdir=lib --buildtype=release --default-library=static \
        -Denable_tools=false -Denable_tests=false -Dc_args="$flags" -Dc_link_args="$flags" &&
    ninja -C build install)

(cd $FFMPEG &&
    PKG_CONFIG_PATH=$prefix/lib/pkgconfig ./configure --prefix=$prefix \
        --enable-static --disable-shared --disable-programs --disable-doc --disable-network \
        --disable-avdevice --disable-avfilter --disable-iconv --disable-lzma --disable-securetransport \
        --disable-sdl2 --disable-xlib --disable-libxcb --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=av1,vp9,libdav1d,opus,aac,vorbis,mp3,flac,ac3,eac3,alac \
        --enable-decoder=pcm_s16le,pcm_s24le,pcm_f32le,pcm_s16be,pcm_s24be \
        --enable-parser=av1,vp9,opus,aac,vorbis,mpegaudio,flac,ac3 \
        --enable-videotoolbox --enable-hwaccel=av1_videotoolbox,vp9_videotoolbox --enable-libdav1d \
        --enable-swscale --enable-swresample \
        --extra-cflags="$flags" --extra-ldflags="$flags" &&
    make -j"$(sysctl -n hw.ncpu)" install)

echo "built into $prefix"
