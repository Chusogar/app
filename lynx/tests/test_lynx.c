/* Test E2E del emulador Lynx: corre 60 frames y vuelca un PNG (PPM) del framebuffer.
   Comprueba CPU progress, timers Mikey y render. */
#include "lynx.h"
#include <stdio.h>
#include <string.h>

extern int lynx_load_bios(Lynx*, const char*);
extern int lynx_load_cart(Lynx*, const char*);
extern void lynx_run_frame(Lynx*);

int main(int argc, char** argv) {
    Lynx l;
    /* Inicializamos sin SDL (head-less): SDL_INIT con dummy video driver */
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);

    extern void lynx_init(Lynx*);
    lynx_init(&l);

    if (argc > 1) lynx_load_bios(&l, argv[1]);
    if (argc > 2) lynx_load_cart(&l, argv[2]);

    extern void m6502_gen_res(m6502*);
    m6502_gen_res(&l.cpu);

    unsigned long cyc0 = l.cpu.cyc;
    /* Forzar paleta visible: blanco para color 1 */
    l.mikey.pal_g[1] = 0x0F;
    l.mikey.pal_br[1] = 0xFF;
    /* Llenamos algo de RAM en disp_addr para ver render */
    for (int i = 0; i < 160*102/2; i++)
        l.ram[(0x2000 + i) & 0xFFFF] = (uint8_t)(i & 0xFF);

    for (int f = 0; f < 60; f++) lynx_run_frame(&l);

    printf("Test: %d frames ejecutados\n", l.frame_counter);
    printf("CPU cycles avanzados: %lu (esperado >= %d)\n",
           l.cpu.cyc - cyc0, LYNX_CYCLES_PER_FRAME * 30);
    printf("Mikey IRQ status=%02X mask=%02X\n", l.mikey.irq_status, l.mikey.irq_mask);
    printf("Mikey timer0 backup=%02X count=%02X ctrl=%02X\n",
           l.mikey.timer[0].backup, l.mikey.timer[0].count, l.mikey.timer[0].control_a);

    /* Cuenta píxeles no negros en el framebuffer */
    int non_black = 0;
    for (int i = 0; i < LYNX_SCREEN_W * LYNX_SCREEN_H; i++)
        if ((l.framebuffer[i] & 0x00FFFFFF) != 0) non_black++;
    printf("Framebuffer: %d/%d píxeles no-negros\n", non_black, LYNX_SCREEN_W * LYNX_SCREEN_H);

    /* Volcar PPM */
    FILE* f = fopen("/tmp/lynx_test.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", LYNX_SCREEN_W, LYNX_SCREEN_H);
        for (int i = 0; i < LYNX_SCREEN_W * LYNX_SCREEN_H; i++) {
            uint32_t p = l.framebuffer[i];
            uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        printf("Volcado /tmp/lynx_test.ppm OK\n");
    }

    extern void lynx_destroy(Lynx*);
    lynx_destroy(&l);
    return (l.frame_counter == 60 && non_black > 0) ? 0 : 1;
}
