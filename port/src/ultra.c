/*
 * ultra.c - la libultra que usa F-Zero X, sobre KallistiOS (a través de plat.h).
 * Se compila con las cabeceras de la decomp, como el juego: las estructuras (OSThread, OSMesgQueue, OSIoMesg,
 * OSPiHandle, OSContPad, OSTask) son las suyas.
 *
 * Modelo:
 * - Hilos de N64 = hilos de KOS con la misma prioridad relativa (plat_kos.c). El hilo se crea en osStartThread.
 * - Colas de mensajes con la semántica de N64; se bloquea con genwait sobre la propia cola, con las interrupciones
 *   cortadas mientras se mira y se toca la cola (también se envían mensajes desde el vblank).
 * - "ROM": las direcciones de ROM son punteros reales a ROM.BIN cargado en RAM (port/gen_ld.py), y las de RAM,
 *   punteros a la ventana de RDRAM; la DMA del PI es un memcpy. La SRAM (partidas) es un búfer de 32 KB (fase 6:
 *   VMU).
 * - VI: el vblank de KOS cambia de imagen y manda el mensaje de retrazado.
 * - RSP: osSpTaskStartGo hace la tarea en el momento (fzx_gfx_task en gfx_rsp.c, fzx_audio_task con el HLE de
 *   audio_hle.c) y manda los eventos SP (y DP para gráficos).
 * - AI: la cola de sonido de plat_kos.c.
 */
#include "global.h"
#include "plat.h"

/* ------------------------------------------------------------------ variables de libultra */

s32 osTvType = OS_TV_NTSC;
s32 osResetType = 0;
u32 osMemSize = 0x400000;   /* 4 MB: sin Expansion Pak, así el juego no intenta usar el 64DD */
s32 osAppNMIBuffer[16];
OSViMode osViModeNtscLan1, osViModeMpalLan1, osViModeFpalLan1;

/* ------------------------------------------------------------------ hilos */

typedef struct {
    void (*entry)(void*);
    void* arg;
    void* kthread;
} PortThread;

#define MAX_THREADS 16
static struct {
    OSThread* t;
    void* k;
} sThreads[MAX_THREADS];

static PortThread* port_of(OSThread* t) {
    return (PortThread*) &t->context;
}

static OSThread* current_thread(void) {
    void* k = plat_thread_self();
    s32 i;

    for (i = 0; i < MAX_THREADS; i++) {
        if (sThreads[i].k == k) {
            return sThreads[i].t;
        }
    }
    return NULL;
}

void osCreateThread(OSThread* t, OSId id, void (*entry)(void*), void* arg, void* sp, OSPri pri) {
    PortThread* p = port_of(t);

    (void) sp; /* KOS da su propia pila */
    bzero(t, sizeof(OSThread));
    t->id = id;
    t->priority = pri;
    t->state = OS_STATE_STOPPED;
    p->entry = entry;
    p->arg = arg;
    p->kthread = NULL;
}

void osStartThread(OSThread* t) {
    PortThread* p = port_of(t);
    char label[16];
    s32 i;
    u32 im;

    if (p->kthread != NULL) {
        return;
    }
    label[0] = 'n';
    label[1] = '6';
    label[2] = '4';
    label[3] = '-';
    label[4] = '0' + (t->id / 10) % 10;
    label[5] = '0' + t->id % 10;
    label[6] = '\0';
    t->state = OS_STATE_RUNNABLE;
    /* registrar antes de que el hilo nuevo pueda correr (puede tener más prioridad que el que lo crea) */
    im = plat_irq_disable();
    p->kthread = plat_thread_start(p->entry, p->arg, t->priority, label);
    for (i = 0; i < MAX_THREADS; i++) {
        if (sThreads[i].t == NULL || sThreads[i].t == t) {
            sThreads[i].t = t;
            sThreads[i].k = p->kthread;
            break;
        }
    }
    plat_irq_restore(im);
    plat_log("fzx: hilo %d (prioridad %d) en marcha\n", (int) t->id, (int) t->priority);
    /* Como en N64: si el nuevo tiene más prioridad que el que lo arranca, corre ya, hasta que se bloquea. Sin esto,
     * en la placa el hilo del juego (10) encolaba las secuencias antes de que el de audio (20) acabara Audio_Init, que
     * vuelve a crear la cola de órdenes: el aviso se perdía y no sonaba nada (en Flycast el orden salía bien). */
    {
        void* self = plat_thread_self();

        for (i = 0; i < MAX_THREADS; i++) {
            if (sThreads[i].t != NULL && sThreads[i].k == self) {
                if (t->priority > sThreads[i].t->priority) {
                    plat_thread_yield_until_blocked(p->kthread);
                }
                break;
            }
        }
    }
}

void osStopThread(OSThread* t) {
    if (t == NULL || t == current_thread()) {
        plat_log("fzx: osStopThread del hilo actual: se queda parado\n");
        plat_block_forever();
    }
    plat_log("fzx: osStopThread(%d) de otro hilo: no soportado\n", t ? (int) t->id : -1);
}

void osSetThreadPri(OSThread* t, OSPri pri) {
    if (t == NULL) {
        t = current_thread();
    }
    if (t != NULL) {
        t->priority = pri;
        plat_thread_set_pri(port_of(t)->kthread, pri);
    } else {
        plat_thread_set_pri(NULL, pri);
    }
}

OSPri osGetThreadPri(OSThread* t) {
    if (t == NULL) {
        t = current_thread();
    }
    return t ? t->priority : 0;
}

OSThread* __osGetActiveQueue(void) {
    return NULL;
}

/* ------------------------------------------------------------------ colas de mensajes */

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

/* para depurar bloqueos: en qué cola espera cada hilo de N64 (por su id; 0 = no espera; bit 0 = esperando sitio) */
u32 gPortWaitQ[16];

static void note_wait(OSMesgQueue* mq, s32 full) {
    OSThread* t = current_thread();

    if (t != NULL && t->id < 16) {
        gPortWaitQ[t->id] = mq ? ((u32) mq | full) : 0;
    }
}

/* genwait espera sobre una dirección: los que reciben esperan en &mq->mtqueue y los que envían en &mq->fullqueue */
static s32 mq_send(OSMesgQueue* mq, OSMesg msg, s32 flag, s32 jam) {
    u32 im = plat_irq_disable();
    s32 woke;

    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK || plat_in_irq()) {
            plat_irq_restore(im);
            return -1;
        }
        note_wait(mq, 1);
        plat_wait(&mq->fullqueue);
        note_wait(NULL, 0);
    }
    if (jam) {
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        mq->msg[mq->first] = msg;
    } else {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    }
    mq->validCount++;
    woke = plat_wake(&mq->mtqueue, 1);
    plat_irq_restore(im);
    /* en N64 el que recibe, si tiene más prioridad, corre ya */
    if (woke > 0 && !plat_in_irq()) {
        plat_yield();
    }
    return 0;
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    return mq_send(mq, msg, flag, 0);
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    return mq_send(mq, msg, flag, 1);
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flag) {
    u32 im = plat_irq_disable();
    s32 woke;

    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) {
            plat_irq_restore(im);
            return -1;
        }
        note_wait(mq, 0);
        plat_wait(&mq->mtqueue);
        note_wait(NULL, 0);
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    woke = plat_wake(&mq->fullqueue, 1);
    plat_irq_restore(im);
    if (woke > 0) {
        plat_yield();
    }
    return 0;
}

/* ------------------------------------------------------------------ eventos */

#define NUM_EVENTS 32
static struct {
    OSMesgQueue* mq;
    OSMesg msg;
} sEvents[NUM_EVENTS];

void osSetEventMesg(OSEvent e, OSMesgQueue* mq, OSMesg msg) {
    if (e < NUM_EVENTS) {
        sEvents[e].mq = mq;
        sEvents[e].msg = msg;
    }
}

static void post_event(s32 e) {
    if (sEvents[e].mq != NULL) {
        osSendMesg(sEvents[e].mq, sEvents[e].msg, OS_MESG_NOBLOCK);
    }
}

/* ------------------------------------------------------------------ interrupciones */

static OSIntMask sIntMask = OS_IM_ALL;
static u32 sIrqSaved;

OSIntMask osGetIntMask(void) {
    return sIntMask;
}

/* El juego corta las interrupciones (OS_IM_NONE) para secciones críticas cortas y luego deja la máscara vieja. */
OSIntMask osSetIntMask(OSIntMask im) {
    OSIntMask old = sIntMask;

    if (im == OS_IM_NONE && old != OS_IM_NONE) {
        sIrqSaved = plat_irq_disable();
    } else if (im != OS_IM_NONE && old == OS_IM_NONE) {
        plat_irq_restore(sIrqSaved);
    }
    sIntMask = im;
    return old;
}

void __osSetHWIntrRoutine(OSHWIntr interrupt, s32 (*handler)(void)) {
    (void) interrupt;
    (void) handler;
}

/* ------------------------------------------------------------------ tiempo (reloj de la N64: 46,875 MHz) */

static OSTime sTimeOffset;

OSTime osGetTime(void) {
    return (plat_time_ns() * 3) / 64 + sTimeOffset;
}

void osSetTime(OSTime t) {
    sTimeOffset = t - (plat_time_ns() * 3) / 64;
}

u32 osGetCount(void) {
    return (u32) osGetTime();
}

/* ------------------------------------------------------------------ memoria y cachés */

/* P1 del SH-4 = físico | 0x80000000, como el KSEG0 de la N64 */
u32 osVirtualToPhysical(void* v) {
    return (u32) v & 0x1FFFFFFF;
}

void* osPhysicalToVirtual(u32 p) {
    return (void*) (p | 0x80000000);
}

/* Sin DMA de verdad (todo es memcpy de la CPU) no hace falta tocar la caché de datos: invalidarla perdería datos.
 * La de instrucciones se sincroniza al copiar código (osEPiStartDma). */
void osWritebackDCache(void* p, s32 n) {
    (void) p;
    (void) n;
}
void osWritebackDCacheAll(void) {
}
void osInvalDCache(void* p, s32 n) {
    (void) p;
    (void) n;
}
void osInvalICache(void* p, s32 n) {
    (void) p;
    (void) n;
}

u32 osGetMemSize(void) {
    return osMemSize;
}

void osInitialize(void) {
    plat_log("fzx: osInitialize\n");
}

/* ------------------------------------------------------------------ PI: ROM, SRAM y 64DD */

extern void fzx_mio0_forget(const void* p, unsigned n);

static OSPiHandle sCartHandle, sDriveHandle, sLeoHandle;
static u8 sSram[0x8000];

static void* ptr_of(u32 a) {
    /* una dirección física de nuestra RAM (0x0C000000-0x0DFFFFFF) pasa a P1 */
    if (a >= 0x0C000000 && a < 0x0E000000) {
        return (void*) (a | 0x80000000);
    }
    return (void*) a;
}

void osCreatePiManager(OSPri pri, OSMesgQueue* cmdQ, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    (void) pri;
    (void) cmdQ;
    (void) cmdBuf;
    (void) cmdMsgCnt;
}

OSPiHandle* osCartRomInit(void) {
    sCartHandle.type = DEVICE_TYPE_CART;
    return &sCartHandle;
}

OSPiHandle* osDriveRomInit(void) {
    sDriveHandle.type = DEVICE_TYPE_BULK;
    return &sDriveHandle;
}

OSPiHandle* osLeoDiskInit(void) {
    sLeoHandle.type = DEVICE_TYPE_64DD;
    return &sLeoHandle;
}

s32 osEPiLinkHandle(OSPiHandle* h) {
    (void) h;
    return 0;
}

s32 osEPiReadIo(OSPiHandle* h, u32 devAddr, u32* data) {
    (void) devAddr;
    if (data != NULL) {
        /* para el 64DD, todos los bits: LEO_STATUS_PRESENCE_MASK puesto = no hay disco (LeoDD_CheckPresence) */
        *data = (h == &sLeoHandle) ? 0xFFFFFFFF : 0;
    }
    return 0;
}

s32 osEPiWriteIo(OSPiHandle* h, u32 devAddr, u32 data) {
    (void) h;
    (void) devAddr;
    (void) data;
    return 0;
}

/* registro de las últimas DMA, para depurar (se lee del volcado de RAM: origen, destino, tamaño) */
u32 gPortDmaLog[1024][3];
u32 gPortDmaCount;

s32 osEPiStartDma(OSPiHandle* h, OSIoMesg* mb, s32 direction) {
    u8* dram = ptr_of((u32) mb->dramAddr);
    u32 k = gPortDmaCount++ % 1024;

    gPortDmaLog[k][0] = mb->devAddr;
    gPortDmaLog[k][1] = (u32) dram;
    gPortDmaLog[k][2] = mb->size | (direction == OS_READ ? 0 : 0x80000000);

    if (h != NULL && h->type == DEVICE_TYPE_SRAM) {
        u32 off = mb->devAddr - 0x08000000;

        if (off + mb->size > sizeof(sSram)) {
            plat_log("fzx: SRAM fuera de rango: %08x + %u\n", (unsigned) mb->devAddr, (unsigned) mb->size);
        } else if (direction == OS_READ) {
            bcopy(&sSram[off], dram, mb->size);
        } else {
            bcopy(dram, &sSram[off], mb->size);
        }
    } else if (direction == OS_READ) {
        u8* rom = ptr_of(mb->devAddr);

        bcopy(rom, dram, mb->size);
        plat_sync_code(dram, mb->size); /* puede ser código (overlays) */
        fzx_mio0_forget(dram, mb->size); /* ya no son bytes de un MIO0 (stubs.c) */
    } else {
        plat_log("fzx: escritura por PI a %08x ignorada\n", (unsigned) mb->devAddr);
    }
    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, (OSMesg) mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

/* ------------------------------------------------------------------ VI */

static void* sCurFb;
static void* sNextFb;
static OSMesgQueue* sViMq;
static OSMesg sViMsg;
static u32 sViRetrace = 1;
static u32 sViCount;
u32 gPortVblankCount;

static void vi_vblank(void) {
    gPortVblankCount++;
    sCurFb = sNextFb;
    if (sViMq != NULL && ++sViCount >= sViRetrace) {
        sViCount = 0;
        /* Si el hilo principal va atrasado (un cuadro largo), los retrazados llenarían su cola y los eventos SP/DP
         * de osSpTaskStartGo se perderían: el juego creería el RSP ocupado para siempre. Se dejan 4 huecos. */
        if (sViMq->validCount < sViMq->msgCount - 4) {
            osSendMesg(sViMq, sViMsg, OS_MESG_NOBLOCK);
        }
    }
}

void osCreateViManager(OSPri pri) {
    (void) pri;
    plat_vblank_install(vi_vblank);
}

void osViSetEvent(OSMesgQueue* mq, OSMesg msg, u32 retraceCount) {
    sViMq = mq;
    sViMsg = msg;
    sViRetrace = retraceCount ? retraceCount : 1;
}

void osViSwapBuffer(void* fb) {
    sNextFb = fb;
}

void* osViGetCurrentFramebuffer(void) {
    return sCurFb;
}

void* osViGetNextFramebuffer(void) {
    return sNextFb;
}

void osViSetMode(OSViMode* mode) {
    (void) mode;
}
void osViSetSpecialFeatures(u32 f) {
    (void) f;
}
void osViSetYScale(f32 s) {
    (void) s;
}
void osViBlack(u8 black) {
    (void) black;
}

/* ------------------------------------------------------------------ SI: mandos, Controller Pak, Rumble Pak */

s32 osContInit(OSMesgQueue* mq, u8* bitpattern, OSContStatus* status) {
    s32 i;

    (void) mq;
    *bitpattern = 1;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        status[i].type = (i == 0) ? CONT_TYPE_NORMAL : 0;
        status[i].status = 0;
        status[i].errno = (i == 0) ? 0 : CONT_NO_RESPONSE_ERROR;
    }
    return 0;
}

s32 osContStartReadData(OSMesgQueue* mq) {
    (void) mq; /* en N64 la lectura acaba con la interrupción del SI, que llega como evento */
    post_event(OS_EVENT_SI);
    return 0;
}

void osContGetReadData(OSContPad* pads) {
    unsigned buttons;
    int x, y;
    s32 i;

    for (i = 0; i < MAXCONTROLLERS; i++) {
        pads[i].button = 0;
        pads[i].stick_x = 0;
        pads[i].stick_y = 0;
        pads[i].errno = CONT_NO_RESPONSE_ERROR;
    }
    if (plat_pad_read(&buttons, &x, &y)) {
#ifdef FZX_AUTOPAD
        /* Solo para pruebas en Flycast (make PORT_EXTRA=-DFZX_AUTOPAD): tras 10 pulsaciones de START (las de la
         * secuencia que llega a la carrera), en carrera mantiene A y pulsa B (turbo) 5 cuadros cada 2 s. */
        {
            extern s32 gGameMode;
            static unsigned starts, prev, n;

            if ((buttons & START_BUTTON) && !(prev & START_BUTTON)) {
                starts++;
            }
            prev = buttons;
            if (starts >= 10 && (gGameMode & 0xFF) == 1) {
                n++;
                buttons |= A_BUTTON;
                if (n % 120 < 5) {
                    buttons |= B_BUTTON;
                }
            }
        }
#endif
        pads[0].button = buttons;
        pads[0].stick_x = x;
        pads[0].stick_y = y;
        pads[0].errno = 0;
    }
}

s32 osPfsInitPak(OSMesgQueue* mq, OSPfs* pfs, int channel) {
    (void) mq;
    (void) pfs;
    (void) channel;
    return PFS_ERR_NOPACK;
}

s32 osPfsFindFile(OSPfs* pfs, u16 company, u32 game, u8* gameName, u8* extName, s32* fileNo) {
    (void) pfs;
    (void) company;
    (void) game;
    (void) gameName;
    (void) extName;
    (void) fileNo;
    return PFS_ERR_NOPACK;
}

s32 osPfsAllocateFile(OSPfs* pfs, u16 company, u32 game, u8* gameName, u8* extName, int len, s32* fileNo) {
    (void) pfs;
    (void) company;
    (void) game;
    (void) gameName;
    (void) extName;
    (void) len;
    (void) fileNo;
    return PFS_ERR_NOPACK;
}

s32 osPfsReadWriteFile(OSPfs* pfs, s32 fileNo, u8 flag, int offset, int size, u8* data) {
    (void) pfs;
    (void) fileNo;
    (void) flag;
    (void) offset;
    (void) size;
    (void) data;
    return PFS_ERR_NOPACK;
}

s32 osMotorInit(OSMesgQueue* mq, OSPfs* pfs, int channel) {
    (void) mq;
    (void) pfs;
    (void) channel;
    return PFS_ERR_NOPACK;
}

s32 __osMotorAccess(OSPfs* pfs, s32 flag) {
    (void) pfs;
    (void) flag;
    return PFS_ERR_NOPACK;
}

/* ------------------------------------------------------------------ AI (plat_kos.c) */

s32 osAiSetFrequency(u32 freq) {
    plat_audio_start(freq);
    return freq;
}

u32 osAiGetLength(void) {
    return plat_audio_pending_frames() * 4;
}

s32 osAiSetNextBuffer(void* buf, u32 size) {
    plat_audio_push(buf, size / 4);
    return 0;
}

/* ------------------------------------------------------------------ RSP / RDP */

/* Las fases 2 y 3 las definen de verdad */
void __attribute__((weak)) fzx_gfx_task(OSTask* t) {
    (void) t;
}
void fzx_audio_task(OSTask* t) {
    fzx_audio_run((const unsigned*) t->t.data_ptr, t->t.data_size / sizeof(Acmd));
}

u32 gPortGfxTasks, gPortAudioTasks;
u32 gPortAudioUs;   /* tiempo total en el HLE de audio (microsegundos): con gPortVblankCount da el % de CPU */

void osSpTaskLoad(OSTask* t) {
    (void) t;
}

void osSpTaskStartGo(OSTask* t) {
    if (t->t.type == M_GFXTASK) {
        gPortGfxTasks++;
        fzx_gfx_task(t);
        post_event(OS_EVENT_SP);
        post_event(OS_EVENT_DP);
    } else {
        unsigned long long t0 = plat_time_ns();

        gPortAudioTasks++;
        fzx_audio_task(t);
        gPortAudioUs += (u32) ((plat_time_ns() - t0) / 1000);
        post_event(OS_EVENT_SP);
    }
}

void osSpTaskYield(void) {
}

OSYieldResult osSpTaskYielded(OSTask* t) {
    (void) t;
    return 0;
}

u32 osDpGetStatus(void) {
    return 0;
}

void osDpSetStatus(u32 s) {
    (void) s;
}
