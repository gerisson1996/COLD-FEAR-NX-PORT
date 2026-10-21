"""Remove a proven gap-prologue false positive for this exact Cold Fear XBE."""
import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = '7371e422ecd0ac5c95ebaa343a6a9c3d504f2f261a53ed8acf212f258ab377ef'


def main():
    digest = hashlib.sha256((ROOT / 'game/default.xbe').read_bytes()).hexdigest()
    if digest != EXPECTED:
        raise SystemExit('Unknown XBE: do not apply the 0x00211062 correction to another edition.')
    # 0x211062: 8B FF alignment, then four uint32 jump targets at 0x211064.
    # sub_00210EE0 uses MEM32(eax * 4 + 0x211064); it ends in RET at 0x211061.
    # There are no static callers to this false function. Keep the XBE data intact.
    count = 0
    for path in (ROOT / 'gen').glob('recomp_*.c'):
        original = path.read_text(encoding='utf-8-sig')
        text, removed = re.subn(
            r'/\*\*\n \* sub_00211062\n.*?\n}\n', '', original, flags=re.S)
        count += removed
        text = re.sub(r'^    \{ 0x00211062u, \(recomp_func_t\)sub_00211062 \},\n',
                      '', text, flags=re.M)
        # Removing an entry also changes the table length. Derive it from the
        # array so init and binary search cannot walk past the final entry.
        text = re.sub(r'static const size_t g_recomp_table_size = \d+;',
                      'static const size_t g_recomp_table_size = '
                      'sizeof(g_recomp_table) / sizeof(g_recomp_table[0]);', text)
        if text != original:
            path.write_text(text, encoding='utf-8', newline='\n')
    if count > 1:
        raise SystemExit('Unexpected duplicate generated function')
    print(f'Cold Fear: removed {count} false function(s); XBE jump-table bytes unchanged.')


if __name__ == '__main__':
    main()
