"""Compile the actual dispatch with inert guest functions and exercise all lookups.

No Xbox instructions are executed. Checks binary search, flat-table init,
idempotent init, every registered address, holes and range boundaries.
"""
import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--cc', default='gcc')
args = ap.parse_args()
source = (ROOT / 'gen/recomp_dispatch.c').read_text(encoding='utf-8-sig')
names = sorted(set(re.findall(r'\(recomp_func_t\)(\w+)\s*}', source)))
prefix = '#include <stdint.h>\n#include <assert.h>\n#include <stdio.h>\n'
prefix += '\n'.join(f'static void {name}(void) {{}}' for name in names) + '\n'
source = source.replace('#include "recomp_funcs.h"', prefix)
source += r'''
static void verify_lookups(void) {
    size_t n = sizeof(g_recomp_table) / sizeof(g_recomp_table[0]);
    for (size_t i = 0; i < n; ++i) {
        assert(recomp_lookup(g_recomp_table[i].xbox_va) == g_recomp_table[i].func);
        if (i && g_recomp_table[i].xbox_va > g_recomp_table[i-1].xbox_va + 1)
            assert(recomp_lookup(g_recomp_table[i-1].xbox_va + 1) == NULL);
    }
    assert(recomp_lookup(0) == NULL);
    assert(recomp_lookup(g_flat_base - 1) == NULL);
    assert(recomp_lookup(g_flat_base + g_flat_span) == NULL);
    assert(recomp_lookup(UINT32_MAX) == NULL);
    assert(recomp_lookup(0x00211062) == NULL);
}
int main(void) {
    size_t n = sizeof(g_recomp_table) / sizeof(g_recomp_table[0]);
    assert(recomp_get_count() == n);
    assert(n == 13258);
    assert(recomp_dispatch_flat_bytes() == 0);
    for (size_t i = 1; i < n; ++i)
        assert(g_recomp_table[i-1].xbox_va < g_recomp_table[i].xbox_va);
    verify_lookups();
    assert(recomp_dispatch_init() == 1);
    verify_lookups();
    recomp_func_t *first = g_flat_table;
    assert(recomp_dispatch_init() == 1 && g_flat_table == first);
    assert(recomp_dispatch_flat_bytes() == (size_t)g_flat_span * sizeof(recomp_func_t));
    free(g_flat_table);
    g_flat_table = NULL;
    printf("PASS: %zu entries, binary/flat lookup, holes, boundaries, repeat init\n", n);
    return 0;
}
'''
output = ROOT / 'analysis/dispatch-test'
output.mkdir(parents=True, exist_ok=True)
(output / 'dispatch_test.c').write_text(source, encoding='utf-8')
subprocess.run([args.cc, '-std=c11', '-O1', 'dispatch_test.c', '-o', 'dispatch_test.exe'],
               cwd=output, check=True)
subprocess.run([str(output / 'dispatch_test.exe')], cwd=output, check=True)
