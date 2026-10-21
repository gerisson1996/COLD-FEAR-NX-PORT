"""Route Cold Fear's own debug log (sub_00242B10) into coldfear_log.txt.

sub_00242B10 is the game's log sink: edi = formatted message, esi = level
(0..3; the error wrapper sub_00242BC0 passes 2 with an "ERROR: " prefix).
Retail builds send it nowhere useful, which hid messages like
"Can't find v-table of entity number %d in any bank." right before a crash.
Idempotent: a second run finds the hook already there.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LABEL = 'loc_00242B10: ;\n'
HOOK = '    coldfear_game_log(esi, edi); /* fix_game_log.py */\n'
DECL = 'void coldfear_game_log(uint32_t level, uint32_t msg_va);\n'


def main():
    done = 0
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        if 'void sub_00242B10(void)\n{' not in text:
            continue
        if HOOK in text:
            print('Cold Fear: game log hook already present.')
            return
        assert text.count(LABEL) == 1, 'unexpected sub_00242B10 shape'
        text = text.replace(LABEL, LABEL + HOOK)
        text = text.replace('void sub_00242B10(void)\n{', DECL + 'void sub_00242B10(void)\n{', 1)
        path.write_text(text, encoding='utf-8', newline='\n')
        done += 1
    if done != 1:
        raise SystemExit('sub_00242B10 not found exactly once')
    print('Cold Fear: game log hook added to sub_00242B10.')


if __name__ == '__main__':
    main()
