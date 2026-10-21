"""Turn the console's prof.bin into per-function tables (RECOMP_NX_PROFILE).

src/switch_nx.c samples every busy thread at 1 kHz and writes 32-byte
records: u16 thread slot, u16 pad, u32 pc, u32 lr, u32 ret[5] -- offsets
into the NRO, which are ELF addresses (the ELF's _start is 0). Symbols come
from `nm -n` of the matching ELF (aarch64-none-elf-nm -n --defined-only).

    python tools/prof_report.py prof.bin syms.txt [--slot N] [--top 40]

Per slot: where the samples are (self, by pc) and which functions are on
the stack (inclusive: pc, lr or any recorded return address), the latter
being what names a wait loop's owner.
"""
import argparse
import bisect
import struct
from collections import Counter, defaultdict


def load_syms(path):
    addrs, names = [], []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                a = int(parts[0], 16)
            except ValueError:
                continue
            addrs.append(a)
            names.append(parts[2])
    return addrs, names


def resolve(addrs, names, off):
    if off in (0, 0xFFFFFFFF):
        return None
    i = bisect.bisect_right(addrs, off) - 1
    return names[i] if i >= 0 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prof')
    ap.add_argument('syms')
    ap.add_argument('--slot', type=int, default=None)
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()

    addrs, names = load_syms(args.syms)
    data = open(args.prof, 'rb').read()
    n = len(data) // 32
    self_by = defaultdict(Counter)
    incl_by = defaultdict(Counter)
    total = Counter()
    for k in range(n):
        slot, _pad, pc, lr, r0, r1, r2, r3, r4 = struct.unpack_from('<HHIIIIIII', data, k * 32)
        if args.slot is not None and slot != args.slot:
            continue
        total[slot] += 1
        fn = resolve(addrs, names, pc) or f'?{pc:08X}'
        self_by[slot][fn] += 1
        seen = set()
        for a in (pc, lr, r0, r1, r2, r3, r4):
            s = resolve(addrs, names, a)
            if s and s not in seen:
                seen.add(s)
                incl_by[slot][s] += 1

    for slot in sorted(total):
        t = total[slot]
        print(f'=== slot {slot}: {t} samples ===')
        print('  self (pc):')
        for fn, c in self_by[slot].most_common(args.top):
            print(f'    {100.0 * c / t:5.1f}%  {fn}')
        print('  inclusive (on the stack):')
        for fn, c in incl_by[slot].most_common(args.top):
            print(f'    {100.0 * c / t:5.1f}%  {fn}')
        print()


if __name__ == '__main__':
    main()
