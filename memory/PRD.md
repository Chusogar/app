# PRD — Atari Lynx Emulator (C/C++)

## Original Problem Statement
> Desarrolla un emulador de atari lynx basándote en el emulador c64.c y c64.h
> del repositorio de github chusogar/zxtiny/zxm

## User Choices
| Pregunta | Respuesta |
|----------|-----------|
| Repositorio fuente | https://github.com/chusogar/zxtiny |
| Lenguaje/plataforma | **C/C++ nativo** (igual que `c64.c`) |
| Alcance | **Emulador funcional completo** |
| Interfaz | **Solo el core** (sin web UI), SDL2 nativo |
| ROMs/BIOS | El usuario las aporta a mano |

## Architecture / Tech Stack
- **CPU**: reutiliza el core `m6502` del propio repo `chusogar/zxtiny/zxm/m6502/`
  con el flag `m65c02_mode = true` (el Lynx integra un MOS 65SC02)
- **Lenguaje**: C11
- **SDL2**: vídeo (160×102 ×4), audio (44.1 kHz mono float), entrada (teclado)
- **Build**: CMake (`apt-get install libsdl2-dev cmake`)

## File Structure
```
/app/lynx/
├── lynx.h                # 208 líneas — estructuras Mikey/Suzy/Lynx (paralelo a c64.h)
├── lynx.c                # 800 líneas — implementación completa (paralelo a c64.c)
├── m6502/
│   ├── m6502.h           # core CPU del repo upstream
│   └── m6502.c
├── CMakeLists.txt
├── README.md             # documentación de uso, mapa de memoria, controles
├── tests/
│   └── test_lynx.c       # test E2E head-less (60 frames, render PPM)
└── roms/                 # carpeta para BIOS y .lnx del usuario
```

## What's been implemented (May 2026)
- **Mapa de memoria con MAPCTL** ($FFF9): conmuta RAM/Suzy/Mikey/ROM/Vectores
- **Mikey** ($FD00-$FDFF): 8 timers con divisores 1/2/4/8/16/32/64µs y reload,
  IRQ status/mask, paleta de 16 colores RGB444, video DMA por scanline,
  4 canales de audio con LFSR + envolvente, IODAT/IODIR (strobes simplificados)
- **Suzy** ($FC00-$FCFF): math co-processor (multiplicación 16×16 con/sin signo,
  división 32/16), motor de sprites SPRGO con cadena SCB linked-list (1–4 bpp
  lineal, sin packed RLE ni colisiones)
- **Carga de cartuchos `.lnx`** con parseo de cabecera de 64 bytes (LYNX magic,
  bank0/1 size, título 32 bytes, manufacturer 16 bytes, rotation)
- **Carga de cartuchos raw `.o`**
- **Carga de BIOS** (512 bytes en $FE00-$FFFF)
- **Joypad**: 8 bits (D-pad + A/B + Opt1/Opt2) + Pause vía teclado
- **Vídeo SDL2 escalado ×4 a 640×408**
- **Audio SDL2** mono float 44.1 kHz
- **Tests E2E**: 60 frames, 4M ciclos CPU, framebuffer no-cero verificado

## Verificación
- Compilación con `-Wall -Wextra -Wpedantic` → 0 errores 0 warnings
- Test head-less: 60 frames OK, CPU avanza 4,000,080 ciclos (= 1s @ 4 MHz),
  framebuffer renderiza datos de RAM con paleta correctamente.
- LNX header parser: título y manufacturer parseados correctamente.

## Backlog / Future work (P1/P2)
- **P1**: Implementar packed/literal RLE completo en `suzy_run_sprites()` para
  juegos comerciales (California Games, Blue Lightning, Chip's Challenge).
- **P1**: Implementar protocolo IODAT/IODIR cycle-accurate del cartucho en
  lugar de la copia directa al arranque.
- **P2**: Detección de colisiones de sprites (CollDepository).
- **P2**: ComLynx (UART) para multijugador.
- **P2**: Save states.
- **P2**: Modo "integrate" del Mikey audio.
- **P2**: Linking de timers (source 7) — actualmente se ignora.

## Build & Run
```bash
sudo apt-get install -y libsdl2-dev cmake
cd /app/lynx && mkdir -p build && cd build
cmake .. && make
./lynx /path/to/lynxboot.img /path/to/game.lnx
```
