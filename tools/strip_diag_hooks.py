"""Remove the diagnostic hooks from the generated code (one-off cleanup).

The ship-spike, particle, camera and FMOD/DirectSound investigations patched
logging calls into gen/ through fix_vfmt_dbg.py, fix_dsound_dbg.py,
fix_fmod_dbg.py, fix_fmodinit_dbg.py, fix_vsconst_dbg.py and
fix_ptank_vb_dbg.py. Those scripts are no longer run by regen.ps1; this
removes what they left in the current gen/: each tagged call line and the
prototype each one added. Idempotent.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FUNCS = ('coldfear_vfmt_dbg', 'coldfear_dsound_enter', 'coldfear_fmod_ret',
         'coldfear_fmodinit_dbg', 'coldfear_vsconst_dbg', 'coldfear_ptank_lock_dbg',
         'coldfear_ptank_draw_dbg', 'coldfear_draw_dbg', 'coldfear_camlog')
CALL = re.compile(r'^\s+(%s)\(.*\); /\* fix_\w+\.py \*/\n' % '|'.join(FUNCS), re.M)
DECL = re.compile(r'^void (%s)\([^)]*\);\n' % '|'.join(FUNCS), re.M)
# The sampling profiler's per-function marks (fix_prof_entry.py, now out of
# regen.ps1): a volatile store at every function entry.
PROF = re.compile(r'^(    g_prof_fn = 0x[0-9A-F]{8}u;|extern volatile uint32_t g_prof_fn;)'
                  r' /\* fix_prof_entry\.py \*/\n', re.M)


def main():
    calls = decls = 0
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        new, n1 = CALL.subn('', text)
        new, n2 = DECL.subn('', new)
        new, n3 = PROF.subn('', new)
        n1 += n3
        if n1 or n2:
            path.write_text(new, encoding='utf-8', newline='\n')
            calls += n1
            decls += n2
    left = sum(p.read_text(encoding='utf-8-sig').count(f + '(')
               for p in (ROOT / 'gen').glob('recomp_*.c') for f in FUNCS)
    if left:
        raise SystemExit(f'{left} diagnostic references still in gen/')
    print(f'Cold Fear: removed {calls} diagnostic calls and {decls} prototypes from gen/.')


if __name__ == '__main__':
    main()
