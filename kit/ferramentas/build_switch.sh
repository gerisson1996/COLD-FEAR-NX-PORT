#!/usr/bin/env bash
# Compila o .nro de um jogo. Rode no bash do devkitPro (C:\devkitPro\msys2\usr\bin\bash.exe -l):
#
#   cd /c/XboxSwitchPortKit && ./ferramentas/build_switch.sh exemplos/black
#
# Variaveis opcionais:
#   JOBS=8              quantos arquivos compilar ao mesmo tempo (padrao 6; cada um usa 1-2 GB de RAM)
#   TARGET=host_check   so o host e o runtime, sem o codigo recompilado: acha erros de compilacao rapido
#   BUILD_DIR=...       pasta do build (padrao build/<nome da pasta do jogo>)
#
# Saida: build/<jogo>/<PORT_NAME>.nro e o .elf (que ferramentas/endereco_crash.sh le).
set -euo pipefail
if [ $# -lt 1 ]; then echo "uso: $0 <pasta do jogo>"; exit 1; fi
KIT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$(cd "$1" && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
BUILD="${BUILD_DIR:-$KIT/build/$(basename "$GAME")}"
cmake -S "$KIT/host-switch" -B "$BUILD" -G Ninja \
    -DPORT_GAME_DIR="$GAME" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release
if [ -n "${TARGET:-}" ]; then
    ninja -C "$BUILD" -j"${JOBS:-6}" "$TARGET"
else
    ninja -C "$BUILD" -j"${JOBS:-6}"
fi
ls -la "$BUILD"/*.nro 2>/dev/null || true
