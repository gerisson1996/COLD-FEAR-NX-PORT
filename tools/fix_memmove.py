"""Replace the title's CRT memmove/memcpy (sub_0003BB00) with a native one.

sub_0003BB00 is MSVC's memmove: rep movsd plus jump tables that sit inside
the code. The disassembler ends the function too early (0x3BDA2), so several
table jumps became indirect calls to "functions" that do not exist
([ICALL] Failed to resolve VA 0x0003BC64/0x3BC84/0x3BCFC/0x3BDDF) and the
1-3 trailing bytes of those copies were silently dropped. memmove semantics
are fixed by the C standard, so a native body is exact: cdecl
(dst, src, n) -> dst, callee pops only its return address.
Idempotent.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN_RE = re.compile(r'^void sub_0003BB00\(void\)\n\{\n.*?\n\}\n', re.M | re.S)
BODY = '''void sub_0003BB00(void)
{
    /* fix_memmove.py: native memmove (see tools/fix_memmove.py). */
    uint32_t sp = recomp_leaf_ld_esp();
    uint32_t dst = MEM32(sp + 4), src = MEM32(sp + 8), n = MEM32(sp + 12);
    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    recomp_leaf_st_eax(dst);
    recomp_leaf_st_esp(sp + 4);   /* ret: pop the return address */
}
'''


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if 'void sub_0003BB00(void)\n{' not in text:
            continue
        if 'fix_memmove.py: native memmove' in text:
            print('Cold Fear: native memmove already present.')
            return
        new, n = FN_RE.subn(BODY, text, count=1)
        if n != 1:
            raise SystemExit('sub_0003BB00: body not found')
        if '#include <string.h>' not in new:
            new = '#include <string.h>\n' + new
        path.write_text(new, encoding='utf-8', newline='\n')
        print('Cold Fear: sub_0003BB00 replaced by native memmove.')
        return
    raise SystemExit('sub_0003BB00 not found')


if __name__ == '__main__':
    main()
