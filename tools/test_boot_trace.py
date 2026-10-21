"""Compile the real trace wrapper with mock registers; verify call semantics."""
from pathlib import Path
import subprocess
import hashlib
import instrument_boot

ROOT = Path(__file__).resolve().parents[1]
files = list((ROOT / 'gen').glob('recomp_*.c'))
before = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
instrument_boot.main()
assert all(hashlib.sha256(p.read_bytes()).digest() == h for p, h in before.items())
out = ROOT / 'analysis/trace-test'
out.mkdir(exist_ok=True)
source = (ROOT / 'src/coldfear_trace.c').read_text().replace('#include "recomp_types.h"', r'''
#include <stdint.h>
#include <assert.h>
static uint32_t g_eax, g_ebx, g_esi, g_edi, g_esp;
static uint32_t ram[0x1000000 / 4];
#define MEM32(va) ram[(va) / 4]
''').replace('#include "coldfear_trace.h"', '')
source += r'''
static unsigned calls;
static void child(void) {
    ++calls;
    g_eax = 42;
    g_edi = 0x6e656572; /* Intentionally retain a bad guest result for diagnosis. */
    g_esp += 8;
}
static void parent(void) { coldfear_trace_call(0x2222, child); }
int main(void) {
    g_esp = 0xf7fa00;
    MEM32(g_esp) = 0x24178f;
    coldfear_trace_call(0x1111, parent);
    assert(calls == 1 && g_eax == 42 && g_edi == 0x6e656572);
    assert(g_esp == 0xf7fa08 && MEM32(0xf7fa00) == 0x24178f);
    trace_count = 2048;
    g_esp = 0xffffffff; /* Capped trace must not dereference the guest stack. */
    coldfear_trace_call(0x3333, child);
    assert(calls == 2 && g_esp == 7);
    puts("PASS: nested call, unchanged guest result, stack delta, trace cap, idempotence");
}
'''
(out / 'trace_test.c').write_text(source)
subprocess.run(['C:/devkitPro/msys2/usr/bin/gcc.exe', '-std=c11', '-Wall', '-Wextra',
                'trace_test.c', '-o', 'trace_test.exe'], cwd=out, check=True)
subprocess.run([str(out / 'trace_test.exe')], cwd=out, check=True)
