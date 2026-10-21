"""Hand Xbox D3D's gamma ramp to the Vulkan presenter (Cold Fear).

sub_0026BF76 writes the ramp at [esp+4] (256 red, 256 green, 256 blue bytes)
into the DAC palette; the register aperture is plain memory in this
runtime, so at its entry coldfear_gamma_ramp (src/recomp_manual.c) passes the
ramp to nv2a_vk_set_gamma_ramp. Without it the game's frames went out
without the brightening it asks for. Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_0026BF76(void)\n{'
TAG = '/* fix_gamma_ramp.py */'
LABEL = 'loc_0026BF76: ;\n'
DECL = 'void coldfear_gamma_ramp(uint32_t ramp_va);\n'


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        if TAG in fn_body:
            print('Cold Fear: gamma ramp hook already in place.')
            return
        assert fn_body.count(LABEL) == 1
        fn_body = fn_body.replace(LABEL, LABEL + f'    coldfear_gamma_ramp(MEM32(esp + 4)); {TAG}\n')
        path.write_text(head + DECL + FN + fn_body + '\n}\n' + rest, encoding='utf-8', newline='\n')
        print('Cold Fear: gamma ramp hook added.')
        return
    raise SystemExit('sub_0026BF76 not found')


if __name__ == '__main__':
    main()
