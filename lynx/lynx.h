#ifndef LYNX_H
#define LYNX_H
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
#include "m6502/m6502.h"

// ---------------------------------------------------------------------------
// Atari Lynx — temporización (CPU 65SC02 @ ~4 MHz, 60 Hz refresh)
// ---------------------------------------------------------------------------
#define LYNX_CPU_FREQ        4000000      // 4 MHz nominal (16 MHz reloj externo / 4)
#define LYNX_FRAME_RATE      60
#define LYNX_CYCLES_PER_FRAME (LYNX_CPU_FREQ / LYNX_FRAME_RATE)  // 66666
#define LYNX_LINES_PER_FRAME 105                                  // 102 visibles + blanking
#define LYNX_CYCLES_PER_LINE  (LYNX_CYCLES_PER_FRAME / LYNX_LINES_PER_FRAME) // ~635
#define LYNX_AUDIO_RATE      44100
#define LYNX_SAMPLES_PER_FRAME (LYNX_AUDIO_RATE / LYNX_FRAME_RATE) // 735

// ---------------------------------------------------------------------------
// Pantalla LCD del Lynx
// ---------------------------------------------------------------------------
#define LYNX_SCREEN_W  160
#define LYNX_SCREEN_H  102
#define LYNX_SCALE     4

// ---------------------------------------------------------------------------
// Mikey: 4 canales de audio
// ---------------------------------------------------------------------------
#define MIKEY_NUM_AUDIO_CH  4
#define MIKEY_NUM_TIMERS    8

// ---------------------------------------------------------------------------
// LNX header
// ---------------------------------------------------------------------------
#define LNX_HEADER_SIZE 64
#define LNX_MAGIC "LYNX"

// ---------------------------------------------------------------------------
// Memoria mapeada
// ---------------------------------------------------------------------------
// $0000-$FBFF  RAM (64K menos páginas IO)
// $FC00-$FCFF  Suzy (cuando MAPCTL bit0=0)
// $FD00-$FDFF  Mikey (cuando MAPCTL bit1=0)
// $FE00-$FFF7  ROM BIOS (cuando MAPCTL bit2=0)  [512 bytes]
// $FFF8        reservado
// $FFF9        MAPCTL
// $FFFA-$FFFF  Vectores (cuando MAPCTL bit3=0)

// ---------------------------------------------------------------------------
// Mikey timer
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  backup;       // valor de recarga
    uint8_t  control_a;    // $FD00..0x21 step 4
    uint8_t  count;        // contador actual
    uint8_t  control_b;    // estado / IRQ pending
    int32_t  divider_acc;  // acumulador de ticks fraccionales
    bool     borrow_in;    // entrada de timer enlazado
} MikeyTimer;

// ---------------------------------------------------------------------------
// Mikey audio channel
// ---------------------------------------------------------------------------
typedef struct {
    int8_t   volume;       // $FD20+ch*8+0  signed -128..127
    uint8_t  feedback;     // $FD20+ch*8+1
    int8_t   output;       // $FD20+ch*8+2  signed sample output
    uint8_t  shift_lo;     // $FD20+ch*8+3
    uint8_t  backup;       // $FD20+ch*8+4
    uint8_t  control;      // $FD20+ch*8+5  (igual que timer control_a)
    uint8_t  count;        // $FD20+ch*8+6
    uint8_t  other;        // $FD20+ch*8+7  (shift_hi + estado)
    int32_t  divider_acc;
    uint16_t shift_reg;    // LFSR de 12 bits (combinado shift_lo + bits de other)
} MikeyAudio;

// ---------------------------------------------------------------------------
// Suzy — co-procesador (sprites + math)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  regs[0x100];   // $FC00..$FCFF
    // Math co-processor latches
    uint16_t math_a, math_b, math_c, math_d;        // mult inputs
    uint32_t math_e_l;                              // mult output
    uint32_t math_np;                               // div numerator
    uint16_t math_efgh;                             // div result
    bool     math_sign;
    bool     math_carry;
    bool     math_warning;
    // Sprite engine state
    uint16_t scb_addr;       // sprite control block address
    bool     busy;
    bool     sprgo;
    int      sprites_drawn;
} Suzy;

// ---------------------------------------------------------------------------
// Mikey — chip principal de I/O y vídeo
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t     regs[0x100];    // $FD00..$FDFF (acceso bruto)
    MikeyTimer  timer[MIKEY_NUM_TIMERS];
    MikeyAudio  audio[MIKEY_NUM_AUDIO_CH];

    // Paleta verde/rojo/azul (16 entradas, RGB444)
    uint8_t  pal_g[16];      // $FDA0..$FDAF (4 bits cada uno, verde)
    uint8_t  pal_br[16];     // $FDB0..$FDBF (alto=azul, bajo=rojo)
    uint32_t palette_argb[16]; // cache ARGB8888

    // Video DMA
    uint16_t disp_addr;      // $FD94/$FD95 DISPADR
    uint8_t  disp_ctl;       // $FD92 DISPCTL
    uint8_t  vid_buf;        // bandera de video buffer (no usada en single-buffer)

    // Raster
    int      current_line;
    int      hcount;         // ciclos dentro de la línea

    // IRQ
    uint8_t  irq_status;     // $FD80 INTRST
    uint8_t  irq_mask;       // $FD81 INTSET

    // CPU sleep / suzy busy
    bool     cpu_sleep;
} Mikey;

// ---------------------------------------------------------------------------
// Cartridge (formato .lnx + raw)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t* data;           // datos crudos del cartucho (sin cabecera)
    uint32_t size;
    uint16_t bank0_size;     // tamaño página banco 0 (de la cabecera)
    uint16_t bank1_size;     // tamaño página banco 1
    uint8_t  rotation;
    uint8_t  audio_bits;
    char     title[33];
    char     manufacturer[17];
    bool     loaded;

    // Acceso por strobes — modela shifter + counter como en hardware real
    uint8_t  shift_reg;      // 8-bit shift register (upper address bits)
    uint32_t counter;        // auto-increment counter (lower address bits)
    uint8_t  strobe;         // current SYSCTL1 bit 0 state
    uint8_t  shift_count;    // bits to shift: 8(64K), 9(128K), 10(256K), 11(512K)
    uint32_t counter_mask;   // counter wrap mask: 0xFF, 0x1FF, 0x3FF, 0x7FF
} LynxCart;

// ---------------------------------------------------------------------------
// Estructura principal del emulador
// ---------------------------------------------------------------------------
typedef struct {
    m6502 cpu;

    // RAM principal de 64K (incluye páginas IO; el mapeo decide si se accede a RAM o IO)
    uint8_t ram[65536];

    // BIOS ROM 512 bytes ($FE00-$FFF7 + vectores $FFFA-$FFFF se sirven desde aquí)
    uint8_t rom[512];
    bool    rom_loaded;

    // MAPCTL ($FFF9): bit0 SUZY, bit1 MIKEY, bit2 ROM, bit3 VECTORS, bit4 (sequential disable)
    uint8_t mapctl;

    // IODIR / IODAT ($FD8B/$FD8A) — cartridge address strobes
    uint8_t iodir;
    uint8_t iodat;

    // Cartridge
    LynxCart cart;

    // Mikey & Suzy
    Mikey mikey;
    Suzy  suzy;

    // Joypad: bit0=Right bit1=Left bit2=Down bit3=Up bit4=Opt1 bit5=Opt2 bit6=B bit7=A
    uint8_t joypad;
    uint8_t switches;        // pause, restart...

    // SDL
    SDL_Window*   window;
    SDL_Renderer* renderer;
    SDL_Texture*  texture;
    uint32_t      framebuffer[LYNX_SCREEN_W * LYNX_SCREEN_H];

    SDL_AudioDeviceID audio_dev;
    float audio_buffer[LYNX_SAMPLES_PER_FRAME * 2];
    int   audio_pos;

    // Estado
    bool quit;
    bool turbo_mode;
    int  frame_counter;

    // Carga diferida del cart
    char pending_cart[512];
    bool cart_pending;
} Lynx;

// Prototipos públicos
void lynx_init(Lynx* l);
void lynx_destroy(Lynx* l);

int  lynx_load_bios(Lynx* l, const char* path);
int  lynx_load_cart(Lynx* l, const char* path);

void lynx_run_frame(Lynx* l);
void lynx_render(Lynx* l);

#endif // LYNX_H
