#!/usr/bin/env python3
"""Build in a temporary directory; run every case in an independent process."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parent.parent
groups = {
    'normal': ('basic', 'tail', 'merge', 'non-buddies', 'list-ends', 'init-ranges',
               'single', 'maximum', 'failures', 'random', 'kernel-check', 'kernel-check-small'),
    'invalid': ('null', 'zero', 'short', 'long', 'internal', 'duplicate', 'joined',
                'reserved', 'unregistered', 'before-array', 'past-array',
                'misaligned-pointer', 'huge-length', 'overlap-registration',
                'zero-registration', 'range-registration'),
    'corrupt': ('null-link', 'prev-link', 'count', 'size', 'alignment',
                'inuse-page', 'overlap', 'unmerged', 'leak'),
}
failures = []
with tempfile.TemporaryDirectory(prefix='ucore-buddy-tests-') as tmp:
    binary = str(Path(tmp) / 'host')
    command = [args.cc, '-std=gnu99', '-O2', '-g', '-nostdinc', '-fno-builtin',
               '-Wall', '-Werror', '-Wno-unused',
               *['-I' + str(root / p) for p in ('libs', 'kern/mm', 'kern/debug', 'kern/driver')],
               str(here / 'host.c'), '-o', binary]
    print('COMMAND:', ' '.join(command), flush=True)
    subprocess.run(command, check=True)
    for group, cases in groups.items():
        for case in cases:
            print(f'RUN {group}/{case}', flush=True)
            try:
                result = subprocess.run([binary, group, case], timeout=60)
                if result.returncode:
                    failures.append((group, case, result.returncode))
            except subprocess.TimeoutExpired:
                failures.append((group, case, 'timeout'))
print(f'RESULT: {sum(map(len, groups.values())) - len(failures)}/'
      f'{sum(map(len, groups.values()))} cases passed', flush=True)
for failure in failures:
    print('FAILED:', *failure, flush=True)
raise SystemExit(bool(failures))
