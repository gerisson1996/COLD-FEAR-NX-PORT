"""Report retired flips from D3D's vblank handler to the runtime.

sub_0026DDB0 is the XDK D3D vblank flip handler: it returns in eax (=ebp)
the number of flips it retired this vblank. The pushbuffer executor's
FLIP_STALL waits on that count (nv2a_pb_flip_retired in nv2a_pb_exec.c);
without the hook the executor never learns a flip retired. This hook was a
hand edit in the old gen/ that a regeneration erased -- now it is a script.
Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_0026DDB0(void)\n{'
A = '    eax = ebp;\n    POP32(esp, ebp);\n'
A_NEW = '    eax = ebp;\n    nv2a_pb_flip_retired(eax); /* fix_flip_hook.py */\n    POP32(esp, ebp);\n'
DECL = 'extern void nv2a_pb_flip_retired(uint32_t retired);\n'


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        if 'fix_flip_hook.py' in fn_body:
            print('Cold Fear: flip hook already present.')
            return
        n = fn_body.count(A)
        if n != 2:
            raise SystemExit(f'sub_0026DDB0: expected 2 returns, found {n}')
        fn_body = fn_body.replace(A, A_NEW)
        path.write_text(head + DECL + FN + fn_body + '\n}\n' + rest,
                        encoding='utf-8', newline='\n')
        print('Cold Fear: flip hook added to sub_0026DDB0 (2 returns).')
        return
    raise SystemExit('sub_0026DDB0 not found')


if __name__ == '__main__':
    main()
