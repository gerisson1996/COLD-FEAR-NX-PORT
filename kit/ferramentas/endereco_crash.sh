#!/usr/bin/env bash
# Traduz as linhas "crash:" de um log do console para funcoes do programa. Rode no bash do devkitPro:
#
#   ./ferramentas/endereco_crash.sh analise/black-0C2BFA28.elf black_001.log
#
# O log tem "crash: exception ... at pc X lr Y" e "crash: main at Z". O programa e carregado num endereco
# diferente a cada vez: endereco no .elf = endereco no log - Z + endereco de main no .elf. Use o .elf do
# MESMO build que gerou o log (empacotar.ps1 guarda cada um com o SHA-256 no nome).
#
# Num crash dentro do codigo do jogo (x86_recomp.*) a funcao aparece como sub_XXXXXXXX, o endereco no xbe
# original, que da para abrir no Ghidra ou no IDA. "crash: guest address" e a pilha do guest ja vem em
# enderecos do xbe.
set -euo pipefail
if [ $# -lt 2 ]; then echo "uso: $0 <programa.elf> <log>"; exit 1; fi
ELF="$1"; LOG="$2"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/devkitA64/bin:$PATH"

hex_after() { grep -o "$1 *\(0x\)\?[0-9A-Fa-f]\{8,16\}" "$LOG" | tail -1 | grep -o '[0-9A-Fa-f]\{8,16\}$' || true; }
main_log=$(hex_after 'main at')
if [ -z "$main_log" ]; then echo "o log nao tem 'crash: main at': nao houve crash registrado"; exit 1; fi
main_elf=$(aarch64-none-elf-nm "$ELF" | awk '$3 == "main" {print $1}')
echo "main: log 0x$main_log, elf 0x$main_elf"
show() {
    local a
    a=$(printf '%x' $(( 0x$2 - 0x$main_log + 0x$main_elf )))
    echo "$1 0x$2 -> elf 0x$a: $(aarch64-none-elf-addr2line -f -C -e "$ELF" "0x$a" | head -1)"
}
for kind in pc lr; do
    v=$(hex_after "$kind")
    [ -n "$v" ] && show "$kind" "$v"
done
# abort() vindo do driver (Mesa/NVK) ou da libc
v=$(hex_after 'abort() called from')
[ -n "$v" ] && show "abort de" "$v"
# no codigo do jogo: o endereco do Xbox acessado, os registradores x86 e os enderecos de retorno na pilha (do xbe)
grep 'crash: guest address\|crash: on guest thread' "$LOG" | tail -2 || true
grep -A40 'crash: on guest thread' "$LOG" | grep '  eax \|  \[esp+' | head -12 || true
