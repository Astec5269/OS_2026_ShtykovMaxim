#!/usr/bin/env python3
"""Сборка и интеграционные проверки. Запуск: python3 test.py."""
from pathlib import Path
import subprocess
import tempfile
import shutil
from concurrent.futures import ThreadPoolExecutor

ROOT = Path(__file__).resolve().parent
for name in ('server', 'client'):
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Wpedantic',
                    '-Werror', str(ROOT / (name + '.c')), '-o', str(ROOT / name),
                    '-pthread', '-lrt'], check=True)

def run(data, expected, ok=True, filename='result.txt', executable=None):
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        result = subprocess.run([str(executable or ROOT / 'server')],
                                input=filename.encode() + b'\n' + data,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                cwd=folder, timeout=10)
        assert (result.returncode == 0) == ok, (result.returncode, result.stderr)
        if expected is not None:
            assert (folder / filename).read_bytes() == expected, result.stderr
        return result

before = set(Path('/dev/shm').glob('*sum_*'))
cases = [
    ('several lines', b'1 2 3\n10 -5 7\n100\n\n99\n', b'6\n12\n100\n', True),
    ('int limits', b'-2147483648 2147483647 +1\n2147483647 2147483647\n', b'0\n4294967294\n', True),
    ('negative and zero', b'-2 -3 -4\n0 0 0\n', b'-9\n0\n', True),
    ('spaces and tabs', b'  +2\t-5   7 \r\n', b'4\n', True),
    ('EOF without newline', b'7 -2 10', b'15\n', True),
    ('empty command stops', b'\n1 2\n', b'', True),
    ('EOF immediately', b'', b'', True),
    ('letters', b'1 2\n12 abc\n', b'3\n', False),
    ('fraction', b'1.5 2\n', b'', False),
    ('sign without digits', b'+\n', b'', False),
    ('int overflow', b'2147483648\n', b'', False),
    ('int underflow', b'-2147483649\n', b'', False),
    ('whitespace-only line', b'   \n', b'', False),
    ('NUL in number', b'12\x003\n', b'', False),
    ('long line', b'1 ' * 100000 + b'\n', b'100000\n', True),
    ('number across blocks', b' ' * 4094 + b'-2147483648 1\n', b'-2147483647\n', True),
    ('many lines', b'1 2 3\n' * 5000, b'6\n' * 5000, True),
    ('long invalid number', b'9' * 10000 + b'\n', b'', False),
]
for label, data, expected, ok in cases:
    run(data, expected, ok)
    print('PASS:', label)
run(b'1 2\n', None, False, 'missing-dir/out.txt')
print('PASS: file open error')
run(b'1 2\n', None, False, '/dev/full')
print('PASS: file write error')
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'server'
    shutil.copy2(ROOT / 'server', path)
    run(b'1 2\n', None, False, executable=path)
print('PASS: missing client')
with ThreadPoolExecutor(max_workers=4) as pool:
    list(pool.map(lambda _: run(b'2 3\n' * 2000, b'5\n' * 2000), range(8)))
print('PASS: independent concurrent instances')
assert set(Path('/dev/shm').glob('*sum_*')) == before
print('PASS: no named IPC leftovers')
print('23 checks passed')
