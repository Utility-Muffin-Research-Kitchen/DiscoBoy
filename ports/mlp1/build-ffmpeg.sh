#!/bin/bash
# Configure and build Disco Boy's bundled FFmpeg. Runs INSIDE the mlp1-toolchain
# image; scripts/build-mlp1-ffmpeg.sh mounts the pinned tarball at
# /src/ffmpeg.tar.xz and the output directory at /out.
#
# LGPL-2.1+ only (no --enable-gpl / --enable-nonfree), shared libraries, and
# nothing autodetected from the sysroot. Only the containers and decoders that
# disco_ff_handles() routes to FFmpeg are enabled; MP3, FLAC, WAV and Ogg Vorbis
# audio is decoded by the single-header decoders. Those formats still reach
# FFmpeg's demuxers for embedded cover art (disco_ff_cover), whose JPEG/PNG
# stream avformat_find_stream_info probes with the mjpeg and png decoders.
set -euo pipefail

TC=/opt/mlp1-toolchain
export PATH="$TC/bin:$PATH"
SYSROOT="$TC/aarch64-buildroot-linux-gnu/sysroot"
CROSS=aarch64-buildroot-linux-gnu-
WORK=/tmp/ffmpeg-build
LIBS=(libavformat.so.58 libavcodec.so.58 libavutil.so.56 libswresample.so.3)

demuxers=(mov aac ogg asf aiff ape wv matroska tta ac3 eac3 flac mp3 wav)
decoders=(
  aac aac_latm alac opus vorbis flac mp3float mp2float ac3 eac3
  wmav1 wmav2 wmapro wmalossless ape wavpack tta adpcm_ima_qt
  pcm_s8 pcm_u8 pcm_s16be pcm_s16le pcm_s24be pcm_s24le pcm_s32be pcm_s32le
  pcm_f32be pcm_f32le pcm_f64be pcm_f64le pcm_alaw pcm_mulaw mjpeg png
)
parsers=(aac aac_latm ac3 flac mpegaudio opus vorbis mjpeg png)

join() { local IFS=,; echo "$*"; }

rm -rf "$WORK"
mkdir -p "$WORK"
tar -xJf /src/ffmpeg.tar.xz -C "$WORK" --strip-components=1
cd "$WORK"

configure_args=(
  --prefix=/usr
  --enable-cross-compile --arch=aarch64 --cpu=cortex-a55 --target-os=linux
  --cross-prefix="$CROSS" --sysroot="$SYSROOT" --pkg-config=pkg-config
  --enable-shared --disable-static --enable-pic
  --disable-autodetect --disable-everything
  --disable-programs --disable-doc --disable-debug
  --disable-avdevice --disable-avfilter --disable-postproc --disable-swscale
  --disable-network
  --enable-pthreads
  --enable-avformat --enable-avcodec --enable-swresample --enable-zlib
  --enable-protocol=file
  --enable-demuxer="$(join "${demuxers[@]}")"
  --enable-decoder="$(join "${decoders[@]}")"
  --enable-parser="$(join "${parsers[@]}")"
)
./configure "${configure_args[@]}" >/tmp/ff_configure.log 2>&1 \
  || { tail -40 /tmp/ff_configure.log; tail -30 ffbuild/config.log; exit 1; }
# A misspelt component is only a configure warning; make it fatal.
if grep -i "did not match anything" /tmp/ff_configure.log; then exit 1; fi
# --disable-autodetect also turns threads off unless asked for. Without them
# FFmpeg's internal locks and once-initialisers are no-ops, and Disco Boy calls
# it from the playback thread and the tag scan at the same time.
grep -q "^#define HAVE_PTHREADS 1$" config.h || { echo "pthreads are not enabled" >&2; exit 1; }
# Each library carries RUNPATH $ORIGIN so it finds its siblings in the pak's
# lib/ before the firmware's /usr/lib. Appended to config.mak rather than passed
# as --extra-ldsoflags, which configure re-evaluates; library.mak expands the
# link rule twice (define + eval), hence four dollars for one.
cat >>ffbuild/config.mak <<'MAK'
LDSOFLAGS += -Wl,-rpath,'$$$$ORIGIN' -Wl,--enable-new-dtags
MAK
make -j"$(nproc)" >/tmp/ff_make.log 2>&1 || { tail -60 /tmp/ff_make.log; exit 1; }
make install DESTDIR="$WORK/stage" >/tmp/ff_install.log 2>&1 \
  || { tail -20 /tmp/ff_install.log; exit 1; }

# FAT32 has no symlinks: ship each library once, under its SONAME.
mkdir -p /out/lib /out/licenses
for lib in "${LIBS[@]}"; do
  cp -L "$WORK/stage/usr/lib/$lib" "/out/lib/$lib"
  "${CROSS}strip" --strip-unneeded "/out/lib/$lib"
  runpath="$("${CROSS}readelf" -d "/out/lib/$lib" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"
  [ "$runpath" = '$ORIGIN' ] || { echo "$lib RUNPATH is '$runpath', want \$ORIGIN" >&2; exit 1; }
done
cp COPYING.LGPLv2.1 /out/licenses/COPYING.LGPLv2.1
{
  echo "FFmpeg $FFMPEG_VERSION, built for Disco Boy (LGPL-2.1-or-later)."
  echo "Source: $FFMPEG_URL"
  echo "SHA-256: $FFMPEG_SHA256"
  echo "Build script: https://github.com/Utility-Muffin-Research-Kitchen/DiscoBoy/blob/main/ports/mlp1/build-ffmpeg.sh"
  echo "Configure:"
  sed -n 's/^#define FFMPEG_CONFIGURATION "\(.*\)"$/\1/p' config.h
} >/out/licenses/FFMPEG-SOURCE.txt
ls -l /out/lib
