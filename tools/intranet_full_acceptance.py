#!/usr/bin/env python3
"""One entry point for offline and three-host GCRP acceptance (Python 3.6)."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REPORT_DIR = ROOT / 'intranet-report' / 'full-acceptance'
STAGES = {
    'P10': ('test_batch_cancel',),
    'P11': ('test_sqlite_persistence', 'test_sqlite_worker'),
    'P12': ('test_worker_recovery', 'test_recovery_process'),
    'P13': ('test_worker_distribution', 'test_multislot_process', 'test_slot_identity', 'test_manager_takeover'),
    'P14': ('test_regression_service',),
    'P15': ('test_regression_service', 'test_sqlite_persistence',
            'test_worker_recovery', 'test_batch_cancel'),
    'P16': ('test_progress_service', 'test_protocol', 'test_client_cli'),
    'P17': ('test_api_queries', 'test_control_plane', 'test_gui_catalog', 'test_task_groups', 'test_daily_plan_process'),
    'LOGS': ('test_run_log_archive', 'test_log_retention',
             'test_pjtest_adapter', 'test_sqlite_worker', 'test_hardening', 'test_log_io_process', 'test_service_control'),
    'WORKER_MANAGEMENT': ('test_worker_controls', 'test_slot_maintenance', 'test_worker_control_process', 'test_manager_takeover'),
}
LATE_STAGES = ('P15', 'P16', 'P17', 'LOGS', 'WORKER_MANAGEMENT')
TEST_LINE = re.compile(r'^\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)\s+\.+\s*(Passed|\*\*\*\S+)', re.M)


def now():
    return datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def executable(name):
    intranet = Path('/home/xshare/scripts/bin/cmake-3.15.2/bin') / name
    if intranet.is_file():
        return str(intranet)
    found = shutil.which(name)
    if not found:
        raise RuntimeError('{} not found'.format(name))
    return found


def save(report):
    REPORT_DIR.mkdir(parents=True, exist_ok=True)
    path = REPORT_DIR / 'report.json'
    temporary = path.with_name('.report-{}.json'.format(os.getpid()))
    try:
        with temporary.open('w', encoding='utf-8') as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write('\n')
        os.replace(str(temporary), str(path))
    finally:
        if temporary.exists():
            temporary.unlink()
    return path


def logged(argv, cwd, name, timeout):
    path = REPORT_DIR / name
    with path.open('w', encoding='utf-8') as stream:
        code = subprocess.run(argv, cwd=str(cwd), stdout=stream,
                              stderr=subprocess.STDOUT, timeout=timeout,
                              check=False).returncode
    return code, path


def tail(path, lines=12):
    with path.open(encoding='utf-8', errors='replace') as stream:
        return [line.rstrip()[:400] for line in stream.readlines()[-lines:]]


def inventory(ctest, build):
    text = subprocess.check_output([ctest, '-N'], cwd=str(build),
                                   stderr=subprocess.STDOUT, timeout=30).decode('utf-8', 'replace')
    names = set(re.findall(r'Test\s+#\d+:\s+(\S+)', text))
    required = set(name for group in STAGES.values() for name in group)
    missing = required - names
    if missing:
        raise RuntimeError('missing CTest cases: ' + ', '.join(sorted(missing)))
    return names


def run_tests(ctest, build, names, report):
    if names is None:
        argv = [ctest, '--output-on-failure', '-j', '1']
        label = 'all'
    else:
        argv = [ctest, '-R', '^(' + '|'.join(sorted(names)) + ')$',
                '--output-on-failure', '-j', '1']
        label = 'latest'
    print('[FULL] running {} isolated tests'.format(label), flush=True)
    code, path = logged(argv, build, 'ctest-{}.log'.format(label), 1800)
    output = path.read_text(encoding='utf-8', errors='replace')
    results = {name: 'PASS' if status == 'Passed' else 'FAIL'
               for name, status in TEST_LINE.findall(output)}
    expected = inventory(ctest, build) if names is None else set(names)
    for name in expected:
        results.setdefault(name, 'NOT_RUN')
    report['offline'] = {'status': 'PASS' if code == 0 and
        all(results[name] == 'PASS' for name in expected) else 'FAIL',
        'passed': sum(results[name] == 'PASS' for name in expected),
        'total': len(expected), 'tests': results, 'log': str(path)}
    if report['offline']['status'] != 'PASS':
        report['offline']['tail'] = tail(path)
    for stage, tests in STAGES.items():
        if names is None or stage in LATE_STAGES:
            report['stages'][stage] = ('PASS' if all(results.get(name) == 'PASS'
                                          for name in tests) else 'FAIL')
    save(report)
    print('[FULL] offline {}/{} {}'.format(report['offline']['passed'],
          report['offline']['total'], report['offline']['status']), flush=True)
    return report['offline']['status'] == 'PASS'


def controller(args, report):
    command = [sys.executable, '-B', '-u', str(ROOT / 'tools' / 'intranet_p10_p14.py'),
               '--role', 'server', '--scale-host', args.scale_host]
    run_id = None
    log = REPORT_DIR / 'p10-p14-console.log'
    print('[FULL] P10-P14 live acceptance; use the printed token on both Workers', flush=True)
    with log.open('w', encoding='utf-8') as stream:
        process = subprocess.Popen(command, cwd=str(ROOT), stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, universal_newlines=True,
                                   bufsize=1)
        for line in process.stdout:
            print(line, end='', flush=True)
            stream.write(re.sub(r'(--token )\S+', r'\1<redacted>', line))
            match = re.search(r'\[ACCEPTANCE\] run=([0-9a-f]{12})', line)
            if match:
                run_id = match.group(1)
        code = process.wait()
    report['live'] = {'status': 'FAIL' if code else 'PASS', 'console': str(log)}
    if run_id:
        from intranet_smoke_common import config_and_root
        config = config_and_root()
        base = Path(config.get('smoke.report_dir') or './intranet-report').expanduser().resolve()
        path = base / ('p10-p14-' + run_id) / 'server-report.json'
        report['live']['report'] = str(path)
        if path.is_file():
            data = json.loads(path.read_text(encoding='utf-8'))
            report['live']['status'] = data.get('verdict', 'FAIL') if code == 0 else 'FAIL'
            report['live']['tasks'] = len(data.get('submitted_task_ids', []))
            for stage in ('P10', 'P11', 'P12', 'P13', 'P14'):
                report['stages'][stage] = (data.get('stages', {}).get(stage) or {}).get('status', 'FAIL')
        else:
            report['live']['status'] = 'FAIL'
    else:
        report['live']['status'] = 'FAIL'
    save(report)
    return report['live']['status'] == 'PASS' and all(
        report['stages'].get(stage) == 'PASS' for stage in ('P10', 'P11', 'P12', 'P13', 'P14'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--role', choices=('server', 'worker'), default='server')
    parser.add_argument('--offline-only', action='store_true',
                        help='run the complete local CTest suite without real Workers')
    parser.add_argument('--scale-host', choices=('novarch3', 'novarch4'), default='novarch4')
    parser.add_argument('--token', help='one-time token printed by Server for Worker role')
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--list', action='store_true', help='show covered stages without running')
    args = parser.parse_args()
    if args.list:
        for stage, tests in STAGES.items():
            print('{}: {}'.format(stage, ', '.join(tests)))
        return 0
    if args.role == 'worker':
        if not args.token or args.offline_only:
            parser.error('Worker needs --token and cannot use --offline-only')
        return subprocess.call([sys.executable, '-B', '-u',
            str(ROOT / 'tools' / 'intranet_p10_p14.py'), '--role', 'worker',
            '--token', args.token], cwd=str(ROOT))
    if args.token:
        parser.error('Server creates its own token')
    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be 1..32')
    report = {'started_at': now(), 'verdict': 'INCOMPLETE', 'stages': {},
              'mode': 'offline' if args.offline_only else 'three-host'}
    save(report)
    try:
        cmake, ctest = executable('cmake'), executable('ctest')
        build = ROOT / 'build'
        if not args.no_build:
            print('[FULL] configuring and building', flush=True)
            code, path = logged([cmake, '-S', str(ROOT), '-B', str(build),
                                 '-DBUILD_TESTING=ON'], ROOT, 'configure.log', 180)
            if code:
                raise RuntimeError('configure failed: ' + str(path))
            code, path = logged([cmake, '--build', str(build), '--parallel',
                                 str(args.jobs)], ROOT, 'build.log', 1200)
            if code:
                raise RuntimeError('build failed: ' + str(path))
        if not (build / 'CTestTestfile.cmake').is_file():
            raise RuntimeError('missing configured build; omit --no-build')
        inventory(ctest, build)
        names = None if args.offline_only else set(
            name for stage in LATE_STAGES for name in STAGES[stage])
        if not run_tests(ctest, build, names, report):
            raise RuntimeError('offline tests failed; inspect ctest log')
        if not args.offline_only and not controller(args, report):
            raise RuntimeError('P10-P14 live acceptance failed; inspect controller report')
        report['verdict'] = 'OFFLINE_PASS' if args.offline_only else 'PASS'
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired,
            subprocess.CalledProcessError, KeyboardInterrupt) as error:
        report['verdict'] = 'FAIL'
        report['error'] = str(error) or type(error).__name__
    report['finished_at'] = now()
    path = save(report)
    print('[FULL] {} report={}'.format(report['verdict'], path), flush=True)
    return 1 if report['verdict'] == 'FAIL' else 0


if __name__ == '__main__':
    sys.exit(main())
