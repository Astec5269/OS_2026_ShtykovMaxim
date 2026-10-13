#!/usr/bin/env python3
"""Запускайте после компиляции; результат — таблицы Markdown в stdout."""
import argparse
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
from datetime import datetime

parser = argparse.ArgumentParser()
parser.add_argument('--bin-dir', type=Path, default=Path(__file__).resolve().parent)
parser.add_argument('--sizes', type=int, nargs='+', default=[100000, 1000000])
parser.add_argument('--kinds', nargs='+', default=['random', 'sorted', 'reverse', 'equal'],
                    choices=['random', 'sorted', 'reverse', 'equal'])
parser.add_argument('--threads', type=int, nargs='+')
parser.add_argument('--repeats', type=int, default=3)
a = parser.parse_args()
cores = os.sysconf('SC_NPROCESSORS_ONLN')
if a.repeats < 1 or any(n < 1 for n in a.sizes):
    parser.error('sizes and repeats must be positive')
if a.threads:
    counts = sorted(set(a.threads))
else:
    counts = sorted(set([1, 2, 3, 4, cores, cores * 4, cores * 8, cores * 16]))
if any(p < 1 for p in counts):
    parser.error('thread limits must be positive')
if min(a.sizes) < max(counts):
    parser.error('every size must be at least the largest thread limit')


def run(executable, n, kind, p=None, seed=42):
    cmd = [str((a.bin_dir / executable).resolve()), '-n', str(n), '-s', str(seed), '-k', kind, '-q']
    if p is not None:
        cmd += ['-t', str(p)]
    result = subprocess.run(cmd, text=True, capture_output=True, timeout=180)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    match = re.search(r'time_ms=([0-9.]+)', result.stderr)
    if not match or 'verified=yes' not in result.stderr:
        raise RuntimeError('No valid timing / verification')
    return float(match.group(1))

print('# Эксперименты: сортировка слиянием\n')
print(f'Дата: {datetime.now().astimezone().isoformat(timespec="seconds")}.  ')
print(f'Среда измерений: {platform.platform()}.  ')
print(f'Логических процессоров по sysconf: {cores}. Повторов: {a.repeats}.  ')
print('В таблицах — медианы; seed = 42 + номер повтора (с нуля).\n')
print('Это результаты компьютера, на котором запущен скрипт. '
      'Число процессоров не гарантирует доступность соответствующей доли CPU в контейнере.\n')
if cores < 5:
    print('**Ограничение:** на этой системе невозможно выбрать четыре различных '
          'положительных числа потоков строго меньше числа логических процессоров. '
          'Это условие следует согласовать с преподавателем.\n')
for n in a.sizes:
    for kind in a.kinds:
        print(f'## N = {n}, данные: {kind}\n', flush=True)
        # Прогрев для каждого режима перед измерениями.
        run('sequential', n, kind)
        serial = [run('sequential', n, kind, seed=42+i) for i in range(a.repeats)]
        baseline = statistics.median(serial)
        print(f'Последовательная версия: **{baseline:.6f} мс**.\n')
        print('| Потоки p | Время Tp, мс | Ускорение Tseq/Tp | Эффективность, % |')
        print('|---:|---:|---:|---:|')
        for p in counts:
            try:
                run('parallel', n, kind, p)
                samples = [run('parallel', n, kind, p, 42+i) for i in range(a.repeats)]
                elapsed = statistics.median(samples)
                speedup = baseline / elapsed
                print(f'| {p} | {elapsed:.6f} | {speedup:.3f} | {100*speedup/p:.2f} |', flush=True)
            except (RuntimeError, subprocess.TimeoutExpired) as error:
                detail = str(error).replace('|', '/').replace('\n', ' ')
                print(f'| {p} | Ошибка: {detail} | — | — |', flush=True)
        print()
print('## Метрики и интерпретация\n')
print('Tseq — время последовательной версии; Tp — время параллельной при лимите p. '
      'Ускорение S = Tseq / Tp, эффективность E = S / p (в процентах: 100 × E). '
      'p включает основной поток; это лимит, а не число одновременно занятых ядер.\n')
print('S > 1 означает ускорение, S < 1 — замедление. На малых массивах затраты '
      'на создание потоков и ожидание могут превышать выигрыш. При больших p '
      'возрастают накладные расходы и конкуренция за CPU и память. '
      'Последнее слияние выполняется одним потоком: оно ограничивает ускорение. '
      'Одинаковые и заранее упорядоченные данные также требуют всех слияний, '
      'но влияют на ветвления и время обработки. '
      'Эти причины объясняют возможные тенденции; сравнивайте их с конкретными числами таблиц.')
