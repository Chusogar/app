---
name: testing-atari-lynx-emulator
description: Guía para probar el arranque y funcionamiento del emulador de Atari Lynx.
---

# Pruebas del Emulador de Atari Lynx

Esta guía describe cómo verificar que el emulador de Atari Lynx arranca correctamente y puede ejecutar juegos comerciales.

## Requisitos Previos

- **BIOS**: Se requiere una imagen de la BIOS de Lynx (típicamente `lynxboot.img`, 512 bytes).
- **ROM**: Un juego en formato `.lnx` o `.o`. Para pruebas de arranque, se recomienda "California Games".

## Proceso de Prueba

### 1. Compilación
El proyecto utiliza CMake. Asegúrate de compilar antes de probar:
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 2. Ejecución Local
Para probar con interfaz gráfica:
```bash
./lynx path/to/lynxboot.img path/to/game.lnx
```

### 3. Verificación de Arranque (Adversarial)
Si el emulador parece quedarse en pantalla negra, verifica lo siguiente mediante trazas de depuración en `lynx_run_frame`:

- **PC (Program Counter)**: Debe avanzar de `$FF80` (reset) a `$FE00` (BIOS) y finalmente al rango `$0300-$0400` (código del juego tras desencriptación).
- **Cartridge Address**: Durante el arranque, la dirección del cartucho debe avanzar. Si se queda en `$00000`, el protocolo de strobes (`SYSCTL1`) o el registro de desplazamiento podrían estar fallando.
- **Sprites**: Verifica `l->suzy.sprites_drawn`. Si es mayor que 0, el sistema de video y Suzy están procesando datos.

## Secretos de Devin Necesarios
- No se requieren secretos externos para este repositorio más allá del acceso estándar al sistema de archivos y visualización.

## Consejos y Solución de Problemas
- **Shift Count**: Si el juego no valida el checksum, verifica que el `shift_count` sea correcto para el tamaño del cartucho (8 para 64KB, 9 para 128KB, etc.).
- **MAPCTL**: El registro `$FFF9` controla qué memoria es visible. Si el arranque falla temprano, asegúrate de que la BIOS pueda ver el hardware de Mikey y Suzy.
