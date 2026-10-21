"""Acknowledge DirectSound's DSP command at once (Cold Fear).

sub_002CC036 (XDK DSOUND, DSP image setup) writes command 3 to the DSP
mailbox at [dsp_scratch + 0x810] and spins until the DSP firmware clears it.
The runtime's APU DSP is a passthrough stub that never runs that firmware, so
the title hung at boot inside DirectSound init. Clearing the mailbox right
after the write is what a DSP that finished instantly would do.
This was a hand edit in the old gen/ (lost on regeneration); now a script.
Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FN = 'void sub_002CC036(void)\n{'
A = '    MMIO_WR32(ebx, (eax));\n\nloc_002CC19D: ;\n'
A_NEW = ('    MMIO_WR32(ebx, (eax));\n'
         '    MMIO_WR32(ebx, 0); /* fix_dsp_ack.py: stub DSP completes the command at once */\n'
         '\nloc_002CC19D: ;\n')


def main():
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if FN not in text:
            continue
        head, body = text.split(FN, 1)
        fn_body, rest = body.split('\n}\n', 1)
        if 'fix_dsp_ack.py' in fn_body:
            print('Cold Fear: DSP ack already present.')
            return
        if fn_body.count(A) != 1:
            raise SystemExit('sub_002CC036: unexpected shape of the DSP mailbox wait')
        path.write_text(head + FN + fn_body.replace(A, A_NEW) + '\n}\n' + rest,
                        encoding='utf-8', newline='\n')
        print('Cold Fear: DSP mailbox acknowledged in sub_002CC036.')
        return
    raise SystemExit('sub_002CC036 not found')


if __name__ == '__main__':
    main()
