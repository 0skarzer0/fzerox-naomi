# Compila F-Zero X (decomp US rev0, sin Expansion Kit) para SH-4 con la KallistiOS del port y lo enlaza con la capa
# de NAOMI (port/src). Lo llama build_fzerox.sh con DECOMP=<clon de la decomp con los assets ya extraídos> y el
# environ.sh de KOS cargado.
#
# Salidas en $(BUILD): fzerox.elf y, para el disco, 1ST_READ.BIN (KallistiOS y la capa de NAOMI), ROM.BIN (la
# "ROM" de N64: código, datos y assets del juego, que main() carga en su sitio) y AUDIOTBL.BIN (las muestras de
# sonido). Ver port/gen_ld.py.

DECOMP ?= $(error falta DECOMP)
BUILD  ?= build
HERE   := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

CC      := $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-gcc
NM      := $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-nm
OBJCOPY := $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-objcopy
SIZE    := $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-size

# Las mismas definiciones que la decomp para US rev0 (su Makefile), más NON_MATCHING/AVOID_UB (C portable en lugar
# de las funciones que siguen en ensamblador de MIPS) y NAOMI_HOST para nuestros cambios.
DEFS := -DVERSION_US=1 -DBUILD_REVISION=REVISION_B -DLANGUAGE=LANGUAGE_ENG -DOS_TV_TYPE=OS_TV_TYPE_NTSC \
        -DBUILD_VERSION=VERSION_I -DASSET_VERSION=us -DASSET_REVISION=rev0 -D_MIPS_SZLONG=32 -DF3DEX_GBI_2 \
        -DNDEBUG -DLANGUAGE_C -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB -D_ULTRA64 -DNAOMI_HOST
INC  := -I$(DECOMP)/include -I$(DECOMP)/bin/us/rev0 -I$(DECOMP) -I$(DECOMP)/include/libultra -I$(DECOMP)/include/PR

# El juego, con los flags de KOS (mismo ABI: -m4-single) y además:
#  -fno-data-sections -fno-toplevel-reorder: los datos en el orden del fuente, como IDO (el juego calcula a veces
#   con la posición relativa de sus variables); -funsigned-char: el char de IDO no tiene signo; gnu89: C de 1996.
# Los avisos del código de la decomp no se miran (-w).
GAME_CFLAGS := $(KOS_CFLAGS) -fno-data-sections -fno-toplevel-reorder -fno-zero-initialized-in-bss -fno-strict-aliasing -funsigned-char \
               -fno-common -std=gnu89 -w $(DEFS) $(INC)
PORT_CFLAGS := $(KOS_CFLAGS) -Wall -Wno-unused -I$(HERE)src

# Lista de ficheros como la decomp (EXPANSION_KIT=0): fuera el 64DD, sus overlays y libultra (la da port/src/ultra.c;
# se queda gu/, las matemáticas de matrices, que es C puro).
EXCL := src/leo/% src/sys/disk/% src/overlays/ovl_i2/dd_save.c src/overlays/ovl_i2/course_edit_context.c \
        src/overlays/ovl_i10/187510.c src/overlays/expansion_kit/% src/overlays/course_edit/% \
        src/overlays/machine_create/% src/overlays/ead_demo/% src/audio/disk/% src/libultra/% \
        src/audio/rom/lib/aisetnextbuf.c
# (aisetnextbuf.c es un osAiSetNextBuffer propio del juego que escribe los registros del AI: lo da ultra.c)
GAME_C  := $(shell cd $(DECOMP) && find src -name '*.c' ! -name '*.inc.c' ! -name '*.incbin.c' ! -path 'src/assets/*' | sort)
GAME_C  := $(filter-out $(EXCL),$(GAME_C)) $(shell cd $(DECOMP) && find src/libultra/gu -name '*.c' | sort)
ASSET_C := $(shell cd $(DECOMP) && find src/assets/us/rev0 -name '*.c' ! -name '*.inc.c' ! -name '*.incbin.c' 2>/dev/null | sort)
OBJS    := $(addprefix $(BUILD)/,$(GAME_C:.c=.o) $(ASSET_C:.c=.o))

# La capa de NAOMI: ultra.c con las cabeceras de la decomp, el resto con las de KOS
PORT_GAMEHDR := ultra gfx_rsp
PORT_KOSHDR  := plat_kos stubs gfx_pvr audio_hle
PORT_OBJS    := $(addprefix $(BUILD)/port/,$(addsuffix .o,$(PORT_GAMEHDR) $(PORT_KOSHDR)))

LD_SCRIPT := $(BUILD)/fzerox_naomi.ld
LDFLAGS   := $(filter-out -T%,$(KOS_LDFLAGS)) -Wl,-T,$(LD_SCRIPT) -Wl,--no-check-sections -Wl,-Map,$(BUILD)/fzerox.map

all: $(BUILD)/1ST_READ.BIN $(BUILD)/ROM.BIN $(BUILD)/AUDIOTBL.BIN

$(BUILD)/%.o: $(DECOMP)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(GAME_CFLAGS) -I$(dir $<) -c $< -o $@ 2>> $(BUILD)/warnings.log || { echo "FALLA $<" | tee -a $(BUILD)/fallos.txt; $(CC) $(GAME_CFLAGS) -I$(dir $<) -c $< -o $@ 2>&1 | grep -m5 error; false; }
	@$(OBJCOPY) --prefix-alloc-sections=.fzxg $@

$(addprefix $(BUILD)/port/,$(addsuffix .o,$(PORT_GAMEHDR))): $(BUILD)/port/%.o: $(HERE)src/%.c $(HERE)src/plat.h $(HERE)src/gfx_backend.h
	@mkdir -p $(dir $@)
	$(CC) $(GAME_CFLAGS) -I$(HERE)src -c $< -o $@

$(addprefix $(BUILD)/port/,$(addsuffix .o,$(PORT_KOSHDR))): $(BUILD)/port/%.o: $(HERE)src/%.c $(HERE)src/plat.h $(HERE)src/gfx_backend.h
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) -c $< -o $@

LDDIR := $(DECOMP)/linker_scripts/us/rev0
$(LD_SCRIPT): $(HERE)gen_ld.py $(LDDIR)/fzerox.ld $(KOS_BASE)/utils/ldscripts/shlelf.xc
	@mkdir -p $(dir $@)
	python3 $(HERE)gen_ld.py $(KOS_BASE)/utils/ldscripts/shlelf.xc $(LDDIR)/fzerox.ld \
	    $(DECOMP)/bin/us/rev0/audio_table.bin $@ $(LDDIR)/undefined_syms.ld $(LDDIR)/auto/undefined_syms_auto.ld

$(BUILD)/fzerox.elf: $(OBJS) $(PORT_OBJS) $(LD_SCRIPT)
	$(CC) $(LDFLAGS) -o $@ $(PORT_OBJS) $(OBJS) $(KOS_LIBS) 2> $(BUILD)/link.log || { grep -m40 "undefined reference\|error" $(BUILD)/link.log | sed 's/.*undefined reference to/  falta/' | sort -u; false; }
	@$(SIZE) $@

$(BUILD)/1ST_READ.BIN: $(BUILD)/fzerox.elf
	$(OBJCOPY) -O binary --wildcard -R '.fzx.*' -R .fzxaudio -R .stack $< $@
	@ls -l $@

$(BUILD)/ROM.BIN: $(BUILD)/fzerox.elf
	$(OBJCOPY) -O binary --wildcard -j '.fzx.*' $< $@
	@ls -l $@

$(BUILD)/AUDIOTBL.BIN: $(DECOMP)/bin/us/rev0/audio_table.bin
	cp $< $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
