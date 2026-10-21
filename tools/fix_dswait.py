"""Hook DirectSound's voice-stop wait loop, sub_002CF73D (performance).

The loop spins while a stopping voice's flag 0x8000 is set, calling
sub_002CEF25 (KeQuerySystemTime, a hardware poll and a 0.5 s timeout) every
turn -- ~200,000 times a second on Cold Fear's ship, all on the main thread.
coldfear_dswait_begin/iter/end (src/recomp_manual.c) measure the waits and,
with COLDFEAR_DSWAIT=yield, hand the guest lock over on every turn.
Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_002CF73D(void)\n{'
TAG = '/* fix_dswait.py */'
SITES = (
    ('loc_002CF73D: ;\n', f'    coldfear_dswait_begin(); {TAG}\n'),
    ('loc_002CF748: ;\n', f'    coldfear_dswait_iter(); {TAG}\n'),
    ('loc_002CF757: ;\n', f'    coldfear_dswait_end(); {TAG}\n'),
)
DECL = ('void coldfear_dswait_begin(void);\nvoid coldfear_dswait_iter(void);\n'
        'void coldfear_dswait_end(void);\n')

# sub_002CEF25's give-up time: a releasing voice (flags 0x8001) older than
# 0x4C4B40 x 100 ns (0.5 s) is stopped by force (sub_002CEE3C). The constant
# becomes coldfear_dswait_timeout() (COLDFEAR_DSWAIT_TIMEOUT_US, stock 0.5 s).
TFN = 'void sub_002CEF25(void)\n{'
TTAG = '/* fix_dswait.py timeout */'
TOLD = ('    _fb = (uint32_t)(0x4C4B40) & 0xFFFFFFFFu; _fbs = (int32_t)(int32_t)(_fb); /* add source, before the write */\n'
        '    _cf = (int)((((uint64_t)(ecx) + (uint64_t)(0x4C4B40)) >> 32) & 1);\n'
        '    ecx = ecx + 0x4C4B40;\n')
TNEW = ('    { uint32_t _to = coldfear_dswait_timeout(); ' + TTAG + '\n'
        '    _fb = _to; _fbs = (int32_t)_fb;\n'
        '    _cf = (int)((((uint64_t)(ecx) + (uint64_t)(_to)) >> 32) & 1);\n'
        '    ecx = ecx + _to; }\n')
TDECL = 'uint32_t coldfear_dswait_timeout(void);\n'


def patch_timeout():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if TFN not in text:
            continue
        head, body = text.split(TFN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        if TTAG in fn_body:
            return
        assert fn_body.count(TOLD) == 1, 'unexpected sub_002CEF25 shape'
        fn_body = fn_body.replace(TOLD, TNEW)
        path.write_text(head + TDECL + TFN + fn_body + '\n}\n' + rest, encoding='utf-8', newline='\n')
        print('Cold Fear: DirectSound voice-stop timeout made configurable.')
        return
    raise SystemExit('sub_002CEF25 not found')


def main():
    patch_timeout()
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        if TAG in fn_body:
            print('Cold Fear: DirectSound voice-stop wait already hooked.')
            return
        for label, call in SITES:
            assert fn_body.count(label) == 1, label
            fn_body = fn_body.replace(label, label + call)
        path.write_text(head + DECL + FN + fn_body + '\n}\n' + rest, encoding='utf-8', newline='\n')
        print('Cold Fear: DirectSound voice-stop wait hooked.')
        return
    raise SystemExit('sub_002CF73D not found')


if __name__ == '__main__':
    main()
