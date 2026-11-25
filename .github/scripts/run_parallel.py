#!/usr/bin/env python3
"""Run independent test commands in parallel, each in its own temporary directory.

Tasks come from standard input, one per line, as ``name :: shell command``
(blank lines and lines starting with ``#`` are skipped). Each task runs under
bash from the current directory with TMPDIR pointing at a directory of its own,
so ``tempfile`` users and compilers never share scratch files. A task's output
(stdout and stderr together) is printed as one block when it finishes, inside a
collapsible log group named after it, so parallel output never interleaves.
Every task runs; the script then exits 1 with the full list of failures.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time


def parse(lines):
    tasks = []
    for number, line in enumerate(lines, 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, sep, command = line.partition(' :: ')
        if not sep or not name.strip() or not command.strip():
            raise SystemExit(f'task line {number} is not "name :: command": {line}')
        tasks.append((name.strip(), command.strip()))
    names = [name for name, _ in tasks]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise SystemExit('duplicate task names: ' + ', '.join(duplicates))
    return tasks


def default_jobs():
    # Half the CPUs: the runners share their nodes with other jobs.
    return max(2, (os.cpu_count() or 2) // 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--title', required=True, help='what the tasks are, for the summary')
    parser.add_argument('--jobs', type=int, default=default_jobs())
    parser.add_argument('--first', default='',
                        help='comma-separated task names to start first (the known slow ones)')
    args = parser.parse_args()
    tasks = parse(sys.stdin.read().splitlines())
    first = [name for name in args.first.split(',') if name]
    for name in sorted(set(first) - {name for name, _ in tasks}):
        print(f'::warning::--first names no task: {name}', flush=True)
    tasks.sort(key=lambda task: first.index(task[0]) if task[0] in first else len(first))

    root = tempfile.mkdtemp(prefix='run-parallel-')
    lock = threading.Lock()
    failed, durations = [], []

    def run(name, command):
        scratch = tempfile.mkdtemp(prefix=re.sub(r'[^A-Za-z0-9_.-]', '_', name) + '-', dir=root)
        env = dict(os.environ, TMPDIR=scratch, TMP=scratch, TEMP=scratch)
        start = time.monotonic()
        result = subprocess.run(['bash', '-c', command], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=env)
        seconds = time.monotonic() - start
        shutil.rmtree(scratch, ignore_errors=True)
        verdict = 'ok' if result.returncode == 0 else f'FAILED (exit {result.returncode})'
        output = result.stdout.decode('utf-8', errors='replace')
        with lock:
            print(f'::group::--- {name}: {verdict} in {seconds:.1f} s', flush=True)
            sys.stdout.write(output if output.endswith('\n') or not output else output + '\n')
            print('::endgroup::', flush=True)
            if result.returncode:
                print(f'::error::{name} failed (exit {result.returncode}); its output is in the group above', flush=True)
                failed.append(name)
            durations.append((seconds, name))

    start = time.monotonic()
    print(f'{len(tasks)} {args.title} on {args.jobs} workers', flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for future in [pool.submit(run, name, command) for name, command in tasks]:
            future.result()
    shutil.rmtree(root, ignore_errors=True)
    wall = time.monotonic() - start
    durations.sort(reverse=True)
    print(f'{len(tasks)} {args.title}: {wall:.0f} s wall, {sum(s for s, _ in durations):.0f} s of task time')
    print('slowest: ' + ', '.join(f'{name} {seconds:.0f} s' for seconds, name in durations[:8]))
    if failed:
        print(f'::error::failed {args.title}: ' + ' '.join(sorted(failed)))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
