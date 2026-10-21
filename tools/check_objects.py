"""Check ELF64 section/string-table bounds after an interrupted cross-build."""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
bad = []
files = list((ROOT / 'build-switch-coldfear').rglob('*.o'))
for path in files:
    data = path.read_bytes()
    try:
        assert data[:6] == b'\x7fELF\x02\x01'
        offset = struct.unpack_from('<Q', data, 40)[0]
        size, count, strings_index = struct.unpack_from('<HHH', data, 58)
        assert size == 64 and 0 < count and strings_index < count
        assert offset + count * size <= len(data)
        strings_header = struct.unpack_from('<IIQQQQIIQQ', data, offset + strings_index * size)
        strings = data[strings_header[4]:strings_header[4] + strings_header[5]]
        assert len(strings) == strings_header[5]
        for i in range(count):
            section = struct.unpack_from('<IIQQQQIIQQ', data, offset + i * size)
            assert section[0] < len(strings) and b'\0' in strings[section[0]:]
            assert section[1] == 8 or section[4] + section[5] <= len(data)
    except (AssertionError, struct.error):
        bad.append(str(path.relative_to(ROOT)))
(ROOT / 'analysis/bad-objects.txt').write_text('\n'.join(bad), encoding='utf-8')
print(f'Checked {len(files)} objects; invalid: {len(bad)}')
print('\n'.join(bad))
sys.exit(bool(bad))
