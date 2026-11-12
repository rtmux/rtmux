#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build and boot every native Pi3 POSIX profile, restoring the caller's config."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path('build/pi3-profiles'))
    parser.add_argument('--profiles', nargs='+', choices=('minimal', 'realtime', 'lkl', 'full', 'syslog-wrap'),
                        default=['minimal', 'realtime', 'lkl', 'syslog-wrap'])
    parser.add_argument('--jobs', type=int, default=2)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--build-only', action='store_true')
    mode.add_argument('--run-only', action='store_true')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    config = ROOT / '.config'
    saved = config.read_bytes() if config.exists() else None
    report = {'status': 'FAIL', 'profiles': {}, 'linux_domains': False}
    make = os.environ.get('MAKE', 'make')

    def run(argv, logfile):
        with logfile.open('w') as log:
            subprocess.run(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)

    def interrupted(signum, frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    try:
        for profile in args.profiles:
            option_profile = 'full' if profile == 'syslog-wrap' else profile
            build = out / profile
            if not args.run_only:
                if build.exists():
                    raise RuntimeError(f'{build} already exists; choose a fresh --out to avoid stale headers')
                build.mkdir()
                fragment = build / 'defconfig'
                fragment.write_text((ROOT / 'configs/pi3_coverage_defconfig').read_text().replace(
                    'CONFIG_POSIX_PROFILE_FULL=y', f'CONFIG_POSIX_PROFILE_{option_profile.upper()}=y') +
                    ('\nCONFIG_ROOTTASK_SYSLOG_SIZE=4096\n' if profile == 'syslog-wrap' else ''))
                print(f'Pi3 profile {profile}: build', flush=True)
                run([make, 'defconfig', f'DEFCONFIG={fragment}'], build / 'config.log')
                run([make, f'-j{args.jobs}', 'pi3', f'OUT={build}'], build / 'build.log')
            built_config = (build / 'build.config').read_text()
            if f'CONFIG_POSIX_PROFILE_{option_profile.upper()}=y\n' not in built_config or 'CONFIG_LKL_SERVERS=y' in built_config:
                raise RuntimeError(f'{build}: unexpected profile or Linux domains enabled')
            if args.build_only:
                report['profiles'][profile] = {'status': 'BUILT', 'tests_run': False}
                continue
            print(f'Pi3 profile {profile}: shared conformance suite', flush=True)
            run([sys.executable, 'scripts/check-pi3-coverage.py', '--out', str(build),
                 '--repeat', '1', '--syslog-wrap' if profile == 'syslog-wrap' else '--suite-only'], build / 'run.log')
            report['profiles'][profile] = json.loads((build / 'pi3-coverage/coverage.json').read_text())
            print(f'Pi3 profile {profile}: PASS', flush=True)
        report['status'] = 'BUILT' if args.build_only else 'PASS'
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        report['error'] = str(exc)
        print(f'Pi3 profiles: FAIL: {exc}', file=sys.stderr)
        return 1
    finally:
        try:
            if saved is not None:
                with tempfile.NamedTemporaryFile(prefix='rtmux-pi3-config-') as original:
                    original.write(saved)
                    original.flush()
                    run([make, 'defconfig', f'DEFCONFIG={original.name}'], out / 'restore.log')
            else:
                config.unlink(missing_ok=True)
        except (OSError, subprocess.CalledProcessError) as exc:
            report['status'] = 'FAIL'
            report['restore_error'] = str(exc)
            raise
        finally:
            filename = 'builds.json' if args.build_only else 'profiles.json'
            (out / filename).write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    sys.exit(main())
