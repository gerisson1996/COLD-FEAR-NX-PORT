"""Compare extracted game files with the supplied XDVDFS image, streaming data."""
import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'xboxrecomp'))
from tools.xiso.xdvdfs import Xiso, SECTOR


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('iso', type=Path)
    args = ap.parse_args()
    game = (ROOT / 'game').resolve()
    records = []
    with Xiso(args.iso) as iso, args.iso.open('rb') as source:
        for directory, entry in iso.walk():
            target = (game / directory / entry.name).resolve()
            if not target.is_relative_to(game):
                raise ValueError('Invalid disc path')
            rel = target.relative_to(game).as_posix()
            if not target.is_file() or target.stat().st_size != entry.size:
                raise ValueError(f'Missing file or incorrect size: {rel}')
            expected, actual = hashlib.sha256(), hashlib.sha256()
            source.seek(iso.base + entry.sector * SECTOR)
            remaining = entry.size
            with target.open('rb') as local:
                while remaining:
                    n = min(1024 * 1024, remaining)
                    block = source.read(n)
                    if len(block) != n:
                        raise ValueError(f'Truncated image: {rel}')
                    expected.update(block)
                    actual.update(local.read(n))
                    remaining -= n
            if actual.digest() != expected.digest():
                raise ValueError(f'Content mismatch: {rel}')
            records.append(dict(path=rel, size=entry.size, sha256=actual.hexdigest()))
    output = ROOT / 'analysis' / 'disc-verification.json'
    output.parent.mkdir(exist_ok=True)
    output.write_text(json.dumps(dict(iso=str(args.iso), files=records), indent=2), encoding='utf-8')
    print(f'OK: {len(records)} files match the ISO; report: {output}')


if __name__ == '__main__':
    main()
