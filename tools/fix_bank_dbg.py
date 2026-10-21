"""Log matches inside the entity-bank lookup sub_001C73B0 (diagnostics).

At the match the lifted code holds: ebx = bank slot (0..7), esi = slot
address (+8 into the 8 x 8-byte slot table), edx = the matched entry's handle
array, ecx = index into it, eax = handle. A handle whose bank byte (>> 24) is
not a real bank means the handle array was overwritten. Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_001C73B0(void)\n{'
A = ('    edx = MEM32(eax + edx * 4 + 8);\n'
     '    eax = MEM32(edx + ecx * 4);\n')
A_NEW = A + '    coldfear_bank_dbg(ebx, esi, edx, ecx, eax, edi); /* fix_bank_dbg.py */\n'
DECL = ('void coldfear_bank_dbg(uint32_t slot, uint32_t slot_va, uint32_t arr,'
        ' uint32_t idx, uint32_t handle, uint32_t key);\n')


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        if 'coldfear_bank_dbg(ebx' in text:
            print('Cold Fear: bank lookup log already present.')
            return
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        assert fn_body.count(A) == 1, 'unexpected sub_001C73B0 shape'
        fn_body = fn_body.replace(A, A_NEW)
        path.write_text(head + DECL + FN + fn_body + '\n}\n' + rest,
                        encoding='utf-8', newline='\n')
        print('Cold Fear: bank lookup log added to sub_001C73B0.')
        return
    raise SystemExit('sub_001C73B0 not found')


if __name__ == '__main__':
    main()
