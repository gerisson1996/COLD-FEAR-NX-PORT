"""Log the entity-bank lookup in sub_00147800 (Cold Fear diagnostics).

sub_00147800 looks an entity up with sub_001C73B0, decodes the handle into a
20-byte bank record and clones the clump in record[0]. When that clump is
NULL the clone walks a zero page and faults at guest 0xFFFFFFC0. This logs
key, handle and record so the failing entity can be named.
Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_00147800(void)\n{'
A = 'loc_0014784A: ;\n'
A_NEW = A + '    uint32_t _cf_handle = eax; /* fix_entity_dbg.py */\n'
B = '    eax = ecx + eax * 4;\n    PUSH32(esp, edx);\n    MEM32(edx + 0x18) = 0x3FA5A8;\n'
B_NEW = ('    eax = ecx + eax * 4;\n'
         '    coldfear_entity_dbg(_cf_handle, eax, MEM32(esp), MEM32(esp + 4), esp, esi); /* fix_entity_dbg.py */\n'
         '    PUSH32(esp, edx);\n    MEM32(edx + 0x18) = 0x3FA5A8;\n')
DECL = 'void coldfear_entity_dbg(uint32_t handle, uint32_t rec, uint32_t pkey1, uint32_t pkey2, uint32_t sp, uint32_t this_);\n'


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        if 'coldfear_entity_dbg(_cf_handle' in text:
            print('Cold Fear: entity lookup log already present.')
            return
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        assert fn_body.count(A) == 1 and fn_body.count(B) == 1, 'unexpected sub_00147800 shape'
        fn_body = fn_body.replace(A, A_NEW).replace(B, B_NEW)
        E = 'loc_00147800: ;\n'
        assert fn_body.count(E) == 1, 'sub_00147800 entry label'
        fn_body = fn_body.replace(E, E + '    coldfear_entity_entry(esp, eax); /* fix_entity_dbg.py */\n')
        path.write_text(head + DECL + 'void coldfear_entity_entry(uint32_t sp, uint32_t this_);\n' + FN + fn_body + '\n}\n' + rest,
                        encoding='utf-8', newline='\n')
        print('Cold Fear: entity lookup log added to sub_00147800.')
        return
    raise SystemExit('sub_00147800 not found')


if __name__ == '__main__':
    main()
