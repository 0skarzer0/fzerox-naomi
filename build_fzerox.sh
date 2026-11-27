#!/bin/bash
# build_fzerox.sh BASEROM.z64 [OUT.bin]
#
# F-Zero X para la NAOMI 2 (mueble del Crazy Taxi), a partir de la decompilación de N64
#   https://github.com/inspectredc/fzerox  (commit 4b00f36c, 2026-09-17, CC0)
# compilada contra KallistiOS 9926b70a con el parche NAOMI_HOST de dc2naomi (kos/kos-9926b70a-naomi_host.patch, el
# mismo de dkr-naomi). Mismo método que Mario Kart 64 (dc2naomi/homebrew/mk64/) y Diddy Kong Racing (dkr-naomi).
#
# Saca los assets de la ROM con las herramientas de la decomp (splat y Torch), compila el juego para SH-4 con la capa
# de NAOMI (port/: libultra sobre KallistiOS, el reparto de memoria de N64) y lo empaqueta con dc2naomi (perfil
# fzerox / fzerox_dbg). Estado y fases: docs/plan.md. Fase 1: arranca y llega a los menús, sin imagen ni sonido.
#
# En este repositorio no hay nada con copyright: hace falta tu ROM, F-Zero X (USA) rev0, .z64 (big-endian),
# MD5 753437d0d8ada1d12f3f9cf0f0a5171f (la que pide la decomp). Vale también el .zip que la contenga sola.
# Lo que sale de la ROM se queda en $WORK.
#
# Necesita: sh-elf GCC 14-15 en /opt/toolchains/dc/sh-elf (dc-chain de KallistiOS), binutils-mips-linux-gnu, git, python3 >= 3.10 con venv,
# ninja o make, y un compilador C/C++ del host (Torch). Pensado para Ubuntu 22.04 (WSL).
#
# Variables (todas opcionales):
#   WORK   dónde se clona y compila todo                                  (por defecto: $HOME/fzx-build)
#   KOS    árbol de KallistiOS 9926b70a ya parcheado y compilado; si no existe se clona y compila ahí
#          (vale el de dkr-naomi, $HOME/dkr-build/kos-dkr)                 (por defecto: $WORK/kos-fzx)
#   JOBS   trabajos en paralelo                                            (por defecto: 4)
#   D2N    clon de dc2naomi con los perfiles fzerox/fzerox_dbg             (por defecto: ../dc2naomi junto a este repo)
#   KOS_NAOMI_ENV environ.sh del árbol KOS_SUBARCH=naomi (loader)          (por defecto: /opt/toolchains/dc/kos-naomi/environ.sh)
#   PROFILE fzerox, o fzerox_dbg (log de KOS en pantalla y diagnóstico)  (por defecto: fzerox)
set -eo pipefail   # (sin -u: el environ.sh de KallistiOS lee variables sin definir)

ROMIN=$(realpath "${1:?uso: build_fzerox.sh BASEROM.z64 [OUT.bin]}")
HERE=$(cd "$(dirname "$0")" && pwd)
D2N=$(realpath -m "${D2N:-$HERE/../dc2naomi}")
KOS_NAOMI_ENV=${KOS_NAOMI_ENV:-/opt/toolchains/dc/kos-naomi/environ.sh}
PROFILE=${PROFILE:-fzerox}
OUT=$(realpath -m "${2:-$HERE/build/${PROFILE}_dc2naomict.bin}")
WORK=${WORK:-$HOME/fzx-build}
KOS=${KOS:-$WORK/kos-fzx}
JOBS=${JOBS:-4}

DECOMP_URL=https://github.com/inspectredc/fzerox.git
DECOMP_REV=4b00f36ceeabfe657866a7d02f3411545b5f55eb
ROM_MD5=753437d0d8ada1d12f3f9cf0f0a5171f
KOS_URL=https://github.com/KallistiOS/KallistiOS.git
KOS_REV=9926b70aeafc8398675caef98813f10d15d8a329
KOS_PATCH=$HERE/kos/kos-9926b70a-naomi_host.patch

say() { echo; echo "== $*"; }
fail() { echo "build_fzerox.sh: $*" >&2; exit 1; }

mkdir -p "$WORK"
python3 -c 'import sys, venv, ensurepip; sys.exit(sys.version_info < (3, 10))' 2>/dev/null ||
    fail "hace falta python3 >= 3.10 con venv (apt install python3-venv)"
# el Makefile de la decomp no arranca sin las binutils de MIPS, aunque la extracción no las use
command -v mips-linux-gnu-ld >/dev/null || command -v mips64-elf-ld >/dev/null ||
    fail "hacen falta las binutils de MIPS (apt install binutils-mips-linux-gnu)"

# 0. la ROM (sola o dentro de un .zip), comprobada por su MD5
ROM=$WORK/baserom.us.rev0.z64
case "$ROMIN" in
    *.zip|*.ZIP) python3 -I - "$ROMIN" "$ROM" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
roms = [n for n in z.namelist() if n.lower().endswith((".z64", ".n64", ".v64"))]
if len(roms) != 1: sys.exit("el .zip debe tener una sola ROM, tiene %d" % len(roms))
open(sys.argv[2], "wb").write(z.read(roms[0]))
PY
    ;;
    *) cp "$ROMIN" "$ROM" ;;
esac
[ "$(md5sum "$ROM" | cut -c1-32)" = $ROM_MD5 ] ||
    fail "$ROMIN no es F-Zero X (USA) rev0 en .z64 (MD5 $ROM_MD5); si es .n64/.v64, pásala antes a .z64"

# 1. KallistiOS 9926b70a con el parche NAOMI_HOST, en su propio árbol
if [ ! -f "$KOS/lib/dreamcast/libkallisti.a" ] || ! git -C "$KOS" apply -R --check "$KOS_PATCH" 2>/dev/null; then
    say "KallistiOS $KOS_REV + NAOMI_HOST en $KOS"
    [ -d "$KOS/.git" ] || git clone -q "$KOS_URL" "$KOS"
    git -C "$KOS" checkout -q -f "$KOS_REV"
    git -C "$KOS" apply "$KOS_PATCH"
    rm -f "$KOS/lib/dreamcast/libkallisti.a"
    sed -e "s#KOS_BASE=\"/opt/toolchains/dc/kos\"#KOS_BASE=\"$KOS\"#" \
        "$KOS/doc/environ.sh.sample" > "$KOS/environ.sh"
    grep -q "KOS_BASE=\"$KOS\"" "$KOS/environ.sh" || fail "no se pudo fijar KOS_BASE en $KOS/environ.sh"
    echo 'export KOS_CFLAGS="${KOS_CFLAGS} -DNAOMI_HOST"' >> "$KOS/environ.sh"
    (cd "$KOS" && source ./environ.sh && make -j"$JOBS")
fi
[ -f "$KOS/environ.sh" ] || fail "no está $KOS/environ.sh"

# 2. la decomp en su commit, sus herramientas (splat, Torch, mio0) y los assets de la ROM
D=$WORK/fzerox
[ -d "$D/.git" ] || git clone -q "$DECOMP_URL" "$D"
if [ "$(git -C "$D" rev-parse HEAD)" != $DECOMP_REV ]; then
    git -C "$D" checkout -q -f $DECOMP_REV
    rm -f "$D/.assets_ok"
fi
if [ ! -f "$D/.assets_ok" ]; then
    say "decomp: submódulos, venv, mio0 y Torch"
    git -C "$D" submodule update --init --recursive -q
    [ -x "$D/.venv/bin/python3" ] || python3 -m venv "$D/.venv"
    "$D/.venv/bin/pip" -q install -r "$D/tools/requirements-python.txt"
    # Torch usa FetchContent con OVERRIDE_FIND_PACKAGE (CMake >= 3.24; Ubuntu 22.04 trae 3.22, que lo lee mal y
    # falla con "Failed to get the hash for HEAD"): un CMake reciente dentro del venv
    "$D/.venv/bin/pip" -q install "cmake>=3.24,<4"
    export PATH=$D/.venv/bin:$PATH
    # Torch baja dependencias con git al configurar: con el DNS de WSL a veces falla; se reintenta desde cero
    for i in 1 2 3; do
        make -s -C "$D/tools" mio0 torch N_THREADS="$JOBS" && break
        [ $i = 3 ] && fail "no se pudo compilar Torch (¿red?)"
        echo "(Torch: reintento $i)"; rm -rf "$D/tools/Torch/cmake-build-release"
    done
    cp "$ROM" "$D/baserom.us.rev0.z64"
    say "decomp: make extract / make assets"
    make -C "$D" extract N_THREADS="$JOBS"
    make -C "$D" assets N_THREADS="$JOBS"
    touch "$D/.assets_ok"
fi

# 3. nuestros cambios al juego (decomp/fzerox-naomi.patch, todo bajo NAOMI_HOST); si el árbol tiene una versión
#    anterior del parche, se restaura y se vuelve a aplicar
DPATCH=$HERE/decomp/fzerox-naomi.patch
if ! git -C "$D" apply -R --check "$DPATCH" 2>/dev/null; then
    say "decomp: parche NAOMI_HOST"
    git -C "$D" checkout -q -- src include
    git -C "$D" apply "$DPATCH"
fi

# 4. el juego y la capa de NAOMI (port/Makefile): fzerox.elf, 1ST_READ.BIN, ROM.BIN y AUDIOTBL.BIN
say "el juego para SH-4"
source "$KOS/environ.sh"
B=$WORK/port-build
rm -f "$B/fallos.txt"
make -C "$HERE/port" -j"$JOBS" -k DECOMP="$D" BUILD="$B" || true
[ ! -s "$B/fallos.txt" ] || { cat "$B/fallos.txt"; fail "$(wc -l < "$B/fallos.txt") ficheros no compilan (errores: $B/warnings.log)"; }
for f in fzerox.elf 1ST_READ.BIN ROM.BIN AUDIOTBL.BIN; do
    [ -s "$B/$f" ] || fail "no se ha generado $B/$f (enlace: $B/link.log)"
done
NM="$KOS_CC_BASE/bin/$KOS_CC_PREFIX-nm"
printf "_end = 0x%s (el residente empieza en 0x8dc00000), naomi_live en 0x%s\n" \
    "$("$NM" "$B/fzerox.elf" | awk '/ _end$/{print $1}')" "$("$NM" "$B/fzerox.elf" | awk '/ _naomi_live$/{print $1}')"

# 5. el disco como ficheros (disposición "rebuild" de dc2naomi)
DISC=$WORK/disc
rm -rf "$DISC"; mkdir -p "$DISC"
[ -x "$KOS/utils/makeip/makeip" ] || make -C "$KOS/utils/makeip"
"$KOS/utils/makeip/makeip" "$HERE/ip.txt" "$DISC/IP.BIN" >/dev/null
cp "$B/1ST_READ.BIN" "$B/ROM.BIN" "$B/AUDIOTBL.BIN" "$DISC/"
# los primeros 4 KB del cartucho (cabecera + arranque): el juego lee de ahí las constantes de física de las naves
# (ROM_READ en racer.c); no se compilan, se copian tal cual de la ROM del usuario
dd if="$ROM" of="$DISC/BOOT.BIN" bs=4096 count=1 status=none
ls -la "$DISC"

# 6. la imagen de NAOMI (el loader de dc2naomi se compila con el árbol KOS_SUBARCH=naomi)
say "dc2naomi: $PROFILE -> $OUT"
[ -f "$D2N/build.sh" ] || fail "D2N=$D2N no es un clon de dc2naomi (con los perfiles fzerox/fzerox_dbg)"
[ -f "$KOS_NAOMI_ENV" ] || fail "no está $KOS_NAOMI_ENV (KallistiOS con KOS_SUBARCH=naomi, para el loader)"
mkdir -p "$(dirname "$OUT")"
(cd "$D2N" && source "$KOS_NAOMI_ENV" && REBUILD=1 ./build.sh "$PROFILE" "$DISC" "$OUT")
python3 -I - "$OUT" <<'PY'
import gzip, os, sys
src = sys.argv[1]; name = os.path.basename(src)
with open(src + ".gz", "wb") as raw, gzip.GzipFile(filename=name, mode="wb", fileobj=raw, compresslevel=6, mtime=0) as gz:
    with open(src, "rb") as f:
        while (b := f.read(1 << 20)): gz.write(b)
print(src + ".gz", os.path.getsize(src + ".gz"), "bytes")
PY
