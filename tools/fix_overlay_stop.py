"""Let the overlay stop complete at once (Cold Fear, XDK D3D).

sub_00270B60 (D3D overlay disable) writes NV_PVIDEO_STOP (0x8704) and spins
until NV_PVIDEO_BUFFER (0x8700) reads 0 -- the hardware clears it once the
overlay has stopped. The runtime's NV2A register aperture is plain memory and
nothing clears it, so the title hung right after the Ubisoft logo movie.
Clearing BUFFER together with the STOP write is what the hardware does.
Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_00270B60(void)\n{'
A = '    MEM32(eax + 0x8704) = 1;\n'
A_NEW = A + '    MEM32(eax + 0x8700) = 0; /* fix_overlay_stop.py: overlay stops at once */\n'


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        if 'fix_overlay_stop.py' in fn_body:
            print('Cold Fear: overlay stop already patched.')
            return
        if fn_body.count(A) != 1:
            raise SystemExit('sub_00270B60: unexpected shape')
        path.write_text(head + FN + fn_body.replace(A, A_NEW) + '\n}\n' + rest,
                        encoding='utf-8', newline='\n')
        print('Cold Fear: overlay stop completes at once in sub_00270B60.')
        return
    raise SystemExit('sub_00270B60 not found')


if __name__ == '__main__':
    main()
