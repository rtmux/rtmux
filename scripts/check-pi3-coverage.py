#!/usr/bin/env python3
"""Run the shared native RISC-V regression contracts on QEMU Raspberry Pi 3B."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pi3_runner', ROOT / 'scripts/run-pi3.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

SUITES = ('timers', 'sync', 'printf', 'setjmp', 'signals', 'notifications',
          'capgate', 'trace-negative', 'user-ptr-negative', 'vfs',
          'posix-options', 'gauges')

def probe(name, line, *expected, timeout=180, allow_skips=False):
    forbidden = [r'(?m)^  FAIL  ', r'(?m)^[-\w]+: FAIL\b']
    if not allow_skips:
        forbidden.append(r'\bSKIP\b')
    return dict(name=name, line=line, expected=expected, limit=timeout,
                forbidden=forbidden, require_summary='syscall_testing' in line)

def conformance(path='/bin/syscall_testing'):
    # A non-interactive parent matches userland/init on RISC-V. The suite's
    # setsid test must start as a non-process-group-leader.
    step = probe('syscall-conformance', f'/bin/sh -c {path}',
                 *[f'\n[{suite}]\r\n' for suite in SUITES], '0 failed ===',
                 timeout=900, allow_skips=True)
    # Upstream explicitly quarantines Q32 in the full SMP suite. Everything
    # else, including allocation-failure injection, must execute in this image.
    step['forbidden'].append(r'(?m)^  SKIP  (?!spawn_concurrent \(SMP: open defect)')
    return step

def serial_probe():
    step = probe('serial-unblock',
        "/bin/syscall_testing --serial-unblock-driver /bin/dev-serial-pl011",
        'dev-serial-pl011: READY', 'serial_unblock replacement got the injected byte',
        '0 failed ===')
    step['input_after'] = [{'pattern': r'\nserial-unblock: WAITING\r?\n', 'data': 'x'}]
    return step

def trace_probe(name, line, classes):
    step = probe(name, line, 'tracelogger: PASS')
    step['required_trace_classes'] = classes
    return step

def native_probes():
    return [
        probe('pi3-registers-runtime', '/bin/pi3-runtime-test', 'pi3-runtime: PASS'),
        probe('fault-recovery', '/bin/syscall_testing --fault-recovery', '0 failed ==='),
        probe('loader-negative', '/bin/syscall_testing --loader-negative', '0 failed ==='),
        probe('disconnect-allocation-failure', '/bin/syscall_testing --disconnect-alloc-failure', '0 failed ==='),
        probe('posix-cap-boundary', '/bin/syscall_testing --posix-cap-boundary', '0 failed ==='),
        probe('namespace-combined-race', '/bin/syscall_testing --namespace-combined-race', '0 failed ===', timeout=300),
        probe('shell-posix', '/bin/sh /bin/sh-posix-test', 'sh-posix: PASS'),
        probe('toolbox', '/bin/sh /bin/toolbox-test', 'toolbox: PASS'),
        probe('lock-rank', '/bin/lock-rank-stress 100', 'lock-rank-stress:'),
        probe('rcu', '/bin/rcu-lookup-measure 100', 'rcu-lookup-measure:'),
        probe('revoke', '/bin/revoke-stress 2 8 1', 'revoke-stress:'),
        probe('preemption', '/bin/preempt-latency', 'preempt-latency: PASS'),
        probe('ipc-contention', '/bin/ipc-contention', 'ipc-contention: PASS'),
        probe('rt-latency', '/bin/rt-latency 100 1000000 60', 'rt-latency:'),
        probe('timer-jitter', '/bin/timer-jitter 50 60', 'timer-jitter:'),
        probe('ipc-roundtrip', '/bin/ipc-roundtrip 500', 'ipc-roundtrip:'),
        probe('priority-inheritance', '/bin/pi-inversion-xfail', 'pi-inversion-xfail: PASS'),
        probe('ipc-priority-donation', '/bin/ipc-priority-donation', 'A_donated=40 B_donated=40'),
        probe('namespace', '/bin/namespace-roundtrip 25', 'namespace-roundtrip:'),
        probe('no-linux-domains', '/bin/lkl-gate', 'lkl-gate: absent-domain'),
        probe('tickless', '/bin/tickless-idle', 'tickless-idle: PASS'),
        probe('monotonic-time', '/bin/monotonic-time', 'monotonic-time: PASS'),
        probe('rr-fairness', '/bin/rr-fairness', 'rr-fairness: PASS'),
        probe('sporadic-budget', '/bin/sporadic-budget', 'sporadic-budget: PASS'),
        probe('mutex-wake', '/bin/mutex-wake', 'mutex-wake:'),
        probe('mutex-uncontended', '/bin/mutex-uncontended', 'mutex-uncontended:'),
        probe('irq-notification', '/bin/irq-notify', 'outcomes ok=100 early=0 timeout=0', 'irq-notify: PASS'),
        probe('irq-suppression-negative', '/bin/irq-notify --suppress-one',
              'outcomes ok=99 early=0 timeout=1', 'irq-notify: PASS'),
        trace_probe('trace-ring', '/bin/tracelogger', [4, 7]),
        trace_probe('syscall-trace', '/bin/tracelogger 32 2 kcall', [2]),
        probe('irq-trace-latency', '/bin/irq-latency', 'irq-latency: PASS'),
        probe('memory-admission', '/bin/mem-pressure', 'hog_enomem=1', 'canary_ok=1', timeout=300),
        serial_probe(),
        probe('shell-after-pressure', 'echo coverage_shell_alive', '\ncoverage_shell_alive\r\n'),
    ]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path('build'))
    parser.add_argument('--qemu', default=os.environ.get('QEMU_AARCH64', 'qemu-system-aarch64'))
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--suite-only', action='store_true',
                        help='Run conformance only, for the POSIX profile matrix')
    parser.add_argument('--probes-only', action='store_true',
                        help='Run native probes only, for focused regression checks')
    parser.add_argument('--syslog-wrap', action='store_true',
                        help='Run the shared 4 KiB log-ring wrap/overflow fixture')
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error('--repeat must be positive')
    if sum((args.suite_only, args.probes_only, args.syslog_wrap)) > 1:
        parser.error('--suite-only, --probes-only and --syslog-wrap are mutually exclusive')
    out = args.out.resolve()
    evidence = out / 'pi3-coverage'
    evidence.mkdir(parents=True, exist_ok=True)
    report = {'status': 'FAIL', 'scope': 'native RTMux on QEMU raspi3b',
              'shared_suites': [] if args.probes_only or args.syslog_wrap else SUITES,
              'mode': 'syslog-wrap' if args.syslog_wrap else 'probes-only' if args.probes_only else 'suite-only' if args.suite_only else 'full',
              'runs': [],
              'platform_equivalents': {'block/VFS': 'RAM block + native romfs',
                                      'IRQ alarm': 'BCM2835 timer compare 3'},
              'not_claimed': ['NVMe/PCI hardware driver', 'FU740 GPIO/IOMMU',
                              'Linux/LKL', 'physical Pi3', 'eight-CPU scaling'],
              'upstream_quarantines': ['spawn_concurrent Q32 (also skipped by RISC-V SMP suite)']}
    try:
        config = (out / 'build.config').read_bytes()
        required = (b'CONFIG_ARCH_AARCH64=y', b'CONFIG_PROCESSORS_MAX=4',
                    b'CONFIG_INSTRUMENTATION=y', b'CONFIG_DEBUG_INJECT=y')
        if any(line not in config.splitlines() for line in required) or b'CONFIG_LKL_SERVERS=y' in config:
            raise RuntimeError('Build with configs/pi3_coverage_defconfig; coverage requires IRQ tracing and fault injection, without Linux domains')
        if args.syslog_wrap and b'CONFIG_ROOTTASK_SYSLOG_SIZE=4096' not in config.splitlines():
            raise RuntimeError('--syslog-wrap requires CONFIG_ROOTTASK_SYSLOG_SIZE=4096')
        report['build_config_sha256'] = hashlib.sha256(config).hexdigest()
        command = runner.command(out, args.qemu)
        for i in range(args.repeat):
            commands = [] if args.probes_only else [conformance()]
            if args.syslog_wrap:
                commands = [probe('syslog-wrap', '/bin/syscall_testing --sys-log-wrap', '0 failed ===')]
            elif (i == 0 or args.probes_only) and not args.suite_only:
                commands += native_probes()
            if i == 1 and not args.probes_only and not args.syslog_wrap:
                disk = conformance('/usr/bin/syscall_testing')
                disk['name'] = 'syscall-conformance-from-block-filesystem'
                commands.append(disk)
            print(f'Pi3 coverage: cold boot {i + 1}/{args.repeat}', flush=True)
            run_dir = evidence / f'boot-{i + 1}'
            try:
                runner.check(command, run_dir, 30, commands)
            finally:
                if (run_dir / 'results.json').exists():
                    report['runs'].append(json.loads((run_dir / 'results.json').read_text()))
        report['status'] = 'PASS'
        print(f'Pi3 coverage: PASS ({args.repeat} cold boots)', flush=True)
        return 0
    except (OSError, RuntimeError) as exc:
        report['error'] = str(exc)
        print(f'Pi3 coverage: FAIL: {exc}', file=sys.stderr)
        return 1
    finally:
        (evidence / 'coverage.json').write_text(json.dumps(report, indent=2) + '\n')

if __name__ == '__main__':
    sys.exit(main())
