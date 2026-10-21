"""Translate the `cmpltss xmmA, xmmB` sites the lifter left as RECOMP_UNIMPL.

cmpltss writes an all-ones/all-zero mask into lane 0 of dst. Cold Fear uses it
in the branch-free safe normalize (mask = 0 < len2; r = rsqrt(len2) & mask);
as a no-op the mask stayed 0 and every normalized vector came out as zero.
The lifter now emits this itself; this script fixes an already-generated gen/.
Idempotent: a second run finds nothing to change.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Whole bracket: leaf stores; RECOMP_UNIMPL(...); leaf loads; /* TODO ... */
PAT = re.compile(
    r'(?:recomp_leaf_st_\w+\(g_\w+\); )*'
    r'RECOMP_UNIMPL\("cmpltss (xmm[0-7]), (xmm[0-7])", 0x([0-9A-F]{8})u\);'
    r'(?: g_\w+ = recomp_leaf_ld_\w+\(\);)*'
    r' /\* TODO: cmpltss xmm[0-7], xmm[0-7] \*/')


def repl(mo):
    a, b, va = mo.groups()
    return (f'{a}.u[0] = ({a}.f[0] < {b}.f[0]) ? 0xFFFFFFFFu : 0u;'
            f' /* cmpltss @0x{va} */')


def main():
    total = 0
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        new, n = PAT.subn(repl, text)
        if n:
            path.write_text(new, encoding='utf-8', newline='\n')
            total += n
    left = sum(p.read_text(encoding='utf-8-sig').count('RECOMP_UNIMPL("cmpltss')
               for p in (ROOT / 'gen').glob('recomp_*.c'))
    print(f'Cold Fear: translated {total} cmpltss site(s); {left} left.')
    if left:
        raise SystemExit('cmpltss sites with an unexpected shape remain')


if __name__ == '__main__':
    main()
