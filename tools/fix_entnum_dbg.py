"""Trace who sets and who reads an entity's number field [obj+0x94c].

sub_002478C0 / sub_00248870: script commands that write it.
sub_00103940: DeadBody init (vtable 0x0031F708 slot 1) that reads it and
looks the entity up. The DeadBody crash reads garbage there; this logs the
order of writes and reads per object. Diagnostics only. Idempotent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOOKS = {
    '002478C0': 'coldfear_entnum_dbg(1, esp, ecx);',
    '00248870': 'coldfear_entnum_dbg(2, esp, ecx);',
    '00103940': 'coldfear_entnum_dbg(3, esp, ecx);',
}
DECL = 'void coldfear_entnum_dbg(int tag, uint32_t esp_, uint32_t ecx_);\n'


def main():
    done = 0
    for path in sorted((ROOT / 'gen').glob('recomp_*.c')):
        text = path.read_text(encoding='utf-8-sig')
        changed = False
        for va, call in HOOKS.items():
            fn = f'void sub_{va}(void)\n{{'
            if fn not in text:
                continue
            head, body = text.split(fn, 1)
            fn_body, rest = body.split('\n}\n', 1)
            label = f'loc_{va}: ;\n'
            if 'fix_entnum_dbg.py' in fn_body:
                done += 1
                continue
            assert fn_body.count(label) == 1, va
            fn_body = fn_body.replace(label, label + f'    {call} /* fix_entnum_dbg.py */\n')
            text = head + DECL + fn + fn_body + '\n}\n' + rest
            changed = True
            done += 1
        if changed:
            path.write_text(text, encoding='utf-8', newline='\n')
    if done != 3:
        raise SystemExit(f'expected 3 hooks, placed {done}')
    print('Cold Fear: entity-number trace hooks in place (3).')


if __name__ == '__main__':
    main()
