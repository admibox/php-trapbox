"""Validate actual concurrent embedded-PHP ZTS requests, native versus prototype."""
from collections import Counter, defaultdict
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

source = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cold', action='store_true', help='Start the first requests concurrently, without a main-thread bootstrap request')
args = parser.parse_args()
root = source.parents[1]
work = Path(tempfile.mkdtemp(prefix='trapbox-zts-threads.'))
work.chmod(0o755)
print('Artifacts:', work, flush=True)
(work / 'run.json').write_text(json.dumps({'cold_start': args.cold, 'threads': 8, 'requests_per_thread': 100}))
for version in ('7.3', '7.4'):
    build = work / version
    subprocess.run(['python3', str(source / 'prepare.py'), str(root), str(build)], check=True)
    for file in ('zts-threads.c', 'zts-thread-target.php'):
        shutil.copy(source / file, build)
    common = ['sudo', 'nerdctl', 'run', '--rm', '--entrypoint', 'sh', '-v', str(build) + ':/ext', '-w', '/ext',
              '-e', 'PHPRC=/ext/thread.ini', '-e', 'PHP_INI_SCAN_DIR=/ext/no-scan', 'php:' + version + '-zts', '-c']
    with (build / 'build.log').open('w') as output:
        subprocess.run(common + ['phpize && ./configure && make -j2 && cc -g -O1 $(php-config --includes) zts-threads.c -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lphp7 -lpthread -o zts-threads'], stdout=output, stderr=subprocess.STDOUT, check=True)
    results = {}
    for mode in ('native', 'prototype'):
        (build / 'thread-results').mkdir()
        ini = 'allow_url_include=1\ndisplay_errors=0\nlog_errors=0\nerror_reporting=32767\n'
        if mode == 'prototype':
            (build / 'events.jsonl').touch()
            ini += 'extension=/ext/modules/trapbox.so\ntrapbox.log_path=/ext/events.jsonl\n'
            ini += '\n'.join('trapbox.' + category + '=off' for category in ('exec', 'file_write', 'secret_read', 'unserialize')) + '\n'
        (build / 'thread.ini').write_text(ini)
        with (build / (mode + '.log')).open('w') as output:
            command = 'TRAPBOX_COLD_START=1 ./zts-threads' if args.cold else './zts-threads'
            name = 'trapbox-zts-' + version + '-' + mode + '-' + work.name.rsplit('.', 1)[-1]
            named = common[:3] + ['--name', name] + common[3:]
            try:
                subprocess.run(named + [command], stdout=output, stderr=subprocess.STDOUT, timeout=180, check=True)
            except BaseException:
                subprocess.run(['sudo', 'nerdctl', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                raise
        files = sorted((build / 'thread-results').glob('*.json'))
        assert len(files) == 800, len(files)
        results[mode] = {}
        pids = set()
        for file in files:
            data = json.loads(file.read_text())
            assert data['clean'] and data['zts'], data
            assert data['tag'] == data['handler'] == data['eval'] == data['include'], data
            assert data['after'] == (data['mode'] in (0, 1)), data
            assert data['caught'] == (data['mode'] == 1), data
            pids.add(data.pop('pid'))
            results[mode][file.name] = data
        assert len(pids) == 1
        (build / 'thread-results').rename(build / (mode + '-results'))
        print(f'PASS PHP {version} ZTS {mode}: 800 requests / 8 concurrent threads, handler isolation, bailout recovery', flush=True)
    assert results['native'] == results['prototype']
    events = [json.loads(line) for line in (build / 'events.jsonl').read_text().splitlines()]
    assert len(events) == 2400, len(events)
    by_rid = defaultdict(list)
    tags = set()
    for event in events:
        by_rid[event['rid']].append(event)
    assert len(by_rid) == 800, ('request ID count', len(by_rid))
    for rid, group in by_rid.items():
        assert Counter(e['category'] for e in group) == {'error': 1, 'eval': 1, 'include': 1}
        request_tags = {re.search(r'thread-\d+-request-\d+', json.dumps(e['args']))[0] for e in group}
        assert len(request_tags) == 1, (rid, request_tags)
        tags.update(request_tags)
    assert len(tags) == 800
    print(f'PASS PHP {version} ZTS: native/prototype equality, 2400 events, 800 isolated request IDs', flush=True)
