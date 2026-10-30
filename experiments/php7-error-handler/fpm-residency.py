"""Measure idle FPM worker memory after repeated handler exits, using real RSS.

Two independent native/prototype pairs per PHP version, with Zend MM enabled.
A short allocator-disabled pair acts as a positive control for retained memory.
Neither worker recycling nor forced GC/cache clearing may hide growth.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('local_magento', Path(__file__).with_name('local-magento.py'))
transport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transport)


def memory_summary(samples):
    result = {}
    for metric in ('Rss', 'Pss', 'Private_Dirty', 'Anonymous', 'heap_used', 'heap_allocated'):
        values = [sample[metric] for sample in samples]
        tail = values[len(values) // 2:]
        xs = [sample['exits'] for sample in samples]
        average_x, average_y = sum(xs) / len(xs), sum(values) / len(values)
        slope = sum((x - average_x) * (y - average_y) for x, y in zip(xs, values)) / sum((x - average_x) ** 2 for x in xs)
        result[metric] = {'start': values[0], 'end': values[-1], 'delta': values[-1] - values[0],
                          'tail_range': max(tail) - min(tail), 'bytes_per_exit': slope}
    return result


def run_worker(common, image, build, runtime, mode, allocator, repeat, blocks, batch, warmup):
    label = f'alloc-{allocator}-{mode}-{repeat}'
    reports = build / label
    reports.mkdir()
    (reports / 'events.jsonl').touch()
    name = 'trapbox-residency-' + build.name + '-' + label + '-' + str(os.getpid())
    flags = ['-n', '-dzend_extension=opcache', '-dopcache.enable=1', '-dopcache.file_update_protection=0',
             '-dmemory_limit=128M', '-ddisplay_errors=0', '-dlog_errors=0']
    if mode == 'prototype':
        flags += ['-dextension=/ext/modules/trapbox.so', '-dtrapbox.log_path=/ext/' + label + '/events.jsonl',
                  '-dtrapbox.errors=bypass']
        flags += ['-dtrapbox.' + category + '=off' for category in
                  ('exec', 'eval', 'include', 'file_write', 'secret_read', 'unserialize')]
    command = common + ['--name', name, '-e', f'USE_ZEND_ALLOC={allocator}', '--entrypoint', 'php-fpm',
                        image, '-R', '-y', '/ext/fpm.conf'] + flags
    (build / 'fpm.log').touch()
    (build / 'fpm.log').chmod(0o666)
    worker_pid = None
    samples = []
    responses = 0
    digest = hashlib.sha256()
    started = time.monotonic()

    def send(case):
        nonlocal worker_pid, responses
        result = transport.request(build / 'fpm.sock', 'trapbox.localhost', f'/fixture.php?mode={case}',
                                   '/ext/fpm-memory-target.php', allowed_statuses=(0, 17, 255), script_name='/fixture.php')
        body = result['body']
        pid = int(re.search(r'PID:(\d+)', body)[1])
        if worker_pid is None:
            worker_pid = pid
        assert worker_pid == pid, 'FPM worker was replaced'
        assert result['status'] == 200 and not result['stderr'], result
        assert 'CLEAN:yes' in body and 'OPCACHE:yes' in body, body
        assert body.count('HANDLER\n') == body.count('SHUTDOWN\n') == body.count('DESTRUCT\n') == 1, body
        assert ('AFTER\n' in body) == (case == 'normal'), body
        digest.update(re.sub(r'PID:\d+', 'PID:WORKER', body).encode())
        responses += 1

    def sample(exits):
        result = transport.request(build / 'fpm.sock', 'trapbox.localhost', '/health.php',
                                   '/ext/fpm-residency-probe.php', script_name='/health.php')
        health = json.loads(result['body'])
        assert health['pid'] == worker_pid and health['clean'] and health['opcache'] and health['cached'], health
        assert health['zend_allocator'] == str(allocator) and health['extension'] == (mode == 'prototype'), health
        assert (health['heap_allocated'] > 0) == bool(allocator), health
        time.sleep(0.01)
        # An external process samples the idle worker, not PHP's own heap only.
        raw = subprocess.check_output(runtime + ['exec', name, 'cat', f'/proc/{worker_pid}/smaps_rollup'], text=True)
        (reports / f'smaps-{exits}.txt').write_text(raw)
        metrics = {key: int(value) * 1024 for key, value in re.findall(r'^(\w+):\s+(\d+) kB$', raw, re.M)}
        assert metrics['Swap'] == 0, 'Swapping would obscure resident-memory growth'
        metrics.update(health, exits=exits, responses=responses)
        samples.append(metrics)
        (reports / 'samples.json').write_text(json.dumps(samples, indent=2))
        print(f'{build.name} {label}: {exits} measured exits, RSS={metrics["Rss"] // 1024} KiB, private dirty={metrics["Private_Dirty"] // 1024} KiB, heap={health["heap_allocated"]}', flush=True)

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
                raise RuntimeError('FPM socket timed out')
            for _ in range(100):
                send('normal')
            for _ in range(warmup):
                send('exit')
            sample(0)
            for block in range(1, blocks + 1):
                for _ in range(batch):
                    send('exit')
                # Recovery checks also prevent an exit-only workload hiding corruption.
                for _ in range(25):
                    send('normal')
                sample(block * batch)
        finally:
            subprocess.run(runtime + ['kill', '--signal', 'SIGQUIT', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            try:
                code = process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                subprocess.run(runtime + ['kill', name], stdout=subprocess.DEVNULL, check=False)
                process.wait(timeout=10)
                raise
            finally:
                subprocess.run(runtime + ['rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                if (build / 'fpm.log').exists():
                    shutil.copy(build / 'fpm.log', reports / 'fpm.log')
                    (build / 'fpm.log').unlink()
    assert code == 0, code
    assert not re.search(r'exited on signal|SIGSEGV|zend_mm_heap|child .* exited with code', (reports / 'fpm.log').read_text())
    event_count = 0
    if mode == 'prototype':
        ids = set()
        with (reports / 'events.jsonl').open() as event_file:
            for line in event_file:
                event = json.loads(line)
                assert event['category'] == 'error' and event['function'] == 'E_WARNING'
                assert event['request']['uri'] == '/fixture.php'
                assert event['rid'] not in ids, 'Request ID reused'
                ids.add(event['rid'])
                event_count += 1
        assert event_count == responses, (event_count, responses)
    summary = {'version': build.name, 'mode': mode, 'zend_allocator': allocator, 'repeat': repeat,
               'pid': worker_pid, 'warmup_exits': warmup, 'measured_exits': blocks * batch,
               'responses': responses, 'events': event_count, 'response_digest': digest.hexdigest(),
               'seconds': time.monotonic() - started, 'memory': memory_summary(samples)}
    (reports / 'summary.json').write_text(json.dumps(summary, indent=2))
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--versions', nargs='+', default=['7.3', '7.4'])
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--blocks', type=int, default=10)
    parser.add_argument('--batch', type=int, default=5000)
    parser.add_argument('--warmup', type=int, default=1000)
    args = parser.parse_args()
    assert args.repeats >= 1 and args.blocks >= 4 and args.batch > 0 and args.warmup > 0
    source = Path(__file__).resolve().parent
    root = source.parents[1]
    runtime = ['sudo', 'nerdctl']
    work = Path(tempfile.mkdtemp(prefix='trapbox-fpm-residency.'))
    work.chmod(0o755)
    (work / 'run.json').write_text(json.dumps(vars(args), indent=2))
    print('Artifacts:', work, flush=True)
    summaries = []
    for version in args.versions:
        build = work / version
        subprocess.run(['python3', str(source / 'prepare.py'), str(root), str(build)], check=True)
        for file in ('fpm-memory-target.php', 'fpm-residency-probe.php'):
            shutil.copy(source / file, build)
        image = f'ecommpro/php:{version}-fpm'
        common = runtime + ['run', '-v', str(build) + ':/ext', '-w', '/ext']
        with (build / 'build.log').open('w') as output:
            subprocess.run(common + ['--rm', '--entrypoint', 'sh', image, '-c',
                                    'php -v; phpize && ./configure && make -j2'], stdout=output, stderr=subprocess.STDOUT, check=True)
        (build / 'fpm.conf').write_text('''[global]
daemonize=no
process_control_timeout=10s
error_log=/ext/fpm.log
[www]
user=root
group=root
listen=/ext/fpm.sock
listen.mode=0666
pm=static
pm.max_children=1
pm.max_requests=0
clear_env=no
catch_workers_output=yes
''')
        for repeat in range(1, args.repeats + 1):
            # Reverse the order on the second run to reduce ordering effects.
            modes = ('native', 'prototype') if repeat % 2 else ('prototype', 'native')
            pair = []
            for mode in modes:
                result = run_worker(common, image, build, runtime, mode, 1, repeat, args.blocks, args.batch, args.warmup)
                summaries.append(result)
                pair.append(result)
                (work / 'summary.json').write_text(json.dumps(summaries, indent=2))
                for metric in ('Rss', 'Private_Dirty', 'heap_used', 'heap_allocated'):
                    measured = result['memory'][metric]
                    assert measured['delta'] <= 1024 * 1024, (version, mode, metric, measured)
                    assert measured['tail_range'] <= 256 * 1024, (version, mode, metric, measured)
            assert pair[0]['response_digest'] == pair[1]['response_digest']
            print(f'PASS PHP {version} repeat {repeat}: behavior matches, bounded post-warmup RSS/private/heap growth', flush=True)
        control = []
        for mode in ('native', 'prototype'):
            result = run_worker(common, image, build, runtime, mode, 0, 1, 10, 100, 100)
            summaries.append(result)
            control.append(result)
            (work / 'summary.json').write_text(json.dumps(summaries, indent=2))
            assert result['memory']['Private_Dirty']['delta'] > 1024 * 1024, 'Positive control failed to show retained memory'
        assert control[0]['response_digest'] == control[1]['response_digest']
        print(f'PASS PHP {version}: allocator-disabled positive control detects memory growth', flush=True)
    print('PASS: stable-worker measurements complete; full samples and summaries retained.', flush=True)


if __name__ == '__main__':
    main()
