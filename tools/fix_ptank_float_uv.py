"""Never compress particle-tank UVs (RenderWare PTank, Cold Fear).

sub_0005AEF0, the PTank Xbox instance callback, switches a tank to 2-byte
compressed UVs (stride 18, writers 0x5AC10 / 0x5ACF0, global 0x37D324 = 1)
when its constant UVs lie in [0,1]. The port's ship scenes then drew those
tanks from vertices in the 24-byte float-UV layout under an 18-byte
declaration: quads stretched across the screen. Per-tank selection of the
format (fix_ptank_flag.py) did not change it -- part of the vertex building
keeps the float layout. The compression is only a memory optimisation, so
both compress sites now take the float-UV path (loc_0005B0DD) instead:
stride 24, float writer, flag 0 -- one layout everywhere. Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_0005AEF0(void)\n{'
SITES = ('loc_0005AFF6: ;\n', 'loc_0005B0C7: ;\n')
TAG = '    goto loc_0005B0DD; /* fix_ptank_float_uv.py: never compress UVs */\n'


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        if TAG in fn_body:
            print('Cold Fear: PTank UVs already never compressed.')
            return
        for lbl in SITES:
            assert fn_body.count(lbl) == 1, lbl
            fn_body = fn_body.replace(lbl, lbl + TAG)
        path.write_text(head + FN + fn_body + '\n}\n' + rest, encoding='utf-8', newline='\n')
        print('Cold Fear: PTank UVs never compressed (float layout only).')
        return
    raise SystemExit('sub_0005AEF0 not found')


if __name__ == '__main__':
    main()
