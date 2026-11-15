#!/bin/bash
# Builds the seven FFmpeg libraries GucciBot's renderer loads on macOS, as
# universal (arm64 + x86_64) dylibs for macOS 11 and later.
#
# Run by .github/workflows/ffmpeg-mac.yml on a macOS arm64 runner, one stage
# at a time:
#   mac/build.sh fetch            sources, each pinned by commit and checked
#   mac/build.sh build arm64      dav1d + x264 (static) and FFmpeg (shared)
#   mac/build.sh build x86_64     the same, cross-compiled
#   mac/build.sh universal        lipo, install names, strip, ad hoc signing
#   mac/build.sh check            otool / codesign / minimum macOS checks
#   mac/build.sh package          ffmpeg-libs-mac.zip + SHA256SUMS.txt
#
# FFmpeg is the same source as the Windows DLLs in release ffmpeg-n8.0.1:
# release/8.0 at 15504610b0 (git describe: n8.0.1-64-g15504610b0).
set -euo pipefail

FFMPEG_URL=https://github.com/FFmpeg/FFmpeg.git           # FFmpeg's official GitHub mirror
FFMPEG_SHA=15504610b0dc12c56e5e9f94ff06c873382368f5
FFMPEG_DESCRIBE=n8.0.1-64-g15504610b0
X264_URL=https://code.videolan.org/videolan/x264.git
X264_SHA=b35605ace3ddf7c1a5d67a2eb553f034aef41d55           # branch "stable", 2025-06-08
DAV1D_URL=https://code.videolan.org/videolan/dav1d.git
DAV1D_TAG=1.5.4
DAV1D_SHA=54706fc6bc0cdecab7e9593974a4039cc038fca7
MIN_MACOS=11.0

# Same names and major versions as the Windows DLLs, in dependency order.
LIBS="avutil.60 swresample.6 swscale.9 avcodec.62 avformat.62 avfilter.11 avdevice.62"

ROOT=${GITHUB_WORKSPACE:-$PWD}
W=$ROOT/work
OUT=$W/out
export MACOSX_DEPLOYMENT_TARGET=$MIN_MACOS
NCPU=$(sysctl -n hw.ncpu)

fetch() {
    mkdir -p "$W/src" && cd "$W/src"

    rm -rf ffmpeg && git init -q ffmpeg
    # Deep enough to reach the n8.0.1 tag, so the version string is the
    # real git describe, the same one the Windows DLLs carry.
    git -C ffmpeg fetch -q --depth 80 "$FFMPEG_URL" "$FFMPEG_SHA" "refs/tags/n8.0.1:refs/tags/n8.0.1"
    git -C ffmpeg checkout -q "$FFMPEG_SHA"
    # A shallow clone abbreviates hashes to 7 digits; FFmpeg's full history
    # gives 10 (the Windows DLLs say ...-g15504610b0). FFmpeg's version.sh
    # runs git describe in this folder, so it picks this up too.
    git -C ffmpeg config core.abbrev 10
    test "$(git -C ffmpeg rev-parse HEAD)" = "$FFMPEG_SHA"
    got=$(git -C ffmpeg describe --tags --always)
    echo "FFmpeg: $(git -C ffmpeg rev-parse HEAD) ($got)"
    test "$got" = "$FFMPEG_DESCRIBE"

    rm -rf x264 && git clone -q "$X264_URL" x264
    git -C x264 checkout -q "$X264_SHA"
    test "$(git -C x264 rev-parse HEAD)" = "$X264_SHA"
    echo "x264: $(git -C x264 rev-parse HEAD) ($(git -C x264 log -1 --format='%cs %s'))"

    rm -rf dav1d && git clone -q --depth 1 --branch "$DAV1D_TAG" "$DAV1D_URL" dav1d
    test "$(git -C dav1d rev-parse HEAD)" = "$DAV1D_SHA"
    echo "dav1d: $(git -C dav1d rev-parse HEAD) (tag $DAV1D_TAG)"
}

# What GucciBot uses (src/render/renderer.cpp, video_decoder.cpp,
# trainers/videomode.cpp), plus the common formats people type into the
# Render page's free-text codec / file type boxes and the videos Video Mode
# is given (.mp4 .mkv .mov .webm .avi from the file picker).
ffmpeg_components() {
    local enc_v="libx264,libx264rgb,h264_videotoolbox,hevc_videotoolbox,prores_videotoolbox,mpeg4,ffv1,utvideo,prores_ks,png,mjpeg,rawvideo,gif"
    local enc_a="aac,aac_at,alac,alac_at,flac,ac3,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le"
    local dec_v="h264,hevc,vp8,vp9,libdav1d,mpeg4,h263,h263p,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmv1,wmv2,wmv3,vc1,mpeg1video,mpeg2video,mjpeg,prores,dnxhd,ffv1,utvideo,huffyuv,ffvhuff,png,apng,gif,webp,rawvideo,qtrle,theora,vp6,vp6a,vp6f,flv,cinepak,svq3,dvvideo,cfhd,magicyuv"
    local dec_a="aac,aac_fixed,aac_latm,aac_at,mp3,mp3float,mp2,mp2float,opus,vorbis,flac,alac,ac3,eac3,dca,wmav1,wmav2,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8,pcm_alaw,pcm_mulaw"
    local demux="mov,matroska,avi,flv,mpegts,mpegps,asf,ogg,mp3,aac,wav,flac,h264,hevc,m4v,ivf,obu,gif,mpegvideo,ac3,eac3,dts"
    local mux="mp4,mov,matroska,webm,avi,mpegts,flv,wav,flac,adts,ogg"
    local parse="h264,hevc,vp8,vp9,av1,aac,aac_latm,mpegaudio,opus,vorbis,flac,mpeg4video,mpegvideo,mjpeg,ac3,png,gif,dca,vc1,h263,webp,dnxhd"
    local filt="buffer,buffersink,abuffer,abuffersink,null,anull,format,aformat,scale,aresample,colorspace,fps,setpts,asetpts,copy,vflip,hflip,transpose,crop,pad,trim,atrim,volume,amix"
    echo --disable-everything \
        --enable-encoder="$enc_v,$enc_a" --enable-decoder="$dec_v,$dec_a" \
        --enable-demuxer="$demux" --enable-muxer="$mux" --enable-parser="$parse" \
        --enable-bsfs --enable-protocol=file,pipe --enable-filter="$filt"
}

build() {
    local A=$1 HOST CPU FAM
    case "$A" in
        arm64) HOST=aarch64-apple-darwin; CPU=arm64; FAM=aarch64 ;;
        x86_64) HOST=x86_64-apple-darwin; CPU=x86_64; FAM=x86_64 ;;
        *) echo "unknown arch $A"; exit 1 ;;
    esac
    local P=$W/prefix/$A B=$W/build/$A
    local CF="-arch $A -mmacosx-version-min=$MIN_MACOS"
    mkdir -p "$P" "$B"
    # Only our own prefix: nothing Homebrew has installed may get linked in.
    export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig
    unset PKG_CONFIG_PATH || true

    echo "::group::dav1d $A"
    cat > "$B/cross.txt" <<EOF
[binaries]
c = ['clang', '-arch', '$A']
cpp = ['clang++', '-arch', '$A']
ar = 'ar'
strip = 'strip'
nasm = 'nasm'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-mmacosx-version-min=$MIN_MACOS']
c_link_args = ['-mmacosx-version-min=$MIN_MACOS']

[host_machine]
system = 'darwin'
cpu_family = '$FAM'
cpu = '$CPU'
endian = 'little'
EOF
    meson setup "$B/dav1d" "$W/src/dav1d" --cross-file "$B/cross.txt" \
        --buildtype=release --default-library=static --prefix="$P" --libdir=lib \
        -Denable_tools=false -Denable_tests=false -Denable_examples=false
    ninja -C "$B/dav1d" install
    echo "::endgroup::"

    echo "::group::x264 $A"
    mkdir -p "$B/x264" && cd "$B/x264"
    CC=clang "$W/src/x264/configure" --prefix="$P" --host="$HOST" \
        --enable-static --enable-pic --disable-cli \
        --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash \
        --extra-cflags="$CF" --extra-ldflags="$CF" --extra-asflags="$([ "$A" = arm64 ] && echo "$CF")"
    make -j"$NCPU"
    make install-lib-static
    echo "::endgroup::"

    echo "::group::FFmpeg $A"
    mkdir -p "$B/ffmpeg" && cd "$B/ffmpeg"
    local cross=()
    if [ "$A" = x86_64 ]; then cross=(--enable-cross-compile --x86asmexe=nasm); fi
    # shellcheck disable=SC2046
    "$W/src/ffmpeg/configure" --prefix="$P" --arch="$A" --target-os=darwin \
        --cc=clang --cxx=clang++ --objcc=clang \
        --extra-cflags="$CF" --extra-objcflags="$CF" \
        --extra-ldflags="$CF -Wl,-headerpad_max_install_names" \
        --install-name-dir=@loader_path \
        --pkg-config=pkg-config --pkg-config-flags=--static \
        --enable-gpl --enable-version3 \
        --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-debug --disable-network \
        --disable-autodetect --enable-pthreads \
        --enable-videotoolbox --enable-audiotoolbox \
        --enable-zlib --enable-bzlib \
        --enable-libx264 --enable-libdav1d \
        $(ffmpeg_components) \
        ${cross[@]+"${cross[@]}"} || { tail -n 60 ffbuild/config.log; exit 1; }
    make -j"$NCPU"
    make install
    echo "::endgroup::"
    ls -l "$P/lib"
}

universal() {
    rm -rf "$OUT" && mkdir -p "$OUT"
    for n in $LIBS; do
        local f=lib$n.dylib
        lipo -create "$W/prefix/arm64/lib/$f" "$W/prefix/x86_64/lib/$f" -output "$OUT/$f"
        chmod 755 "$OUT/$f"
    done
    for n in $LIBS; do
        local f=$OUT/lib$n.dylib
        install_name_tool -id "@loader_path/lib$n.dylib" "$f"
        # Every reference to another of the seven goes through @loader_path
        # (configure's --install-name-dir already does this; this makes sure).
        for dep in $(otool -arch all -L "$f" | awk '/^[[:space:]]/ && /libav|libsw/ {print $1}' | sort -u); do
            local base
            base=$(basename "$dep")
            if [ "$dep" != "@loader_path/$base" ]; then
                install_name_tool -change "$dep" "@loader_path/$base" "$f"
            fi
        done
        strip -x "$f"
        codesign -f -s - "$f"
        codesign -v --strict "$f"
    done
    ls -l "$OUT"
}

check() {
    local bad=0
    for n in $LIBS; do
        local f=$OUT/lib$n.dylib
        echo "=== lib$n.dylib ($(stat -f %z "$f") bytes)"
        lipo -info "$f"
        echo "install name: $(otool -D "$f" | tail -n 1)"
        [ "$(otool -D "$f" | tail -n 1)" = "@loader_path/lib$n.dylib" ] || { echo "BAD install name"; bad=1; }
        otool -arch all -L "$f"
        while read -r dep; do
            case "$dep" in
                @loader_path/libavutil.60.dylib|@loader_path/libswresample.6.dylib|@loader_path/libswscale.9.dylib|\
                @loader_path/libavcodec.62.dylib|@loader_path/libavformat.62.dylib|@loader_path/libavfilter.11.dylib|\
                @loader_path/libavdevice.62.dylib) ;;
                /usr/lib/*|/System/Library/Frameworks/*) ;;
                *) echo "BAD dependency: $dep"; bad=1 ;;
            esac
        done < <(otool -arch all -L "$f" | awk '/^[[:space:]]/ {print $1}')
        for a in arm64 x86_64; do
            local minos
            minos=$(otool -arch "$a" -l "$f" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')
            echo "$a minos: $minos"
            [ "$minos" = "$MIN_MACOS" ] || { echo "BAD minos"; bad=1; }
        done
        codesign -v --strict --verbose=2 "$f" || bad=1
        codesign -dv "$f" 2>&1 | grep -E 'Signature=|Identifier=|Format=' || true
    done
    [ "$bad" = 0 ] && echo "ALL CHECKS PASSED" || { echo "CHECKS FAILED"; exit 1; }
}

package() {
    cd "$OUT"
    rm -f "$W/ffmpeg-libs-mac.zip"
    zip -9 -X "$W/ffmpeg-libs-mac.zip" $(for n in $LIBS; do echo "lib$n.dylib"; done)
    cd "$W"
    { (cd "$OUT" && shasum -a 256 lib*.dylib); shasum -a 256 ffmpeg-libs-mac.zip; } > SHA256SUMS.txt
    cat SHA256SUMS.txt
    echo "zip size: $(stat -f %z ffmpeg-libs-mac.zip) bytes"
    unzip -l ffmpeg-libs-mac.zip
}

"$@"
