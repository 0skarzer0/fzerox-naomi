# Compila el código de la decompilación de F-Zero X (US rev0, sin Expansion Kit) para SH-4 con la KallistiOS del
# port. Lo llama build_fzerox.sh con DECOMP=<clon de la decomp con los assets ya extraídos> y el environ.sh de KOS
# cargado.
#
# Fase 0: objetos de todo el juego y un enlace parcial (fzerox_game.o) del que sale la lista de símbolos que faltan
# (libultra, segmentos de la ROM...), en build/undefined.txt.

DECOMP ?= $(error falta DECOMP)
BUILD  ?= build

CC := $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-gcc
NM := $(KOS_CC_BASE)/bin/$(KOS_CC_PREFIX)-nm

# Las mismas definiciones que la decomp para US rev0 (su Makefile), más NON_MATCHING/AVOID_UB (C portable en lugar
# de las funciones que siguen en ensamblador de MIPS) y NAOMI_HOST para nuestros cambios.
DEFS := -DVERSION_US=1 -DBUILD_REVISION=REVISION_B -DLANGUAGE=LANGUAGE_ENG -DOS_TV_TYPE=OS_TV_TYPE_NTSC \
        -DBUILD_VERSION=VERSION_I -DASSET_VERSION=us -DASSET_REVISION=rev0 -D_MIPS_SZLONG=32 -DF3DEX_GBI_2 \
        -DNDEBUG -DLANGUAGE_C -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB -D_ULTRA64 -DNAOMI_HOST
INC  := -I$(DECOMP)/include -I$(DECOMP)/bin/us/rev0 -I$(DECOMP) -I$(DECOMP)/include/libultra -I$(DECOMP)/include/PR

# El juego está escrito en C89 para IDO; con gnu89 GCC lo acepta casi todo. Los avisos se ven en el log.
CFLAGS := -O2 -ml -m4-single-only -fno-strict-aliasing -funsigned-char -fno-common -std=gnu89 \
          -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types \
          -Wno-return-mismatch -Wno-implicit-int -Wno-pointer-sign -Wno-builtin-declaration-mismatch \
          $(DEFS) $(INC)

# Lista de ficheros como la decomp (EXPANSION_KIT=0): fuera el 64DD, sus overlays y libultra (la sustituye KOS en
# la fase 1; se queda gu/, las matemáticas de matrices, que es C puro).
EXCL := src/leo/% src/sys/disk/% src/overlays/ovl_i2/dd_save.c src/overlays/ovl_i2/course_edit_context.c \
        src/overlays/ovl_i10/187510.c src/overlays/expansion_kit/% src/overlays/course_edit/% \
        src/overlays/machine_create/% src/overlays/ead_demo/% src/audio/disk/% src/libultra/%
GAME_C  := $(shell cd $(DECOMP) && find src -name '*.c' ! -name '*.inc.c' ! -name '*.incbin.c' ! -path 'src/assets/*' | sort)
GAME_C  := $(filter-out $(EXCL),$(GAME_C)) $(shell cd $(DECOMP) && find src/libultra/gu -name '*.c' | sort)
ASSET_C := $(shell cd $(DECOMP) && find src/assets/us/rev0 -name '*.c' ! -name '*.inc.c' ! -name '*.incbin.c' 2>/dev/null | sort)
OBJS    := $(addprefix $(BUILD)/,$(GAME_C:.c=.o) $(ASSET_C:.c=.o))

all: $(BUILD)/undefined.txt

$(BUILD)/%.o: $(DECOMP)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -I$(dir $<) -c $< -o $@ 2>> $(BUILD)/warnings.log || { echo "FALLA $<" | tee -a $(BUILD)/fallos.txt; $(CC) $(CFLAGS) -I$(dir $<) -c $< -o $@ 2>&1 | grep -m5 error; false; }

# enlace parcial: todo el juego en un objeto; lo que queda sin definir es lo que tiene que dar la capa de NAOMI
$(BUILD)/fzerox_game.o: $(OBJS)
	$(CC) -ml -m4-single-only -nostdlib -r -o $@ $(OBJS)

$(BUILD)/undefined.txt: $(BUILD)/fzerox_game.o
	$(NM) -u $< | awk '{print $$2}' | sort > $@
	@echo "$(words $(GAME_C)) ficheros del juego + $(words $(ASSET_C)) de assets; símbolos sin definir: $$(wc -l < $@) ($@)"

clean:
	rm -rf $(BUILD)

.PHONY: all clean
