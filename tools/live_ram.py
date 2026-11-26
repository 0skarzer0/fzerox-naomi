#!/usr/bin/env python3
"""live_ram.py RAM.bin ELF [SÍMBOLO...]

Lee de un volcado de los 32 MB de RAM (run1.sh de dc2naomi con DUMP=1: la dirección 0x8c000000 + X está en el
desplazamiento X) el bloque naomi_live del parche NAOMI_HOST de KallistiOS (estado, excepción y el log de lo que ha
impreso KOS) y, si se piden, los u32 de unos símbolos del ELF (gPortVblankCount...).
"""
import struct
import subprocess
import sys

FIELDS = ("magic vbls served last_cmd istext irq_pc irq_pr irq_sr main_pc main_pr".split() + ["ring%d" % i for i in range(16)] +
          "thd_polls halted halt_pr halt_why log_pos exc_code exc_evt exc_pc exc_pr exc_sr".split() +
          ["r%d" % i for i in range(16)] + "exc_expevt exc_intevt exc_tea".split())


def syms(elf):
    nm = subprocess.run(["/opt/toolchains/dc/sh-elf/bin/sh-elf-nm", elf], capture_output=True, text=True).stdout
    out = {}
    for ln in nm.splitlines():
        p = ln.split()
        if len(p) == 3:
            out[p[2]] = int(p[0], 16)
    return out


def main():
    ram = open(sys.argv[1], "rb").read()
    s = syms(sys.argv[2])
    off = lambda a: (a & 0x1FFFFFFF) - 0x0C000000

    base = off(s["_naomi_live"])
    vals = struct.unpack_from("<%dI" % len(FIELDS), ram, base)
    v = dict(zip(FIELDS, vals))
    if v["magic"] != 0x4556494C:
        sys.exit("naomi_live: magic %08x, no es este programa" % v["magic"])
    print("vbls %d  peeks %d  halted %d (por %d, pr %08x)  irq_pc %08x irq_pr %08x" % (
        v["vbls"], v["served"], v["halted"], v["halt_why"], v["halt_pr"], v["irq_pc"], v["irq_pr"]))
    if v["exc_code"] or v["exc_pc"]:
        print("EXCEPCIÓN code %x evt %x pc %08x pr %08x sr %08x expevt %x intevt %x tea %08x" % (
            v["exc_code"], v["exc_evt"], v["exc_pc"], v["exc_pr"], v["exc_sr"], v["exc_expevt"], v["exc_intevt"],
            v["exc_tea"]))
        print("  " + " ".join("r%d=%08x" % (i, v["r%d" % i]) for i in range(16)))
    print("pc del vblank: " + " ".join("%08x" % v["ring%d" % i] for i in range(16)))
    logoff = base + 4 * len(FIELDS)
    log = ram[logoff:logoff + 8192]
    pos = v["log_pos"]
    if pos > 8192:
        k = pos % 8192
        log = log[k:] + log[:k]
    else:
        log = log[:pos]
    print("---- log de KOS (%d bytes) ----" % pos)
    print(log.decode("latin-1", "replace"))
    for name in sys.argv[3:]:
        a = s.get("_" + name, s.get(name))
        if a is None:
            print("%s: no está en el ELF" % name)
        else:
            print("%s @%08x = %d" % (name, a, struct.unpack_from("<I", ram, off(a))[0]))


if __name__ == "__main__":
    main()
