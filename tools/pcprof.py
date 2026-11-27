#!/usr/bin/env python3
"""pcprof.py RAM ELF [N] - perfil por muestreo: agrupa por función los PC que plat_kos.c guarda en cada vblank
(gPortPcProf, 8192 muestras de PC y PR) en un volcado de la RAM principal (32 MB desde 0x0c000000: el de Flycast o
el de la placa). Muestra las N funciones con más muestras (por PC) y, para las funciones de biblioteca, quién las
llama (por PR)."""
import bisect, struct, subprocess, sys
from collections import Counter

ram = open(sys.argv[1], 'rb').read()
top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
syms = []
for line in subprocess.run(['sh-elf-nm', '-n', sys.argv[2]], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16), p[2].lstrip('_')))
addrs = [a for a, _ in syms]
named = {n: a for a, n in syms}
allsyms = {}
for line in subprocess.run(['sh-elf-nm', sys.argv[2]], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        allsyms[p[2].lstrip('_')] = int(p[0], 16)


def at(addr, n):
    off = (addr & 0x1FFFFFFF) - 0x0C000000
    return ram[off:off + n]


def name(pc):
    i = bisect.bisect_right(addrs, pc) - 1
    return syms[i][1] if i >= 0 else '?'


pos = struct.unpack('<I', at(allsyms['gPortPcProfPos'], 4))[0]
n = min(pos, 8192)
raw = struct.unpack('<%dI' % (2 * 8192), at(allsyms['gPortPcProf'], 8192 * 8))
samples = [(raw[2 * i], raw[2 * i + 1]) for i in range(8192)][:n] if pos < 8192 else \
    [(raw[2 * i], raw[2 * i + 1]) for i in range(8192)]
by_pc = Counter(name(pc) for pc, _ in samples)
print('%d muestras' % len(samples))
for f, c in by_pc.most_common(top):
    callers = Counter(name(pr) for pc, pr in samples if name(pc) == f)
    who = ', '.join('%s %d' % (k, v) for k, v in callers.most_common(3))
    print('%5.1f%%  %-40s  <- %s' % (100.0 * c / len(samples), f, who))
