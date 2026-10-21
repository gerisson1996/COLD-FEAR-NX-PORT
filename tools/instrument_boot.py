"""Bounded call tracing around the graphics-init chain; preserves guest semantics.

Run after fix_generated.py. Idempotent, and resolves functions by VA rather than
chunk index so regeneration cannot silently instrument a different function.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    '002415A0', '00221B20', '0021C810', '0021E180', '0021EA40',
    '00025C50', '000321A0', '00021BF0', '00262610', '002625D0',
    '002624C0', '00242CC0',
}

def main():
    found = set()
    calls = 0
    for path in (ROOT / 'gen').glob('recomp_*.c'):
        original = path.read_text(encoding='utf-8-sig')
        def instrument(match):
            nonlocal calls
            va, body = match.group(1), match.group(0)
            if va not in TARGETS:
                return body
            assert va not in found, f'duplicate function {va}'
            found.add(va)
            body = body.replace('RECOMP_ABI_CALL(', 'coldfear_trace_call(')
            calls += body.count('coldfear_trace_call(')
            return body
        updated = re.sub(r'void sub_([0-9A-F]{8})\(void\)\n\{.*?\n\}',
                         instrument, original, flags=re.S)
        if 'coldfear_trace_call(' in updated and '#include "coldfear_trace.h"' not in updated:
            updated = updated.replace('#include "recomp_funcs.h"',
                '#include "recomp_funcs.h"\n#include "coldfear_trace.h"', 1)
        if updated != original:
            path.write_text(updated, encoding='utf-8', newline='\n')
    assert found == TARGETS, f'missing functions: {TARGETS - found}'
    assert calls > 0
    print(f'Boot tracing: {len(found)} functions, {calls} direct call sites; limit 2048 calls.')

if __name__ == '__main__':
    main()
