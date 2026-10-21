"""Log each RenderWare camera's matrices at the view-row upload (diagnostics).

Hooks the entry of sub_00265100, the D3D constant setter (fastcall: ecx =
hardware constant index, edx = data); coldfear_camlog (src/recomp_manual.c)
logs the current camera when c[163] is written. Idempotent; remove with the
other diagnostics once the flashlight is understood.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_00265100(void)\n{'
TAG = '/* fix_camlog_dbg.py */'
LABEL = 'loc_00265100: ;\n'
DECL = 'void coldfear_camlog(uint32_t index, uint32_t data);\n'


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        if TAG in fn_body:
            print('Cold Fear: camera log already hooked.')
            return
        assert fn_body.count(LABEL) == 1
        fn_body = fn_body.replace(LABEL, LABEL + f'    coldfear_camlog(ecx, edx); {TAG}\n')
        path.write_text(head + DECL + FN + fn_body + '\n}\n' + rest, encoding='utf-8', newline='\n')
        print('Cold Fear: camera log hooked.')
        return
    raise SystemExit('sub_00265100 not found')


if __name__ == '__main__':
    main()
