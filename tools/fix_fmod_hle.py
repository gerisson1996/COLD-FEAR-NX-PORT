"""Rename the FMOD stream entry points so src/fmod_hle.c can wrap them.

Each generated sub_X becomes sub_X_orig; fmod_hle.c defines sub_X, which
the dispatch table and every direct call then reach. See fmod_hle.c for why
(Cold Fear's music/ambience streams come out of FMOD as static) and what
each function is. Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VAS = ['0007F250', '0007F3B0', '00080190', '0007FB30', '00080310',
       '000804A0', '0007FE50', '0007F770', '0007CC40']


def main():
    renamed = present = 0
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        changed = False
        for va in VAS:
            old = f'\nvoid sub_{va}(void)\n{{'
            new = f'\nvoid sub_{va}_orig(void)\n{{'
            if new in text:
                present += 1
            elif old in text:
                text = text.replace(old, new, 1)
                renamed += 1
                changed = True
        if changed:
            path.write_text(text, encoding='utf-8', newline='\n')
    total = renamed + present
    if total != len(VAS):
        raise SystemExit(f'expected {len(VAS)} FMOD functions, found {total}')
    print(f'Cold Fear: FMOD stream functions wrapped ({renamed} renamed now).')


if __name__ == '__main__':
    main()
