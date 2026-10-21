"""Record the guest function being entered, for the sampling profiler.

Every lifted function stores its address in g_prof_fn on entry (after its
RECOMP_PREEMPT() check where it has one). Only one guest thread runs at a
time (the guest lock), so the global names the guest code running now -- or
the last function it entered. src/prof.c samples it (COLDFEAR_PROF=1) and
reports where the guest CPU time goes. One store per call. Idempotent.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TAG = '/* fix_prof_entry.py */'
DECL = f'extern volatile uint32_t g_prof_fn; {TAG}\n'
# The RECOMP_PREEMPT() line is taken whenever it is there (no backtracking to
# mark before it), and a function already marked is left alone.
ENTRY = re.compile(r'(void sub_([0-9A-F]{8})\(void\)\n\{\n(?:    RECOMP_PREEMPT\(\);\n|(?!    RECOMP_PREEMPT)))(?!    g_prof_fn)')


def main():
    files = funcs = 0
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        new, n = ENTRY.subn(lambda m: m.group(1) + f'    g_prof_fn = 0x{m.group(2)}u; {TAG}\n', text)
        if n and DECL not in new:
            first = new.find('\nvoid sub_')
            new = new[:first + 1] + DECL + new[first + 1:]
        if n:
            path.write_text(new, encoding='utf-8', newline='\n')
            files += 1
        funcs += new.count(TAG) - (1 if DECL in new else 0)
    print(f'Cold Fear: profiler entry marks in {funcs} functions ({files} files changed).')


if __name__ == '__main__':
    main()
