#!/bin/sh
# FFmpeg minimal pour les FMV de THUG sur Vita : decodeurs Bink 1 video
# (binkvideo) + audio (binkaudio_dct/rdft) + demuxeur bink. Rien d'autre.
# Installe dans un prefixe A PART (pas $VITASDK) pour ne pas ecraser le paquet
# vdpm ffmpeg (qui n'a pas Bink).
#
# Licence : FFmpeg LGPL 2.1+ (aucune option GPL/nonfree activee, verifie par
# la ligne "License:" de configure). Edition de liens STATIQUE : voir le
# rapport pour les obligations (sources + relinkabilite).
#
# Usage : build_ffmpeg_bink.sh <src ffmpeg-7.1.1> <dossier build> <prefixe>
# Mesure 2026-10-10 : configure + make -j10 ~1 min sur M5 ; un seul patch (tx).
set -e
SRC=$1; BUILD=$2; PREFIX=$3
: "${VITASDK:?VITASDK non defini}"
export PATH="$VITASDK/bin:$PATH"
# Patch obligatoire : libavutil/tx tire sinon 16,7 Mo de tables de cosinus en
# BSS (toutes les tailles jusqu'a 2^21, float/double/int32). Bink audio ne
# depasse pas 2048 points : on plafonne a 8192. BSS 17,1 Mo -> 0,4 Mo, sortie
# audio identique au bit pres (md5 verifie sur ATVI.bik).
HERE=$(cd "$(dirname "$0")" && pwd)
if ! grep -q "Plafond THUG" "$SRC/libavutil/tx_template.c"; then
  patch -d "$SRC" -p0 < "$HERE/tx_template_cap8192.patch"
  echo "/* Plafond THUG : tables FFT <= 8192 */" >> "$SRC/libavutil/tx_template.c"
fi
mkdir -p "$BUILD" && cd "$BUILD"
"$SRC/configure" --prefix="$PREFIX" \
  --enable-cross-compile --cross-prefix=arm-vita-eabi- \
  --arch=armv7-a --cpu=cortex-a9 --target-os=none \
  --disable-runtime-cpudetect --disable-armv5te --disable-armv6t2 --enable-neon \
  --disable-everything \
  --enable-decoder=bink,binkaudio_dct,binkaudio_rdft \
  --enable-demuxer=bink \
  --disable-programs --disable-doc --disable-network --disable-autodetect \
  --enable-static --disable-shared \
  --disable-avdevice --disable-swscale --disable-swresample --disable-avfilter \
  --disable-debug --enable-pthreads \
  --extra-cflags="-O2 -mfpu=neon -mfloat-abi=hard -D_BSD_SOURCE \
    -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types \
    -Wno-error=int-conversion"
grep "^License" ffbuild/config.log 2>/dev/null || true
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
make install
# Resultat : libavformat.a (~260 Ko), libavcodec.a (~400 Ko), libavutil.a (~760 Ko) ;
# dans l ELF : ~770 Ko de code (newlib compris), 0,4 Mo de BSS.
# Liens : -lavformat -lavcodec -lavutil -lpthread -lm
