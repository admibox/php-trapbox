"""Run native/prototype FPM under Memcheck with persistent-worker bailouts."""
import argparse
from collections import Counter
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

# Reuse the Unix FastCGI transport. This fixture returns exit(17), so the
# transport accepts an explicitly requested application status as well.
spec = importlib.util.spec_from_file_location('local_magento', Path(__file__).with_name('local-magento.py'))
transport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transport)


def check_bailout_loss_delta(losses):
    """No additional lost bytes or blocks are accepted beyond native FPM."""
    assert losses['native'] == losses['prototype'], losses



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--versions', nargs='+', default=['7.3', '7.4'])
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    root = source.parents[1]
    runtime = ['sudo', 'nerdctl']
    work = Path(tempfile.mkdtemp(prefix='trapbox-fpm-memory.'))
    work.chmod(0o755)
    print(f'Artifacts: {work}', flush=True)
    for version in args.versions:
        build = work / version
        subprocess.run(['python3', str(source / 'prepare.py'), str(root), str(build)], check=True)
        for file in ('fpm-memory-target.php', 'valgrind-string.c', 'audit-bailout.php'):
            shutil.copy(source / file, build)
        image = 'trapbox-proxy-memcheck:' + version + '-fpm'
        common = runtime + ['run', '-v', str(build) + ':/ext', '-w', '/ext']
        with (build / 'build.log').open('w') as output:
            subprocess.run(common + ['--rm', '--entrypoint', 'sh', image, '-c',
                'phpize && ./configure && make -j2 && cc -shared -fPIC -g -O1 $(php-config --includes) valgrind-string.c -o valgrind-string.so'],
                stdout=output, stderr=subprocess.STDOUT, check=True)
        (build / 'fpm.conf').write_text('''[global]
daemonize=no
process_control_timeout=60s
error_log=/ext/fpm.log
[www]
user=root
group=root
listen=/ext/fpm.sock
listen.mode=0666
pm=static
pm.max_children=1
clear_env=no
catch_workers_output=yes
''')
        for suite, sequence in (
            ('normal', ['normal', 'throw'] * 10),
            ('bailout', ['normal', 'exit', 'normal', 'fatal', 'throw'] * 10),
        ):
            bodies = {}
            losses = {}
            for mode in ('native', 'prototype'):
                label = suite + '-' + mode
                reports = build / label
                reports.mkdir()
                name = 'trapbox-memory-' + version + '-' + label + '-' + str(os.getpid())
                flags = ['-n', '-dzend_extension=opcache', '-dopcache.enable=1',
                         '-dopcache.file_update_protection=0', '-ddisplay_errors=0', '-dlog_errors=0']
                if mode == 'prototype':
                    (reports / 'events.jsonl').touch()
                    flags += ['-dextension=/ext/modules/trapbox.so', '-dtrapbox.log_path=/ext/' + label + '/events.jsonl']
                    flags += ['-dtrapbox.' + category + '=off' for category in
                              ('exec', 'eval', 'include', 'file_write', 'secret_read', 'unserialize')]
                    flags += ['-dtrapbox.errors=bypass']
                command = common + ['--name', name, '--entrypoint', 'valgrind',
                    '-e', 'USE_ZEND_ALLOC=0', '-e', 'ZEND_DONT_UNLOAD_MODULES=1',
                    '-e', 'LD_PRELOAD=/ext/valgrind-string.so', image,
                    '--tool=memcheck', '--error-exitcode=99', '--leak-check=full',
                    '--show-leak-kinds=definite,indirect,possible',
                    '--errors-for-leak-kinds=none',
                    '--track-origins=yes', '--log-file=/ext/' + label + '/memcheck.%p.log',
                    'php-fpm', '-R', '-y', '/ext/fpm.conf'] + flags
                responses = []
                pids = set()
                (build / 'fpm.log').touch()
                (build / 'fpm.log').chmod(0o666)
                with (reports / 'container.log').open('w') as output:
                    process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
                    try:
                        for attempt in range(300):
                            if process.poll() is not None:
                                raise RuntimeError(f'{label} FPM exited before readiness')
                            if (build / 'fpm.sock').exists():
                                break
                            time.sleep(0.1)
                        else:
                            raise RuntimeError(f'{label} FPM socket timed out')
                        for index, case in enumerate(sequence):
                            result = transport.request(build / 'fpm.sock', 'trapbox.localhost',
                                f'/request-{index}.php?mode={case}',
                                '/ext/fpm-memory-target.php', allowed_statuses=(0, 17, 255),
                                script_name=f'/request-{index}.php')
                            body = result['body']
                            pids.add(re.search(r'PID:(\d+)', body)[1])
                            assert 'CLEAN:yes' in body and 'OPCACHE:yes' in body, body
                            assert body.count('HANDLER\n') == 1 and body.count('SHUTDOWN\n') == 1, body
                            # Native FPM skips this destructor on uncaught fatal.
                            assert body.count('DESTRUCT\n') == (0 if case == 'fatal' else 1), body
                            assert ('AFTER\n' in body) == (case in ('normal', 'throw')), body
                            assert ('CAUGHT\n' in body) == (case == 'throw'), body
                            responses.append(result)
                        assert len(pids) == 1, pids
                    finally:
                        subprocess.run(runtime + ['kill', '--signal', 'SIGQUIT', name], stdout=subprocess.DEVNULL, check=False)
                        try:
                            code = process.wait(timeout=120)
                        except subprocess.TimeoutExpired:
                            subprocess.run(runtime + ['kill', name], check=False)
                            process.wait(timeout=10)
                            raise
                        subprocess.run(runtime + ['rm', name], stdout=subprocess.DEVNULL, check=False)
                        (reports / 'responses.json').write_text(json.dumps(responses, indent=2))
                        if (build / 'fpm.log').exists():
                            shutil.copy(build / 'fpm.log', reports / 'fpm.log')
                            (build / 'fpm.log').unlink()
                assert code == 0, (label, code)
                logs = list(reports.glob('memcheck.*.log'))
                assert len(logs) == 2, (label, logs)
                losses[mode] = {}
                for log in logs:
                    report = log.read_text()
                    assert 'ERROR SUMMARY: 0 errors' in report, str(log)
                    assert 'Process terminating with default action' not in report, str(log)
                    role = 'master' if log.name == 'memcheck.1.log' else 'worker'
                    losses[mode][role] = re.findall(r'(?:definitely|indirectly|possibly) lost: [^\n]+', report)
                    assert len(losses[mode][role]) == 3
                assert not re.search(r'exited on signal|SIGSEGV|zend_mm_heap|child .* exited with code 99',
                                     (reports / 'fpm.log').read_text())
                if mode == 'prototype':
                    events = [json.loads(line) for line in (reports / 'events.jsonl').read_text().splitlines()]
                    assert len(events) == len(sequence), (label, len(events))
                    assert len({event['rid'] for event in events}) == len(sequence)
                    assert Counter(event['request']['uri'] for event in events) == Counter(
                        f'/request-{index}.php' for index, case in enumerate(sequence))
                    for event in events:
                        assert event['category'] == 'error' and event['function'] == 'E_WARNING'
                    if suite == 'bailout':
                        for log in logs:
                            subprocess.run(common + ['--rm', '--entrypoint', 'php', image, '-n',
                                '/ext/audit-bailout.php', '/ext/' + label + '/' + log.name], check=True)
                bodies[mode] = [re.sub(r'PID:\d+', 'PID:WORKER', response['body']) for response in responses]
                print(f'PASS PHP {version} {label}: {len(sequence)} requests, same worker, clean Memcheck access checks', flush=True)
                if suite == 'bailout':
                    for log in logs:
                        print(log.name + ': ' + '; '.join(re.findall(r'(?:definitely|indirectly|possibly) lost: [^\n]+', log.read_text())), flush=True)
            assert bodies['native'] == bodies['prototype'], 'native/prototype response mismatch'
            if suite == 'normal':
                assert losses['native'] == losses['prototype'], losses
                print('PASS: normal shutdown lost bytes/blocks match native FPM exactly', flush=True)
            else:
                check_bailout_loss_delta(losses)
                print('PASS: bailout shutdown lost bytes/blocks match native FPM exactly; no wrapper allowance', flush=True)
            (build / (suite + '-losses.json')).write_text(json.dumps(losses, indent=2))
        print(f'PASS PHP {version}: native/prototype shutdown, destructor, handler and next-request behavior match', flush=True)


if __name__ == '__main__':
    main()
