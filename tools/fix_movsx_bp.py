"""Sign-extend `movsx r32, bp` in C generated before the lifter fix.

The lifter's movsx listed ax..di as 16-bit sources but not bp/sp, so
`movsx edx, bp` became `edx = LO16(ebp)` -- a zero extension. FMOD's IMA
ADPCM decoder (sub_000931D0, sub_00092FF0) clamps each sample with
`movsx edx, bp; cmp edx, ebp; je ok`: every negative sample failed that
test and was clamped to +32767, so all of Cold Fear's music and effects
(Xbox ADPCM, 1-8 channels) played as saturated static.

Every site below was checked against the XBE (each is `movsx r32, bp`).
Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SITES = {  # function -> (label, register) of each movsx r32, bp
    '000493B0': [('loc_000493E2', 'edx'), ('loc_000494A8', 'edi')],
    '00092FF0': [('loc_0009311E', 'eax')],
    '000931D0': [('loc_0009328F', 'edx')],
    '000A2750': [('loc_000A2DCB', 'ebx')],
    '00256ED0': [('loc_00256F34', 'eax'), ('loc_00256FAD', 'eax')],
    '002570F0': [('loc_00257154', 'eax'), ('loc_002571CD', 'eax')],
}


def main():
    fixed = done = 0
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        changed = False
        for fn, sites in SITES.items():
            head = f'void sub_{fn}(void)\n{{'
            if head not in text:
                continue
            pre, body = text.split(head, 1)
            fn_body, rest = body.split('\n}\n', 1)
            for label, reg in sites:
                at = fn_body.index(label + ': ;\n')
                bad = f'    {reg} = LO16(ebp);\n'
                good = f'    {reg} = SX16(LO16(ebp));\n'
                nxt = fn_body.find('\nloc_', at + 1)
                block = fn_body[at:nxt if nxt > 0 else len(fn_body)]
                if good in block:
                    done += 1
                    continue
                assert block.count(bad) == 1, (fn, label)
                fn_body = fn_body[:at] + block.replace(bad, good) + fn_body[at + len(block):]
                fixed += 1
                changed = True
            text = pre + head + fn_body + '\n}\n' + rest
        if changed:
            path.write_text(text, encoding='utf-8', newline='\n')
    print(f'Cold Fear: movsx r32, bp sign-extends at {fixed + done} sites ({fixed} patched now).')


if __name__ == '__main__':
    main()
