#!/usr/bin/env python3
"""Boot native RTMux on QEMU raspi3b, optionally verify the real serial shell."""
import argparse
import json
import hashlib
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import time


def command(out, qemu):
    kernel = out / 'arch/aarch64/rtmux-kernel.bin'
    archive = out / 'initramfs.cpio'
    for path in (kernel, archive):
        if not path.is_file():
            raise RuntimeError(f'Missing {path}; build with make pi3 first')
    return [qemu, '-M', 'raspi3b', '-display', 'none', '-monitor', 'none',
            '-serial', 'stdio', '-kernel', str(kernel),
            '-device', f'loader,file={archive},addr=0x08000000']


def validate_output(name, output, actual, expected=(), status=0, forbidden=(),
                    require_summary=False, result=None, required_trace_classes=()):
    """Validate a completed command; never treat a banner/skip as a test pass."""
    if result is None:
        result = {}
    result['exit_status'] = actual
    result['passed_assertions'] = [s.decode(errors='replace') for s in
        re.findall(rb'(?m)  PASS  (.*?)\r?$', output)]
    result['skips'] = [s.decode(errors='replace') for s in
        re.findall(rb'(?m)^.*\bSKIP\b(.*?)\r?$', output)]
    result['summaries'] = [list(map(int, values)) for values in re.findall(
        rb'=== Results: (\d+) tests, (\d+) passed, (\d+) failed ===', output)]
    if required_trace_classes:
        headers = [int(h, 0) for h in re.findall(rb'tracelogger: buf \d+ ev \d+ header=(0x[0-9a-fA-F]+|\d+)', output)]
        result['trace_class_counts'] = {str(c): sum((h >> 10) & 31 == c for h in headers)
                                        for c in required_trace_classes}
        if not all(result['trace_class_counts'].values()):
            raise RuntimeError(f'{name}: missing requested trace class')
    if actual != status:
        raise RuntimeError(f'{name}: exit {actual}, expected {status}')
    if any(s[2] or s[0] != s[1] or not s[0] for s in result['summaries']):
        raise RuntimeError(f'{name}: failing or empty test summary')
    for pattern in forbidden:
        if re.search(pattern.encode(), output):
            raise RuntimeError(f'{name}: forbidden output {pattern!r}')
    for text in expected:
        if text.encode() not in output:
            raise RuntimeError(f'{name}: missing output {text!r}')
    if require_summary and not result['summaries']:
        raise RuntimeError(f'{name}: missing test summary')
    if result['summaries'] and sum(s[1] for s in result['summaries']) != len(result['passed_assertions']):
        raise RuntimeError(f'{name}: assertion count disagrees with summary')
    return result


def check(cmd, out, timeout, commands=None, failure_hook=None):
    out.mkdir(parents=True, exist_ok=True)
    log = bytearray()
    results = []
    report = {'command': cmd, 'checks': results, 'status': 'FAIL'}
    kernel = Path(cmd[cmd.index('-kernel') + 1])
    if '-initrd' in cmd:
        archive = Path(cmd[cmd.index('-initrd') + 1])
    else:
        loader = next(arg for arg in cmd if arg.startswith('loader,file='))
        archive = Path(loader.split('file=', 1)[1].rsplit(',addr=', 1)[0])
    report['kernel_sha256'] = hashlib.sha256(kernel.read_bytes()).hexdigest()
    report['initramfs_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
    started = time.monotonic()
    serial = (out / 'serial.log').open('wb', buffering=0)
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)

    def receive(pattern, start=0, limit=timeout, input_after=()):
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            for trigger in input_after:
                if not trigger.get('sent') and re.search(trigger['pattern'].encode(), log[start:]):
                    proc.stdin.write(trigger['data'].encode())
                    proc.stdin.flush()
                    trigger['sent'] = True
            match = re.search(pattern, log[start:])
            if match:
                return match
            if proc.poll() is not None:
                raise RuntimeError(f'QEMU exited ({proc.returncode})')
            for key, _ in sel.select(min(.2, deadline - time.monotonic())):
                data = os.read(key.fileobj.fileno(), 65536)
                if not data:
                    raise RuntimeError('Serial console closed')
                log.extend(data)
                serial.write(data)
                if len(log) > 8 * 1024 * 1024:
                    raise RuntimeError('Excessive serial output')
            if any(marker in log for marker in
                   (b'AArch64 KERNEL FAULT', b'FATAL:', b'=== CRASH DUMP ===')):
                # Keep the rest of the register/backtrace dump. The marker can
                # arrive in the middle of a serial write; killing QEMU at once
                # would discard the address and registers needed to debug it.
                drain_until = time.monotonic() + .3
                while time.monotonic() < drain_until:
                    for key, _ in sel.select(.03):
                        data = os.read(key.fileobj.fileno(), 65536)
                        if data:
                            log.extend(data)
                            serial.write(data)
                raise RuntimeError('Kernel failure')
        raise RuntimeError(f'Timeout waiting for {pattern!r}; see {out / "serial.log"}')

    def step(name, line, expected=(), status=0, limit=timeout, forbidden=(),
             require_summary=False, input_after=(), required_trace_classes=()):
        start = len(log)
        token = f'__PI3_STEP_{len(results)}__'
        entry = {'name': name, 'command': line, 'status': 'FAIL'}
        results.append(entry)
        proc.stdin.write(f'{line}; echo {token}:$?\n'.encode())
        proc.stdin.flush()
        triggers = [dict(t) for t in input_after]
        match = receive(rb'\n' + token.encode() + rb':(\d+)\r?\n', start, limit, triggers)
        actual = int(match.group(1))
        output = bytes(log[start:])
        if triggers:
            entry['input_after'] = triggers
            if not all(t.get('sent') for t in triggers):
                raise RuntimeError(f'{name}: missing serial input handshake')
        validate_output(name, output, actual, expected, status,
                        forbidden, require_summary, result=entry,
                        required_trace_classes=required_trace_classes)
        entry['status'] = 'PASS'
        print(f'PASS {name}', flush=True)

    try:
        receive(rb'\n# ')
        for marker in (b'RTMux User-Mode Roottask starting', b'4 harts online',
                       b'RTMux Pi 3: native services ready'):
            if marker not in log:
                raise RuntimeError(f'Missing boot milestone {marker!r}')
        results.append({'name': 'kernel-roottask-shell-four-cpus', 'status': 'PASS'})
        print('PASS kernel-roottask-shell-four-cpus', flush=True)
        if commands:
            for item in commands:
                if isinstance(item, dict):
                    step(**item)
                else:
                    step(item, item)
            report['status'] = 'PASS'
            return report
        step('root-filesystem', '/bin/ls /', ('boot', 'lib', 'sbin', 'proc'))
        step('board', '/bin/cat /sys/board', ('raspberrypi,3-model-b',))
        step('processes', '/bin/ps', ('logger', 'pipe', 'sh'))
        step('pipeline', 'echo native_pipe_ok | /bin/cat', ('\nnative_pipe_ok\r\n',))
        step('timer', 'sleep 1')
        step('native-runtime', '/bin/pi3-runtime-test', ('pi3-runtime: PASS',), limit=60)
        step('toolbox', '/bin/sh /bin/toolbox-test', ('toolbox: PASS',), limit=180)
        step('shell-status', 'false', status=1)
        step('shell-still-alive', 'echo native_shell_verified', ('\nnative_shell_verified\r\n',))
        report['status'] = 'PASS'
        print(f'Pi3 native RTMux: PASS ({len(results)} checks)', flush=True)
    except (RuntimeError, OSError) as exc:
        report['error'] = str(exc)
        if failure_hook is not None:
            failure_hook(proc, report)
        raise
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        sel.close()
        proc.stdin.close()
        proc.stdout.close()
        serial.close()
        (out / 'serial.log').write_bytes(log)
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path('build'))
    parser.add_argument('--qemu', default=os.environ.get('QEMU_AARCH64', 'qemu-system-aarch64'))
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--command', action='append', help='Check a shell command (repeatable) instead of the smoke sequence')
    args = parser.parse_args()
    try:
        cmd = command(args.out.resolve(), args.qemu)
        if args.check or args.command:
            check(cmd, args.out / 'pi3-check', args.timeout, args.command)
            return 0
        return subprocess.call(cmd)
    except (RuntimeError, OSError) as exc:
        print(f'Pi3: FAIL: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
