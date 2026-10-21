"""Decide each particle tank's vertex format from the tank, not a global.

RenderWare's PTank Xbox pipeline (Cold Fear's rain, sparks, smoke) instances
a tank in sub_0005AEF0: it picks compressed UVs (stride 18, writers 0x5AC10 /
0x5ACF0) or float UVs (stride 24, 0x5E070 / 0x5E260), stores the writer in
the tank's plugin data [plugin+0x2C] and sets the GLOBAL 0x37D324. The render
callback sub_0005A590 builds the vertex format from that global. When a tank
is drawn after another tank was instanced, it gets the other tank's format:
24-byte vertices declared as 18 -- the quads stretched across the ship.

loc_0005A61E reads the global; it now asks coldfear_ptank_compressed(),
which answers from this tank's own writer (edx still holds the object
there; plugin = [[0x37D31C] + object], as both callbacks compute it), and
falls back to the global for an unknown writer. Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_0005A590(void)\n{'
OLD = 'loc_0005A61E: ;\n    eax = MEM32(0x37D324);\n'
NEW = ('loc_0005A61E: ;\n'
       '    eax = coldfear_ptank_compressed(edx, MEM32(0x37D324)); /* fix_ptank_flag.py */\n')
DECL = 'uint32_t coldfear_ptank_compressed(uint32_t obj, uint32_t global_flag);\n'


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        if 'fix_ptank_flag.py' in text:
            print('Cold Fear: PTank format already per tank.')
            return
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        assert fn_body.count(OLD) == 1, 'unexpected sub_0005A590 shape'
        fn_body = fn_body.replace(OLD, NEW)
        path.write_text(head + DECL + FN + fn_body + '\n}\n' + rest,
                        encoding='utf-8', newline='\n')
        print('Cold Fear: PTank vertex format now decided per tank.')
        return
    raise SystemExit('sub_0005A590 not found')


if __name__ == '__main__':
    main()
