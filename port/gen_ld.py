#!/usr/bin/env python3
"""gen_ld.py KOS_SHLELF.xc DECOMP_FZEROX.ld AUDIO_TABLE.bin SALIDA.ld SIMBOLOS.ld...

Hace el script de enlazado del port: el de KallistiOS (shlelf.xc) con F-Zero X dentro, sacado casi tal cual del
script que genera splat para la decomp (linker_scripts/us/rev0/fzerox.ld). El juego reparte la RAM de la N64 a mano
(sys_gfx.c pone los assets "detrás de buffers", segment.c sus arenas en direcciones fijas, los overlays comparten
dirección), así que se reproduce la N64:

- Una ventana de RDRAM (__fzx_n64, 4 MB y un poco) dentro de nuestra RAM. Cada segmento de código y datos va en
  su dirección de N64 desplazada a la ventana (0x80xxxxxx -> __fzx_n64 + 0xxxxxx); los overlays se solapan, como
  en N64. Su BSS es NOLOAD y la ventana no va en el binario: main() la pone a cero.
- Una "ROM" (__fzx_rom_start..__fzx_rom_end) con los bytes (LMA) de todos los segmentos en el orden de la ROM de
  N64: código, datos y assets. build_fzerox.sh la saca a ROM.BIN y main() la lee del disco; luego copia "entry" y
  "main" a la ventana, como el arranque de N64. Los overlays los copia el propio juego con su DMA (ultra.c).
- Los assets llevan su dirección segmentada de N64 como VMA (0x0A000000...): los punteros entre assets y
  SEGMENTED_TO_VIRTUAL funcionan sin tocarlos.
- audio_table (las muestras, 10,7 MB) no va en ROM.BIN: un hueco NOLOAD del tamaño del .bin que main() llena con
  AUDIOTBL.BIN.
- Las direcciones segmentadas sueltas de undefined_syms*.ld (D_A000000...) como PROVIDE.
- En SH-ELF los símbolos de C llevan "_" delante (en MIPS no): se añade a todo lo que ve el C.
"""
import os
import re
import sys

SKIP = {"header", "boot", "F67900"}        # cabecera de la ROM, IPL3 y un trozo sin usar
WINDOW = 0x410000                           # 4 MB de RDRAM + leo_font (0x80400000..0x80403010)
INPUT_RE = re.compile(r"^\s*build/us/rev0/(\S+?\.o)\((\.\w+)\);\s*$")
# los objetos del juego llevan las secciones renombradas a .fzxg.* (port/Makefile)
KINDS = {".text": ".fzxg.text .fzxg.text.*", ".data": ".fzxg.data .fzxg.data.*",
         ".rodata": ".fzxg.rodata .fzxg.rodata.*", ".bss": ".fzxg.bss .fzxg.bss.*"}
SEG_SYM = re.compile(r"\b(\w+_(?:ROM_START|ROM_END|VRAM|VRAM_END|TEXT_START|TEXT_END|TEXT_SIZE|DATA_START|"
                     r"DATA_END|DATA_SIZE|RODATA_START|RODATA_END|RODATA_SIZE|BSS_START|BSS_END|BSS_SIZE))\b")


def parse_segments(lines):
    """[(nombre, [líneas del bloque])] en el orden del script"""
    segs, cur, name = [], None, None
    for ln in lines:
        m = re.match(r"^    (\w+)_ROM_START = __romPos;", ln)
        if m and cur is None:
            name, cur = m.group(1), []
        if cur is not None:
            cur.append(ln.rstrip("\n"))
            if re.match(r"^    %s_VRAM_END = \.;" % re.escape(name), ln):
                segs.append((name, cur))
                cur = None
    return segs


def convert_input(ln):
    m = INPUT_RE.match(ln)
    if not m:
        return ln
    path, kind = m.groups()
    if not path.startswith("src/"):
        return None                      # ensamblador y .bin de la decomp: lo da el port
    return "        KEEP(*/%s(%s))" % (path, KINDS.get(kind, kind))


def shift_vma(expr):
    """0x80xxxxxx (RAM de N64) -> en la ventana; lo demás (segmentado, 0x8F000000, expresiones) igual"""
    def rep(m):
        v = int(m.group(0), 16)
        if 0x80000000 <= v < 0x80000000 + WINDOW:
            return "(__fzx_n64 + 0x%X)" % (v - 0x80000000)
        return m.group(0)
    return re.sub(r"0x[0-9A-Fa-f]+", rep, expr)


def convert_block(name, block):
    out, i = [], 0
    while i < len(block):
        ln = block[i]
        m = re.match(r"^    \.(\w+) (.*?)(: AT\(\w+\) SUBALIGN\(16\)|\(NOLOAD\) : SUBALIGN\(16\)|: SUBALIGN\(16\))$", ln)
        if m:
            sec, vma, rest = m.groups()
            vma = shift_vma(vma.strip())
            if rest.startswith(": AT("):
                rest = ": AT(%s_ROM_START) SUBALIGN(16)" % name
            out.append("    .fzx.%s %s %s" % (sec, vma, rest) if vma else "    .fzx.%s %s" % (sec, rest))
            assert block[i + 1].strip() == "{", block[i + 1]
            out.append("    {")
            i += 2
            while block[i].strip() != "}":
                c = convert_input(block[i])
                if c is not None:
                    out.append(c)
                i += 1
            out.append("    }")
            i += 1
            continue
        ln = ln.replace("__romPos", "__fzx_romPos")
        ln = re.sub(r"(ADDR|SIZEOF)\(\.(\w+)\)", r"\1(.fzx.\2)", ln)
        out.append(ln)
        i += 1
    return out


def main():
    kos_ld, decomp_ld, audio_bin, out = sys.argv[1:5]
    syms_lds = sys.argv[5:]
    segs = parse_segments(open(decomp_ld).readlines())
    audio_size = os.path.getsize(audio_bin)

    ins = ["  /* ---- F-Zero X (port/gen_ld.py): ventana de RDRAM de N64 ---- */",
           "  . = ALIGN(0x10000);",
           "  __fzx_n64 = .;",
           "  . = __fzx_n64 + 0x%X;" % WINDOW,
           "  __fzx_n64_end = .;",
           "  /* ---- ROM: los segmentos de N64 en su orden (LMA aquí, VMA en la ventana o segmentada) ---- */",
           "  . = ALIGN(32);",
           "  __fzx_rom_start = .;",
           "  __fzx_romPos = .;"]
    nseg = 0
    for name, block in segs:
        if name in SKIP or name == "audio_table":
            continue
        ins += convert_block(name, block)
        nseg += 1
    ins += ["  __fzx_rom_end = __fzx_romPos;",
            "  . = __fzx_romPos;",
            "  /* ---- hueco de audio_table, lo llena main() con AUDIOTBL.BIN ---- */",
            "  .fzxaudio (NOLOAD) : ALIGN(32)\n  {",
            "    audio_table_ROM_START = .;",
            "    audio_table_VRAM = .;",
            "    . += 0x%X;" % audio_size,
            "    audio_table_ROM_END = .;",
            "    audio_table_VRAM_END = .;",
            "  }",
            "  ASSERT(__fzx_rom_start >= __fzx_n64_end, \"gen_ld.py: la ROM pisa la ventana de RDRAM\")"]

    provides, seen = [], set()
    for syms_ld in syms_lds:
        for ln in open(syms_ld):
            m = re.match(r"^(\w+) = (0x[0-9A-Fa-f]+);", ln)
            if m and int(m.group(2), 16) < 0x10000000 and m.group(1) not in seen:
                seen.add(m.group(1))
                provides.append("PROVIDE(_%s = %s);" % m.groups())

    ins = [SEG_SYM.sub(r"_\1", l) for l in ins]
    for s in ("__fzx_rom_start", "__fzx_rom_end", "__fzx_n64_end", "__fzx_n64"):
        ins = [re.sub(r"(?<![\w])%s(?![\w])" % s, "_" + s, l) for l in ins]

    kos = open(kos_ld).read()
    marker = "  _edata = .; PROVIDE (edata = .);"
    if kos.count(marker) != 1:
        raise SystemExit("gen_ld.py: no encuentro '%s' en %s" % (marker.strip(), kos_ld))
    kos = kos.replace(marker, "\n".join(ins) + "\n" + marker)
    with open(out, "w") as f:
        f.write("/* Generado por port/gen_ld.py a partir de %s y %s: no editar */\n" % (
            os.path.basename(kos_ld), os.path.basename(decomp_ld)))
        f.write(kos)
        f.write("\n/* direcciones segmentadas sueltas de la decomp (undefined_syms*.ld) */\n")
        f.write("\n".join(provides) + "\n")
    print("gen_ld.py: %d segmentos, audio_table %d bytes -> %s" % (nseg, audio_size, out))


if __name__ == "__main__":
    main()
