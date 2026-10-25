#!/usr/bin/env python3
"""Compile the actual SLUB and PMM modules; each case runs in its own process."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='gcc')
parser.add_argument('--pmm', choices=('buddy', 'best_fit', 'both'), default='both')
parser.add_argument('--group', choices=('all', 'normal', 'oom', 'failure', 'invalid', 'corrupt'), default='all')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parent.parent
groups = {
    'normal': ('functional', 'ordinary-coexist'),
    'oom': ('oom-small', 'oom-large'),
    'failure': ('failure-snapshots',),
    'invalid': ('before-init-alloc', 'before-init-free', 'repeat-init', 'small-interior',
                'unused-slot', 'duplicate-live-slab', 'duplicate-reclaimed', 'ordinary',
                'reserved', 'below-map', 'above-map', 'large-offset', 'large-interior', 'large-duplicate'),
    'corrupt': ('link', 'freelist-outside', 'freelist-cycle', 'freelist-live', 'bitmap',
                'capacity', 'offset', 'inuse', 'wrong-list', 'owner', 'original-pages', 'lower-free'),
}
if args.group != 'all':
    groups = {args.group: groups[args.group]}
pmms = ('buddy', 'best_fit') if args.pmm == 'both' else (args.pmm,)
failures = []
total = 0
with tempfile.TemporaryDirectory(prefix='ucore-slub-tests-') as tmp:
    for pmm in pmms:
        binary = str(Path(tmp) / pmm)
        command = [args.cc, '-std=gnu99', '-O2', '-g', '-nostdinc', '-fno-builtin',
                   '-Wall', '-Werror', '-Wno-unused', '-DSLUB_TEST=1', '-DPMM_' + pmm.upper(),
                   *['-I' + str(root / p) for p in ('libs', 'kern/mm', 'kern/debug', 'kern/driver')],
                   str(here / 'host.c'), '-o', binary]
        print('COMMAND:', ' '.join(command), flush=True)
        subprocess.run(command, check=True)
        for group, cases in groups.items():
            for case in cases:
                total += 1
                print('RUN', pmm, group, case, flush=True)
                try:
                    result = subprocess.run([binary, group, case], timeout=60)
                    if result.returncode:
                        failures.append((pmm, group, case, result.returncode))
                except subprocess.TimeoutExpired:
                    failures.append((pmm, group, case, 'timeout'))
print(f'RESULT: {total - len(failures)}/{total} cases passed', flush=True)
for failure in failures:
    print('FAILED:', *failure, flush=True)
raise SystemExit(bool(failures))
