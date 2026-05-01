# Atari Lynx Emulator (C/C++)

Emulador funcional del **Atari Lynx (1989)** escrito en C, basado en la
estructura de `c64.c` / `c64.h` del proyecto
[`chusogar/zxtiny`](https://github.com/chusogar/zxtiny) (carpeta `zxm`).

Reutiliza el mismo core de CPU `m6502` que el emulador de C64 activando el
flag `m65c02_mode = true`, ya que el procesador del Lynx es un MOS 65SC02.

## Componentes implementados

| Subsistema | Estado | Notas |
|------------|--------|-------|
| **CPU 65SC02** | OK | Reutiliza `m6502/m6502.c` con `m65c02_mode=1`, sin BCD |
| **Mapeo MAPCTL ($FFF9)** | OK | Conmuta RAM/Suzy/Mikey/ROM/Vectores |
| **Mikey: timers** | OK | 8 timers, IRQ, RELOAD, divisores 1/2/4/8…/1024 µs |
| **Mikey: paleta** | OK | 16 colores RGB444 (regs `$FDA0-$FDBF`) |
| **Mikey: video DMA** | OK | Render por scanline, 160×102, 4bpp packed |
| **Mikey: audio** | OK | 4 canales con LFSR + envolvente lineal |
| **Mikey: IRQs**   | OK | INTRST/INTSET ($FD80/$FD81) |
| **Mikey: IODAT/IODIR** | Parcial | Strobes de cartucho simplificados |
| **Suzy: math co-proc** | OK | Multiplicación 16×16 con/sin signo y división 32/16 |
| **Suzy: motor de sprites** | Básico | SPRGO + SCB linked-list, 1–4 bpp lineal |
| **Suzy: colisiones** | No | Stub (no se reportan colisiones) |
| **Carga `.lnx`**   | OK | Detecta cabecera `LYNX` de 64 bytes |
| **Carga `.o` raw** | OK | |
| **BIOS Lynx**      | OK | Espera fichero binario de 512 bytes |
| **Vídeo SDL2**     | OK | 160×102 escalado ×4 |
| **Audio SDL2**     | OK | 44.1 kHz mono float |
| **Joypad**         | OK | Teclado: flechas + Z/X + 1/2 + Enter |

## Compilación

```bash
sudo apt-get install -y libsdl2-dev cmake
cd /app/lynx
mkdir -p build && cd build
cmake .. && make
```

Esto produce el binario `./lynx`.

## Uso

```bash
./lynx [lynxboot.img] [cart.lnx | cart.o]
```

* Si no se pasa BIOS, el emulador arranca sin ROM de boot (la mayoría de
  cartuchos comerciales requieren la BIOS de 512 bytes para que el cart
  pase el handshake de descodificación).
* Cartuchos `.lnx` con cabecera de 64 bytes ("LYNX") se cargan
  automáticamente; cartuchos `.o` se tratan como datos crudos.

### Controles

| Tecla     | Función       |
|-----------|---------------|
| Flechas   | D-Pad         |
| Z         | Botón A       |
| X         | Botón B       |
| 1         | Option 1      |
| 2         | Option 2      |
| Enter     | Pause         |
| F2        | Turbo on/off  |
| Esc       | Salir         |

## Estructura del emulador

```
/app/lynx/
├── lynx.h           # Estructuras Mikey, Suzy, Lynx (paralelo a c64.h)
├── lynx.c           # Implementación completa (paralelo a c64.c)
├── m6502/
│   ├── m6502.h      # Core de CPU (copia idéntica del repo zxtiny)
│   └── m6502.c
├── CMakeLists.txt
├── README.md
└── roms/            # Coloca aquí lynxboot.img y tus cartuchos .lnx
```

## Mapa de memoria implementado

```
$0000-$FBFF   RAM 64K (incluye páginas IO si MAPCTL las desactiva)
$FC00-$FCFF   Suzy   (cuando MAPCTL bit0 = 0)
$FD00-$FDFF   Mikey  (cuando MAPCTL bit1 = 0)
$FE00-$FFF7   ROM BIOS 512 bytes (cuando MAPCTL bit2 = 0)
$FFF8         (reservado)
$FFF9         MAPCTL
$FFFA-$FFFF   Vectores NMI/RES/IRQ (cuando MAPCTL bit3 = 0)
```

## Limitaciones conocidas

* El motor de sprites está simplificado: dibuja sprites lineales sin
  estiramiento (`HSIZ`/`VSIZ` parte fraccionaria), sin compresión RLE
  ("packed") completa ni colisiones. Suficiente para arrancar la BIOS y
  algunas demos sencillas. Los juegos comerciales requieren ampliar
  `suzy_run_sprites()` con el algoritmo packed/literal completo.
* La descodificación de cartucho a través de `IODAT/IODIR` está
  simplificada: el emulador copia el primer banco directamente a la RAM
  para acelerar el arranque, en lugar de implementar el protocolo
  cycle-accurate del bus de cartucho.
* La emulación de audio es un LFSR simplificado de 12 bits, no incluye el
  modo "integrate" del Mikey original.
* No incluye soporte de save states ni de ComLynx (UART).

## Créditos

* CPU core `m6502/` por **Chusogar** (proyecto `zxtiny`).
* Estructura y estilo del emulador inspirados en `zxm/c64.c` del mismo
  proyecto.
