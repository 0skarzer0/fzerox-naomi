/*
 * plat.h - lo que la libultra del port (ultra_*.c, compilada con las cabeceras de la decomp) necesita de
 * KallistiOS (plat_kos.c, compilada con las de KOS). Solo tipos de C, para que no se mezclen las dos cabeceras.
 */
#ifndef FZX_PLAT_H
#define FZX_PLAT_H

/* Interrupciones */
unsigned plat_irq_disable(void);
void plat_irq_restore(unsigned old);
int plat_in_irq(void);

/* Esperar / despertar sobre una dirección (genwait de KOS). Llamar a plat_wait con las interrupciones cortadas. */
void plat_wait(const void *obj);
int plat_wake(const void *obj, int count);   /* devuelve cuántos ha despertado */
void plat_yield(void);                        /* cede el turno (otro hilo de igual o más prioridad) */

/* Hilos. pri es la prioridad de N64 (0..255, más alto = más importante). */
void *plat_thread_start(void (*entry)(void *), void *arg, int pri, const char *label);
void plat_thread_set_pri(void *thread, int pri);   /* thread NULL = el actual */
void *plat_thread_self(void);
void plat_block_forever(void);

/* Después de escribir código con la CPU (overlays): caché de datos a la RAM e invalidar la de instrucciones */
void plat_sync_code(void *p, unsigned n);

/* Tiempo y vblank */
unsigned long long plat_time_ns(void);
void plat_vblank_install(void (*fn)(void));   /* fn corre dentro de la interrupción */

/* Mando 0 de Dreamcast/NAOMI (vía el gancho maple del residente), ya traducido al de N64: botones con los bits
 * de os_cont.h y stick en -80..80 (arriba positivo). Devuelve 0 si no hay mando. */
int plat_pad_read(unsigned *n64_buttons, int *stick_x, int *stick_y);

/* Log (printf de KOS: consola de dc-tool / log de naomi_live) */
void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void plat_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#endif
