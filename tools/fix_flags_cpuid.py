"""Turn the untranslated pushfd / popfd / cpuid sites into real calls.

They were RECOMP_UNIMPL no-ops. pushfd/popfd as no-ops unbalance the stack:
FMOD's CPU check (sub_000A6320: pushfd; pop eax; ...; popfd; pushfd; pop eax)
popped its own saved registers one slot off and returned ebx = a stack
address. FSOUND_Init uses ebx as 0, so a failed channel-array allocation
passed as success, every play failed with error 2 and the game was silent.

The runtime implements them (xbox_memory_layout.c) on the global registers,
which is what the surrounding spill/reload already provides. The lifter
emits the same calls now; this patches C generated before. Idempotent.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITE = re.compile(r'RECOMP_UNIMPL\("(pushfd|popfd|cpuid)", 0x[0-9A-F]{8}u\);')


def main():
    total = 0
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        text, n = SITE.subn(lambda m: f'recomp_{m.group(1)}();', text)
        if n:
            path.write_text(text, encoding='utf-8', newline='\n')
            total += n
    print(f'Cold Fear: {total} pushfd/popfd/cpuid sites now call the runtime.')


if __name__ == '__main__':
    main()
