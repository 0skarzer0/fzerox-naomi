#!/usr/bin/env python3
"""audio_wav.py RAM ELF OUT.wav [Hz] - saca a un WAV los últimos 4 s de sonido que guarda plat_kos.c
(gPortAudioCapture) de un volcado de la RAM principal (32 MB desde 0x0c000000, el de naomi_live o Flycast)."""
import struct, subprocess, sys, wave

ram = open(sys.argv[1], 'rb').read()
syms = {}
for line in subprocess.run(['sh-elf-nm', sys.argv[2]], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2].lstrip('_')] = int(p[0], 16)
hz = int(sys.argv[4]) if len(sys.argv) > 4 else 32000
n = 32000 * 4


def at(name, size):
    off = (syms[name] & 0x1FFFFFFF) - 0x0C000000
    return ram[off:off + size]


pos = struct.unpack('<I', at('gPortAudioCapturePos', 4))[0]
buf = at('gPortAudioCapture', n * 4)
data = buf[pos * 4:] + buf[:pos * 4]   # del más antiguo al más nuevo
w = wave.open(sys.argv[3], 'wb')
w.setnchannels(2)
w.setsampwidth(2)
w.setframerate(hz)
w.writeframes(data)
w.close()
s = struct.unpack('<%dh' % (len(data) // 2), data)
peak = max(abs(x) for x in s)
rms = (sum(x * x for x in s) / len(s)) ** 0.5
clip = sum(1 for x in s if abs(x) >= 32767)
print('%s: %d cuadros a %d Hz, pico %d, RMS %.0f, saturadas %d' % (sys.argv[3], len(s) // 2, hz, peak, rms, clip))
