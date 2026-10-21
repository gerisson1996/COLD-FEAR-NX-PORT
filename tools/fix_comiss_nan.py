"""Make jb/jbe after comiss/ucomiss take the branch on NaN, as the CPU does.

An unordered compare (either operand NaN) sets ZF, PF and CF to 1. The lifter
translated jb as (a < b) and jbe as (a <= b), which C makes false on NaN, so
every such branch went the other way whenever a NaN reached it. Cold Fear's
RenderWare particle instancer (sub_0005AEF0) tests constant UVs against 0 and
1 this way: NaN UVs passed every test, switched on compressed UVs and the
global 0x37D324, and the other tanks were then drawn with stride 18 over
24-byte vertices -- the ship's spikes.

The lifter is fixed too (tools/recomp/lifter.py); this patches C generated
before that. ja/jae are right as they were. Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UN = '(_fca != _fca || _fcb != _fcb)'
SUBS = {
    'if ((_fca < _fcb)) goto':  f'if ((_fca < _fcb || {UN})) goto',
    'if ((_fca <= _fcb)) goto': f'if ((_fca <= _fcb || {UN})) goto',
}


def main():
    total = 0
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        n = sum(text.count(old) for old in SUBS)
        if not n:
            continue
        for old, new in SUBS.items():
            text = text.replace(old, new)
        path.write_text(text, encoding='utf-8', newline='\n')
        total += n
    print(f'Cold Fear: {total} float-compare branches now honour NaN.')


if __name__ == '__main__':
    main()
