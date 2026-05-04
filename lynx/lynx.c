// =============================================================================
// Atari Lynx emulator core (basado en la estructura del c64.c de zxtiny/zxm)
// =============================================================================
//
// Componentes implementados:
//   * CPU 65SC02 reutilizando el core m6502 con m65c02_mode = true
//   * Mikey: timers (8), IRQs, paleta de 16 colores RGB444, video DMA por
//            scanline, audio de 4 canales (LFSR + envolvente lineal)
//   * Suzy : math co-processor (mult/div 16x16) y motor de sprites (modo
//            literal / packed lineal sin colisión avanzada)
//   * MAPCTL ($FFF9) para conmutar RAM / Suzy / Mikey / ROM / Vectores
//   * Carga de cartuchos .lnx (cabecera de 64 bytes) y raw .o
//   * SDL2 para vídeo, audio y entrada del joypad
//
// =============================================================================

#include "lynx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Lynx lynx;

// =============================================================================
// Pequeñas utilidades
// =============================================================================
static inline uint16_t rd16le(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static void mikey_update_palette_entry(Lynx* l, int idx) {
    uint8_t g = l->mikey.pal_g[idx]  & 0x0F;
    uint8_t br = l->mikey.pal_br[idx];
    uint8_t b = (br >> 4) & 0x0F;
    uint8_t r = br & 0x0F;
    // RGB444 -> RGB888
    uint32_t R = (r << 4) | r;
    uint32_t G = (g << 4) | g;
    uint32_t B = (b << 4) | b;
    l->mikey.palette_argb[idx] = 0xFF000000u | (R << 16) | (G << 8) | B;
}

// =============================================================================
// Suzy — math co-processor + sprite engine (simplificado)
// =============================================================================

static void suzy_reset(Suzy* s) {
    memset(s, 0, sizeof(Suzy));
}

// Multiplica A*B = NP (16x16=32). Con sign si SPRSYS bit7 set.
static void suzy_do_multiply(Suzy* s) {
    int32_t a = s->math_a;
    int32_t b = s->math_b;
    if (s->math_sign) {
        if (a & 0x8000) a -= 0x10000;
        if (b & 0x8000) b -= 0x10000;
    }
    int32_t r = a * b;
    s->math_e_l = (uint32_t)r;
    // Escribir en regs $60..$63 (NP)
    s->regs[0x60] = (uint8_t)(r & 0xFF);
    s->regs[0x61] = (uint8_t)((r >> 8) & 0xFF);
    s->regs[0x62] = (uint8_t)((r >> 16) & 0xFF);
    s->regs[0x63] = (uint8_t)((r >> 24) & 0xFF);
}

// Divide NP / EFGH = ABCD remainder MN
static void suzy_do_divide(Suzy* s) {
    uint32_t np = (uint32_t)s->regs[0x60]
                | ((uint32_t)s->regs[0x61] << 8)
                | ((uint32_t)s->regs[0x62] << 16)
                | ((uint32_t)s->regs[0x63] << 24);
    uint16_t efgh = (uint16_t)s->regs[0x64] | ((uint16_t)s->regs[0x65] << 8);
    if (efgh == 0) {
        s->math_warning = true;
        s->regs[0x52] |= 0x40; // SPRSYS DIVIDE-by-zero
        return;
    }
    uint32_t q = np / efgh;
    uint16_t r = (uint16_t)(np % efgh);
    s->regs[0x52] = (uint8_t)(s->regs[0x52] & ~0x40);
    s->regs[0x6C] = (uint8_t)(q & 0xFF);
    s->regs[0x6D] = (uint8_t)((q >> 8) & 0xFF);
    s->regs[0x6E] = (uint8_t)((q >> 16) & 0xFF);
    s->regs[0x6F] = (uint8_t)((q >> 24) & 0xFF);
    s->regs[0x6A] = (uint8_t)(r & 0xFF);
    s->regs[0x6B] = (uint8_t)((r >> 8) & 0xFF);
}

// SCB simplificado: lee la cabecera y dibuja un sprite en la RAM de vídeo.
static void suzy_run_sprites(Lynx* l) {
    Suzy* s = &l->suzy;
    uint16_t scb = (uint16_t)s->regs[0x10] | ((uint16_t)s->regs[0x11] << 8);
    if (scb == 0) { s->busy = false; s->sprgo = false; return; }

    int safety = 256; // límite de sprites en una llamada
    while (scb != 0 && safety-- > 0) {
        // Leer SCB (mínimo 8 bytes principales)
        uint8_t sprctl0 = l->ram[scb + 0];
        uint8_t sprctl1 = l->ram[scb + 1];
        uint8_t sprcoll = l->ram[scb + 2];
        uint16_t next   = rd16le(&l->ram[scb + 3]);
        uint16_t data   = rd16le(&l->ram[scb + 5]);

        // Posición y tamaño (para sprites no skipped)
        int xpos = (int)(int16_t)rd16le(&l->ram[scb + 7]);
        int ypos = (int)(int16_t)rd16le(&l->ram[scb + 9]);
        uint16_t sw = rd16le(&l->ram[scb + 11]);
        uint16_t sh = rd16le(&l->ram[scb + 13]);

        // sprite type
        uint8_t type = sprctl0 & 0x07;
        uint8_t bpp  = ((sprctl0 >> 6) & 0x03) + 1; // 1..4 bits per pixel

        // skip if SKIP set in sprctl1 bit 2
        bool skip = (sprctl1 & 0x04) != 0;

        if (!skip && data != 0) {
            // Dibujado simplificado: paquetes de scanline tipo Lynx (literal/packed).
            // Implementación pragmática: consumir hasta sh*sw/(8/bpp) bytes y
            // mapearlos como tiles 1bpp a 4bpp con la paleta de 16 colores.
            uint32_t dat_addr = data;
            int width  = sw >> 8;        // entero parte alta (HSIZ es 8.8 fp)
            int height = sh >> 8;
            if (width <= 0)  width  = 8;
            if (height <= 0) height = 8;
            if (width > 64)  width  = 64;
            if (height > 64) height = 64;

            // VIDBAS from Suzy registers $FC08/$FC09
            uint16_t vidbas = (uint16_t)s->regs[0x08] | ((uint16_t)s->regs[0x09] << 8);
            if (vidbas == 0 || vidbas >= 0xFC00) vidbas = l->mikey.disp_addr;

            for (int dy = 0; dy < height; dy++) {
                int y = ypos + dy;
                if (y < 0 || y >= LYNX_SCREEN_H) continue;
                for (int dx = 0; dx < width; dx++) {
                    int x = xpos + dx;
                    if (x < 0 || x >= LYNX_SCREEN_W) continue;

                    // Leer un nibble/bit/etc.
                    uint32_t bit_offset = (uint32_t)((dy * width + dx) * bpp);
                    uint32_t byte_off = dat_addr + (bit_offset >> 3);
                    if (byte_off >= 0xFC00) break;
                    uint8_t b = l->ram[byte_off];
                    uint8_t bit = (uint8_t)(bit_offset & 7);
                    uint8_t mask = (uint8_t)((1 << bpp) - 1);
                    uint8_t color = (uint8_t)((b >> (8 - bpp - bit)) & mask);

                    if (color == 0) continue; // transparent

                    // Pintar en la RAM de vídeo en vidbas (Suzy destination)
                    uint16_t vaddr = vidbas + (uint16_t)(y * (LYNX_SCREEN_W / 2) + (x >> 1));
                    if (vaddr >= 0xFC00) continue;
                    uint8_t cur = l->ram[vaddr];
                    if (x & 1)
                        l->ram[vaddr] = (cur & 0xF0) | (color & 0x0F);
                    else
                        l->ram[vaddr] = (cur & 0x0F) | ((color & 0x0F) << 4);
                }
            }
            (void)type; (void)sprcoll; (void)bpp;
            s->sprites_drawn++;
        }

        scb = next;
    }
    s->busy = false;
    s->sprgo = false;
    s->regs[0x91] &= ~0x01; // SPRGO done
}

// Lectura/escritura de registros Suzy ($FC00-$FCFF)
static uint8_t suzy_read(Lynx* l, uint8_t reg) {
    Suzy* s = &l->suzy;
    switch (reg) {
    case 0x88: return 0x01; // SUZYHREV: revision
    case 0x90: return s->regs[0x90]; // SPRSYS
    case 0x91: return (uint8_t)((s->busy ? 0x01 : 0x00) | s->regs[0x91]);
    case 0x92: return l->joypad ^ 0xFF; // JOYSTICK (active high in Lynx?)
    case 0x93: return l->switches;
    case 0xB2: { // RCART0 — read cartridge bank 0
        if (l->cart.data && l->cart.size > 0) {
            uint32_t addr = ((uint32_t)l->cart.shift_reg << l->cart.shift_count)
                          + (l->cart.counter & l->cart.counter_mask);
            addr %= l->cart.size;
            uint8_t byte = l->cart.data[addr];
            if (!l->cart.strobe) {
                l->cart.counter++;
                l->cart.counter &= 0x7FF;
            }
            return byte;
        }
        return 0xFF;
    }
    case 0xB3: { // RCART1 — read cartridge bank 1
        if (l->cart.data && l->cart.size > 0) {
            uint32_t bank0_bytes = (uint32_t)l->cart.bank0_size * 256;
            uint32_t addr = bank0_bytes + ((uint32_t)l->cart.shift_reg << l->cart.shift_count)
                          + (l->cart.counter & l->cart.counter_mask);
            addr %= l->cart.size;
            uint8_t byte = l->cart.data[addr];
            if (!l->cart.strobe) {
                l->cart.counter++;
                l->cart.counter &= 0x7FF;
            }
            return byte;
        }
        return 0xFF;
    }
    default: return s->regs[reg];
    }
}

static void suzy_write(Lynx* l, uint8_t reg, uint8_t val) {
    Suzy* s = &l->suzy;
    s->regs[reg] = val;

    // Math co-proc: escribir en MATHD ($54) dispara multiplicación
    // Los registros: $54=MATHD,$55=MATHC,$56=MATHB,$57=MATHA
    // Y $52 SPRSYS bit 7 = signed multiply
    if (reg == 0x55) { // MATHC
        // Carga A*B disparada en MATHC, segun docs
    } else if (reg == 0x57) { // MATHA -> dispara multiplicación
        s->math_a = (uint16_t)s->regs[0x57] | ((uint16_t)s->regs[0x56] << 8);
        s->math_b = (uint16_t)s->regs[0x55] | ((uint16_t)s->regs[0x54] << 8);
        s->math_sign = (s->regs[0x52] & 0x80) != 0;
        suzy_do_multiply(s);
    } else if (reg == 0x65) { // MATHF -> dispara división
        suzy_do_divide(s);
    } else if (reg == 0x91) { // SPRGO
        if (val & 0x01) {
            s->busy = true;
            s->sprgo = true;
            suzy_run_sprites(l);
        }
    }
}

// =============================================================================
// Mikey — timers, IRQ, vídeo DMA, paleta, audio
// =============================================================================

// Frecuencias base de los timers (Hz)
static const uint32_t mikey_clock_table[8] = {
    1000000, 500000, 250000, 125000, 62500, 31250, 15625, 0 // 7=linking
};

static void mikey_reset(Mikey* m) {
    memset(m, 0, sizeof(Mikey));
    for (int i = 0; i < 16; i++) {
        m->pal_g[i] = (uint8_t)i;
        m->pal_br[i] = (uint8_t)((i << 4) | (15 - i));
    }
}

static uint8_t mikey_read(Lynx* l, uint8_t reg) {
    Mikey* m = &l->mikey;

    // Timers $00..$1F
    if (reg < 0x20) {
        int idx = reg >> 2;
        switch (reg & 3) {
        case 0: return m->timer[idx].backup;
        case 1: return m->timer[idx].control_a;
        case 2: return m->timer[idx].count;
        case 3: return m->timer[idx].control_b;
        }
    }
    // Audio $20..$3F
    if (reg >= 0x20 && reg < 0x40) {
        int idx = (reg - 0x20) >> 3;
        MikeyAudio* a = &m->audio[idx];
        switch ((reg - 0x20) & 7) {
        case 0: return (uint8_t)a->volume;
        case 1: return a->feedback;
        case 2: return (uint8_t)a->output;
        case 3: return a->shift_lo;
        case 4: return a->backup;
        case 5: return a->control;
        case 6: return a->count;
        case 7: return a->other;
        }
    }
    // Paleta verde $A0..$AF
    if (reg >= 0xA0 && reg < 0xB0) return m->pal_g[reg - 0xA0];
    // Paleta azul/rojo $B0..$BF
    if (reg >= 0xB0 && reg < 0xC0) return m->pal_br[reg - 0xB0];
    // Otros registros conocidos
    switch (reg) {
    case 0x80: return m->irq_status;
    case 0x81: return m->irq_mask;
    case 0x84: return 0; // MAGRDY0 stub
    case 0x88: return 0x01; // MIKEYHREV
    case 0x8B: return l->iodir;
    case 0x8A: { // IODAT
        // Bit 4: leyendo el bit de comst del cart EEPROM = 1 idle
        return l->iodat;
    }
    case 0x92: return m->disp_ctl;
    case 0x94: return (uint8_t)(m->disp_addr & 0xFF);
    case 0x95: return (uint8_t)((m->disp_addr >> 8) & 0xFF);
    default: return m->regs[reg];
    }
}

static void mikey_write(Lynx* l, uint8_t reg, uint8_t val) {
    Mikey* m = &l->mikey;
    uint8_t prev_val = m->regs[reg];
    m->regs[reg] = val;

    if (reg < 0x20) {
        int idx = reg >> 2;
        switch (reg & 3) {
        case 0: m->timer[idx].backup = val; break;
        case 1: m->timer[idx].control_a = val;
                if (val & 0x40) m->timer[idx].count = m->timer[idx].backup; // RELOAD
                break;
        case 2: m->timer[idx].count = val; break;
        case 3: m->timer[idx].control_b = val; break;
        }
        return;
    }
    if (reg >= 0x20 && reg < 0x40) {
        int idx = (reg - 0x20) >> 3;
        MikeyAudio* a = &m->audio[idx];
        switch ((reg - 0x20) & 7) {
        case 0: a->volume   = (int8_t)val; break;
        case 1: a->feedback = val; break;
        case 2: a->output   = (int8_t)val; break;
        case 3: a->shift_lo = val; a->shift_reg = (a->shift_reg & 0x0F00) | val; break;
        case 4: a->backup   = val; break;
        case 5: a->control  = val;
                if (val & 0x40) a->count = a->backup;
                break;
        case 6: a->count    = val; break;
        case 7: a->other    = val;
                a->shift_reg = (uint16_t)((a->shift_reg & 0x00FF) | ((val & 0x0F) << 8));
                break;
        }
        return;
    }
    if (reg >= 0xA0 && reg < 0xB0) {
        m->pal_g[reg - 0xA0] = val & 0x0F;
        mikey_update_palette_entry(l, reg - 0xA0);
        return;
    }
    if (reg >= 0xB0 && reg < 0xC0) {
        m->pal_br[reg - 0xB0] = val;
        mikey_update_palette_entry(l, reg - 0xB0);
        return;
    }
    switch (reg) {
    case 0x80: m->irq_status &= ~val; break;       // INTRST: limpia bits a 1
    case 0x81: m->irq_mask = val; break;            // INTSET
    case 0x8B: l->iodir = val; break;
    case 0x8A: {
        l->iodat = val;
        break;
    }
    case 0x92: m->disp_ctl = val; break;
    case 0x94: m->disp_addr = (uint16_t)((m->disp_addr & 0xFF00) | val); break;
    case 0x95: m->disp_addr = (uint16_t)((m->disp_addr & 0x00FF) | ((uint16_t)val << 8)); break;
    case 0x87: { // SYSCTL1 — cart address strobe
        uint8_t prev = prev_val;
        l->cart.strobe = val & 0x01;
        if (l->cart.strobe) l->cart.counter = 0;
        // Rising edge of bit 0: clock one address bit into shift register
        if ((val & 0x01) && !(prev & 0x01)) {
            uint8_t addr_bit = l->iodat & 0x01;
            l->cart.shift_reg = (uint8_t)((l->cart.shift_reg << 1) | addr_bit);
            l->cart.shift_reg &= 0xFF;
        }
        break;
    }
    case 0x8C: // SERCTL
    case 0x8D: // SERDAT
        break;
    default: break;
    }
}

// Renderizado de una scanline a partir del frame buffer del Lynx en RAM.
static void mikey_render_line(Lynx* l, int line) {
    if (line < 0 || line >= LYNX_SCREEN_H) return;
    uint16_t base = l->mikey.disp_addr + (uint16_t)(line * (LYNX_SCREEN_W / 2));
    uint32_t* dst = &l->framebuffer[line * LYNX_SCREEN_W];
    for (int x = 0; x < LYNX_SCREEN_W; x += 2) {
        uint16_t addr = (uint16_t)(base + (x >> 1));
        uint8_t b = (addr < 0xFC00) ? l->ram[addr] : 0;
        uint8_t hi = (b >> 4) & 0x0F;
        uint8_t lo = b & 0x0F;
        dst[x]     = l->mikey.palette_argb[hi];
        dst[x + 1] = l->mikey.palette_argb[lo];
    }
}

// Avance de timers Mikey por elapsed ciclos de CPU.
// Linking chain: timer 0→2→4→6,  timer 1→3→5→7
static void mikey_step(Lynx* l, int elapsed) {
    Mikey* m = &l->mikey;
    bool borrow[MIKEY_NUM_TIMERS];
    memset(borrow, 0, sizeof(borrow));

    for (int i = 0; i < MIKEY_NUM_TIMERS; i++) {
        MikeyTimer* t = &m->timer[i];
        if (!(t->control_a & 0x08)) continue;        // ENABLE_COUNT
        uint8_t src = t->control_a & 0x07;
        int ticks = 0;

        if (src == 7) {
            // Linked timer: clocked by borrow-out of timer i-2
            if (i >= 2 && borrow[i - 2]) ticks = 1;
        } else {
            uint32_t hz = mikey_clock_table[src];
            if (hz == 0) continue;
            t->divider_acc += (int32_t)elapsed;
            int32_t cyc_per_tick = (int32_t)(LYNX_CPU_FREQ / hz);
            if (cyc_per_tick <= 0) cyc_per_tick = 1;
            while (t->divider_acc >= cyc_per_tick) {
                t->divider_acc -= cyc_per_tick;
                ticks++;
            }
        }

        for (int k = 0; k < ticks; k++) {
            if (t->count == 0) {
                borrow[i] = true;
                t->control_b |= 0x08;        // BORROW_OUT
                if (t->control_a & 0x10)      // RELOAD
                    t->count = t->backup;
                else
                    t->control_a &= ~0x08;    // stop
                if (t->control_a & 0x80)      // ENABLE_INT
                    m->irq_status |= (uint8_t)(1u << i);
                // HSYNC (timer 0) — render scanline
                if (i == 0) {
                    mikey_render_line(l, m->current_line);
                    m->current_line++;
                    if (m->current_line >= LYNX_LINES_PER_FRAME)
                        m->current_line = 0;
                }
            } else {
                t->count--;
            }
        }
    }
}

// Genera una muestra de audio (mezcla 4 canales)
static float mikey_generate_sample(Lynx* l) {
    Mikey* m = &l->mikey;
    int32_t acc = 0;
    for (int i = 0; i < MIKEY_NUM_AUDIO_CH; i++) {
        MikeyAudio* a = &m->audio[i];
        if (!(a->control & 0x08)) continue;
        uint8_t src = a->control & 0x07;
        if (src == 7) continue;
        uint32_t base = mikey_clock_table[src];
        if (base == 0) continue;
        a->divider_acc += (int32_t)(LYNX_CPU_FREQ / LYNX_AUDIO_RATE);
        int32_t cyc_per_tick = (int32_t)(LYNX_CPU_FREQ / base);
        if (cyc_per_tick <= 0) cyc_per_tick = 1;
        while (a->divider_acc >= cyc_per_tick) {
            a->divider_acc -= cyc_per_tick;
            if (a->count == 0) {
                if (a->control & 0x10) a->count = a->backup;
                // LFSR feedback
                uint16_t fb = a->shift_reg & a->feedback;
                uint8_t parity = 0;
                while (fb) { parity ^= (fb & 1); fb >>= 1; }
                a->shift_reg = (uint16_t)(((a->shift_reg << 1) | (parity ^ 1)) & 0x0FFF);
                if (a->control & 0x20)
                    a->output = (a->shift_reg & 1) ? a->volume : (int8_t)-a->volume;
                else
                    a->output = (int8_t)(a->volume * ((a->shift_reg & 1) ? 1 : -1));
            } else {
                a->count--;
            }
        }
        acc += a->output;
    }
    // Normalizar: -512..512 (4 canales x ±128)
    return (float)acc / 512.0f;
}

// =============================================================================
// Memoria — mapeo MAPCTL
// =============================================================================

static uint8_t mem_read(void* userdata, uint16_t addr) {
    Lynx* l = (Lynx*)userdata;

    // $FFF9 MAPCTL
    if (addr == 0xFFF9) return l->mapctl;

    // Vectores $FFFA-$FFFF
    if (addr >= 0xFFFA) {
        if (!(l->mapctl & 0x08) && l->rom_loaded) {
            // ROM mapped
            uint16_t off = addr - 0xFE00;
            if (off < 512) return l->rom[off];
        }
        return l->ram[addr];
    }
    // BIOS ROM $FE00-$FFF7
    if (addr >= 0xFE00 && addr <= 0xFFF7) {
        if (!(l->mapctl & 0x04) && l->rom_loaded) {
            return l->rom[addr - 0xFE00];
        }
        return l->ram[addr];
    }
    // Mikey $FD00-$FDFF
    if (addr >= 0xFD00 && addr < 0xFE00) {
        if (!(l->mapctl & 0x02)) return mikey_read(l, (uint8_t)(addr & 0xFF));
        return l->ram[addr];
    }
    // Suzy $FC00-$FCFF
    if (addr >= 0xFC00 && addr < 0xFD00) {
        if (!(l->mapctl & 0x01)) return suzy_read(l, (uint8_t)(addr & 0xFF));
        return l->ram[addr];
    }
    return l->ram[addr];
}

static void mem_write(void* userdata, uint16_t addr, uint8_t val) {
    Lynx* l = (Lynx*)userdata;

    if (addr == 0xFFF9) { l->mapctl = val; return; }

    if (addr >= 0xFD00 && addr < 0xFE00 && !(l->mapctl & 0x02)) {
        mikey_write(l, (uint8_t)(addr & 0xFF), val);
        return;
    }
    if (addr >= 0xFC00 && addr < 0xFD00 && !(l->mapctl & 0x01)) {
        suzy_write(l, (uint8_t)(addr & 0xFF), val);
        return;
    }
    // Escritura siempre llega a la RAM subyacente (igual que C64)
    l->ram[addr] = val;
}

// =============================================================================
// Joypad
// =============================================================================

static void lynx_handle_key(Lynx* l, SDL_Scancode sc, bool pressed) {
    // F2 turbo, ESC quit
    if (sc == SDL_SCANCODE_ESCAPE && pressed) { l->quit = true; return; }
    if (sc == SDL_SCANCODE_F2 && pressed) {
        l->turbo_mode = !l->turbo_mode;
        return;
    }

    // Joypad bits: 0=Right 1=Left 2=Down 3=Up 4=Opt1 5=Opt2 6=B (inner) 7=A (outer)
    uint8_t bit = 0xFF;
    switch (sc) {
    case SDL_SCANCODE_RIGHT: bit = 0; break;
    case SDL_SCANCODE_LEFT:  bit = 1; break;
    case SDL_SCANCODE_DOWN:  bit = 2; break;
    case SDL_SCANCODE_UP:    bit = 3; break;
    case SDL_SCANCODE_1:     bit = 4; break;  // Option 1
    case SDL_SCANCODE_2:     bit = 5; break;  // Option 2
    case SDL_SCANCODE_X:     bit = 6; break;  // Inner B
    case SDL_SCANCODE_Z:     bit = 7; break;  // Outer A
    case SDL_SCANCODE_RETURN: // Pause
        if (pressed) l->switches |= 0x01;
        else         l->switches &= ~0x01;
        return;
    default: return;
    }
    if (pressed) l->joypad |=  (1u << bit);
    else         l->joypad &= ~(1u << bit);
}

// =============================================================================
// Carga de BIOS y de cartucho (.lnx / .o / .lyx)
// =============================================================================

int lynx_load_bios(Lynx* l, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Lynx: no se pudo abrir BIOS '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != 512) {
        fprintf(stderr, "Lynx: BIOS tamaño inesperado %ld (deben ser 512 bytes)\n", sz);
        fclose(f);
        return -1;
    }
    if (fread(l->rom, 1, 512, f) != 512) { fclose(f); return -1; }
    fclose(f);
    l->rom_loaded = true;
    printf("Lynx: BIOS cargada (512 bytes)\n");
    return 0;
}

int lynx_load_cart(Lynx* l, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }

    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if ((long)fread(buf, 1, (size_t)sz, f) != sz) { free(buf); fclose(f); return -1; }
    fclose(f);

    if (l->cart.data) { free(l->cart.data); l->cart.data = NULL; }

    // Detectar cabecera LNX (64 bytes empezando por "LYNX")
    if (sz >= LNX_HEADER_SIZE && memcmp(buf, LNX_MAGIC, 4) == 0) {
        // Cabecera LNX (64 bytes):
        //   0-3   "LYNX"
        //   4-5   bank0 page size
        //   6-7   bank1 page size
        //   8-9   version
        //  10-41  cartridge name (32 bytes)
        //  42-57  manufacturer name (16 bytes)
        //  58     rotation
        //  59     audio bits / spare
        l->cart.bank0_size = rd16le(buf + 4);
        l->cart.bank1_size = rd16le(buf + 6);
        memcpy(l->cart.title, buf + 10, 32);        l->cart.title[32] = 0;
        memcpy(l->cart.manufacturer, buf + 42, 16); l->cart.manufacturer[16] = 0;
        l->cart.rotation   = buf[58];
        l->cart.audio_bits = buf[59];
        // En implementación simple: sólo guardamos todo el cartucho menos cabecera.
        uint32_t data_sz = (uint32_t)(sz - LNX_HEADER_SIZE);
        l->cart.data = (uint8_t*)malloc(data_sz);
        memcpy(l->cart.data, buf + LNX_HEADER_SIZE, data_sz);
        l->cart.size = data_sz;
        printf("Lynx: cartucho LNX '%s' (%s) cargado: %u bytes (banco0=%u banco1=%u)\n",
               l->cart.title, l->cart.manufacturer, data_sz,
               l->cart.bank0_size, l->cart.bank1_size);
    } else {
        // Raw .o — compute page size from total size (like Handy)
        l->cart.data = buf;
        l->cart.size = (uint32_t)sz;
        l->cart.bank0_size = (uint16_t)(sz >> 8);
        buf = NULL;
        printf("Lynx: cartucho RAW cargado: %u bytes\n", l->cart.size);
    }
    if (buf) free(buf);

    // Compute shift_count and counter_mask from bank0 page_size
    {
        uint16_t ps = l->cart.bank0_size;
        if      (ps <= 0x100) { l->cart.shift_count = 8;  l->cart.counter_mask = 0x0FF; }
        else if (ps <= 0x200) { l->cart.shift_count = 9;  l->cart.counter_mask = 0x1FF; }
        else if (ps <= 0x400) { l->cart.shift_count = 10; l->cart.counter_mask = 0x3FF; }
        else                  { l->cart.shift_count = 11; l->cart.counter_mask = 0x7FF; }
        printf("Lynx: cart shift_count=%d counter_mask=%03X\n",
               l->cart.shift_count, l->cart.counter_mask);
    }
    l->cart.counter = 0;
    l->cart.strobe = 0;
    l->cart.shift_reg = 0;

    // Fallback: copy cart to RAM for BIOS-less boot
    if (!l->rom_loaded) {
        uint32_t copy = l->cart.size;
        if (copy > 0xFC00) copy = 0xFC00;
        memcpy(l->ram, l->cart.data, copy);
    }

    l->cart.loaded = true;

    return 0;
}

// =============================================================================
// Inicialización / destrucción
// =============================================================================

void lynx_init(Lynx* l) {
    memset(l, 0, sizeof(Lynx));

    l->joypad = 0x00;       // sin botones
    l->switches = 0x00;
    l->mapctl = 0x00;       // todo mapeado: Suzy/Mikey/ROM/Vec activos

    // Mikey & Suzy
    mikey_reset(&l->mikey);
    suzy_reset(&l->suzy);
    // Pintar paleta inicial en cache
    for (int i = 0; i < 16; i++) mikey_update_palette_entry(l, i);

    // Disp addr por defecto: muchas demos esperan $2000
    l->mikey.disp_addr = 0x2000;
    l->mikey.disp_ctl  = 0x0D; // DISP enable + 4-bit color

    // CPU
    m6502_init(&l->cpu);
    l->cpu.read_byte = mem_read;
    l->cpu.write_byte = mem_write;
    l->cpu.userdata = l;
    l->cpu.enable_bcd = false;
    l->cpu.m65c02_mode = true;

    // SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return;
    }
    l->window = SDL_CreateWindow("Atari Lynx",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        LYNX_SCREEN_W * LYNX_SCALE, LYNX_SCREEN_H * LYNX_SCALE,
        SDL_WINDOW_SHOWN);
    l->renderer = SDL_CreateRenderer(l->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    l->texture = SDL_CreateTexture(l->renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        LYNX_SCREEN_W, LYNX_SCREEN_H);

    // Audio
    SDL_AudioSpec want = {0}, have;
    want.freq = LYNX_AUDIO_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 1024;
    l->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (l->audio_dev > 0) SDL_PauseAudioDevice(l->audio_dev, 0);
}

void lynx_destroy(Lynx* l) {
    if (l->audio_dev > 0) SDL_CloseAudioDevice(l->audio_dev);
    if (l->texture)  SDL_DestroyTexture(l->texture);
    if (l->renderer) SDL_DestroyRenderer(l->renderer);
    if (l->window)   SDL_DestroyWindow(l->window);
    if (l->cart.data) free(l->cart.data);
    SDL_Quit();
}

// =============================================================================
// Frame loop
// =============================================================================

void lynx_run_frame(Lynx* l) {
    int cycles_done = 0;
    float sample_accum = (float)LYNX_CPU_FREQ / (float)LYNX_AUDIO_RATE;
    float next_sample_at = 0.0f;
    l->audio_pos = 0;
    l->mikey.current_line = 0;



    while (cycles_done < LYNX_CYCLES_PER_FRAME) {
        unsigned long cyc_before = l->cpu.cyc;
        m6502_step(&l->cpu);
        int elapsed = (int)(l->cpu.cyc - cyc_before);
        if (elapsed <= 0) elapsed = 1;

        // Mikey timers + raster
        mikey_step(l, elapsed);

        // IRQ → CPU
        if (l->mikey.irq_status & l->mikey.irq_mask)
            m6502_gen_irq(&l->cpu);

        // Audio sampling
        float pos_f = (float)cycles_done;
        while (pos_f >= next_sample_at) {
            if (l->audio_pos < LYNX_SAMPLES_PER_FRAME && !l->turbo_mode)
                l->audio_buffer[l->audio_pos++] = mikey_generate_sample(l);
            next_sample_at += sample_accum;
        }

        cycles_done += elapsed;
    }

    // Si HSYNC de timer 0 no estaba activo, garantiza un render completo
    if (l->mikey.current_line == 0) {
        for (int y = 0; y < LYNX_SCREEN_H; y++) mikey_render_line(l, y);
    }



    if (l->audio_dev > 0 && l->audio_pos > 0 && !l->turbo_mode) {
        SDL_QueueAudio(l->audio_dev, l->audio_buffer,
                       (uint32_t)l->audio_pos * sizeof(float));
    }
    l->audio_pos = 0;
    l->frame_counter++;
}

void lynx_render(Lynx* l) {
    SDL_UpdateTexture(l->texture, NULL, l->framebuffer, LYNX_SCREEN_W * sizeof(uint32_t));
    SDL_RenderClear(l->renderer);
    SDL_Rect dst = { 0, 0, LYNX_SCREEN_W * LYNX_SCALE, LYNX_SCREEN_H * LYNX_SCALE };
    SDL_RenderCopy(l->renderer, l->texture, NULL, &dst);
    SDL_RenderPresent(l->renderer);
}

// =============================================================================
// Main
// =============================================================================

static bool ext_eq(const char* path, const char* ext) {
    size_t pl = strlen(path), el = strlen(ext);
    if (pl < el) return false;
    const char* tail = path + pl - el;
    for (size_t i = 0; i < el; i++) {
        char a = tail[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

#ifndef LYNX_NO_MAIN
int main(int argc, char* argv[]) {
    lynx_init(&lynx);

    // Argumentos: [bios.img] [cart.lnx]
    const char* bios = NULL;
    const char* cart = NULL;
    for (int i = 1; i < argc; i++) {
        if (ext_eq(argv[i], ".lnx") || ext_eq(argv[i], ".o") || ext_eq(argv[i], ".lyx"))
            cart = argv[i];
        else
            bios = argv[i];
    }

    if (bios) lynx_load_bios(&lynx, bios);
    if (cart) lynx_load_cart(&lynx, cart);

    // Reset CPU
    m6502_gen_res(&lynx.cpu);

    if (!bios && !cart) {
        printf("Uso: %s [bios.img] [cart.lnx|.o]\n", argv[0]);
        printf("  Controles: flechas=dpad, Z=A, X=B, 1=Opt1, 2=Opt2, Enter=Pause\n");
        printf("             F2=turbo, Esc=salir\n");
    }

    const uint32_t FRAME_MS = 1000 / LYNX_FRAME_RATE;
    while (!lynx.quit) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) lynx.quit = true;
            else if (e.type == SDL_KEYDOWN) lynx_handle_key(&lynx, e.key.keysym.scancode, true);
            else if (e.type == SDL_KEYUP)   lynx_handle_key(&lynx, e.key.keysym.scancode, false);
        }

        lynx_run_frame(&lynx);

        if (lynx.turbo_mode) {
            if ((lynx.frame_counter & 7) == 0) lynx_render(&lynx);
        } else {
            lynx_render(&lynx);
            uint32_t el = SDL_GetTicks() - t0;
            if (el < FRAME_MS) SDL_Delay(FRAME_MS - el);
        }
    }

    lynx_destroy(&lynx);
    return 0;
}
#endif // LYNX_NO_MAIN
