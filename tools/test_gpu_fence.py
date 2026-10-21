"""Exercise real fence registration and semaphore-release code with guest RAM.

Host test only: no renderer/console execution. The functions under test are
extracted verbatim from the vendored runtime; device addresses come from main.c.
"""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
memory = (ROOT / 'xboxrecomp/src/kernel/xbox_memory_layout.c').read_text()
executor = (ROOT / 'xboxrecomp/src/kernel/nv2a_pb_exec.c').read_text()
main = (ROOT / 'src/main.c').read_text(encoding='utf-8-sig')
config = re.search(r'xbox_Nv2aMirrorFence\((0x[\dA-Fa-f]+)u, (0x\w+), (0x\w+)\)', main)
assert config, 'Cold Fear must register its D3D fence'
device, put, get = [int(value, 16) for value in config.groups()]
assert (device, put, get) == (0x273398, 0x2C, 0x30)

def block(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 1
    cursor = opening + 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor]

registration = memory[memory.index('#define XBOX_MAX_FENCE_MIRRORS'):memory.index('/* A guest address is usable')]
source = r'''
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#define XBOX_FS_BASE 0x4000u
#define XBOX_CONTIG_BASE 0x80000000u
#define XBOX_CONTIG_SIZE 0x04000000u
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE 0x01000000u
static void *g_memory_base, *g_contig_memory, *g_nv2a_memory;
static size_t g_memory_size = 0x00400000;
static uintptr_t g_memory_offset;
static uint32_t s_sem_va, s_vtx_epoch;
static uintptr_t xbox_GetMemoryOffset(void) { return g_memory_offset; }
void nv2a_pb_set_semaphore_target(uint32_t va) { s_sem_va = va; }
#define WORD(va) (*(uint32_t *)(g_memory_offset + (va)))
'''
source += registration + '\n' + block(memory, 'static int fence_readable(uint32_t va, uint32_t bytes)\n{')
source += '\n' + block(memory, 'static void fence_mirrors_tick(')
source += '\nstatic void gpu_release(uint32_t method, uint32_t param) {\n'
source += block(executor, 'if (method == 0x1D70) {') + '\n}\n'
source += f'#define DEVICE_PTR {device}u\n#define PUT_OFFSET {put}u\n#define GET_OFFSET {get}u\n'
source += r'''
int main(void) {
    g_memory_base = calloc(1, g_memory_size);
    assert(g_memory_base);
    g_memory_offset = (uintptr_t)g_memory_base;
    setenv("RECOMP_PB_EXEC", "1", 1);
    assert(xbox_Nv2aMirrorFence(DEVICE_PTR, PUT_OFFSET, GET_OFFSET) == 0);
    fence_mirrors_tick();  /* device not yet constructed */
    assert(s_sem_va == 0);
    WORD(DEVICE_PTR) = 0x2733A0;
    fence_mirrors_tick();  /* semaphore not yet allocated */
    assert(s_sem_va == 0);
    WORD(0x2733A0 + GET_OFFSET) = 0x300000;
    WORD(0x2733A0 + PUT_OFFSET) = 11;
    WORD(0x300000) = 10;
    fence_mirrors_tick();
    assert(s_sem_va == 0x300000);
    assert(WORD(0x300000) == 10); /* registration must NOT fake completion */
    gpu_release(0x1D70, 11);
    assert(WORD(0x300000) == 11 && s_vtx_epoch == 1);
    /* The unsigned comparison in Cold Fear's loc_00267C97 now terminates. */
    assert(!((11u - 11u) < (11u - WORD(0x300000))));
    WORD(0x2733A0 + GET_OFFSET) = 0x300004; /* device recreates its semaphore */
    WORD(0x300004) = 8;
    fence_mirrors_tick();
    assert(s_sem_va == 0x300004 && WORD(0x300004) == 8);
    gpu_release(0x1D70, 12);
    assert(WORD(0x300004) == 12 && WORD(0x300000) == 11);
    free(g_memory_base);
    puts("PASS: delayed device, semaphore registration, no premature completion, GPU release, relocation");
    return 0;
}
'''
out = ROOT / 'analysis/fence-test'
out.mkdir(parents=True, exist_ok=True)
(out / 'fence_test.c').write_text(source, encoding='utf-8')
subprocess.run(['C:/devkitPro/msys2/usr/bin/gcc.exe', '-std=gnu11', '-O1',
                '-Wall', '-Wextra', 'fence_test.c', '-o', 'fence_test.exe'], cwd=out, check=True)
subprocess.run([str(out / 'fence_test.exe')], cwd=out, check=True)
