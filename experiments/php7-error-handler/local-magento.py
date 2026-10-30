"""Compare native PHP and the isolated prototype against a running local Magento.

Uses its exact image, mounts and network, but a separate single-worker FPM and
Unix socket. GET requests can update application sessions, caches and logs.
No storefront configuration is changed. Artifacts may contain application data.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from urllib.parse import urlsplit


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def record(kind, content):
    return struct.pack('!BBHHBB', 1, kind, 1, len(content), 0, 0) + content


def pair(key, value):
    key, value = key.encode(), value.encode()
    def length(size):
        return bytes([size]) if size < 128 else struct.pack('!I', size | 0x80000000)
    return length(len(key)) + length(len(value)) + key + value


def normalize_body(body):
    # Normalize only per-request values observed in the real storefront.
    for token in re.findall(r'name="form_key"[^>]*value="([A-Za-z0-9]{16})"', body):
        body = body.replace(token, 'FORM_KEY')
    for token in re.findall(r'id="(remember_me[A-Za-z0-9]{10})"', body):
        body = body.replace(token, 'remember_meID')
    def checkout(match):
        data = json.loads(match[1])
        for captcha in data.get('captcha', {}).values():
            if 'timestamp' in captcha:
                captcha['timestamp'] = 0
        return 'window.checkout = ' + json.dumps(data, sort_keys=True) + ';'
    return re.sub(r'window.checkout = (.*);', checkout, body)


def request(path, host, uri, script, allowed_statuses=(0,), script_name='/index.php'):
    params = {
        'SCRIPT_FILENAME': script, 'SCRIPT_NAME': script_name,
        'DOCUMENT_ROOT': '/var/www/pub', 'REQUEST_METHOD': 'GET',
        'REQUEST_URI': uri, 'QUERY_STRING': urlsplit(uri).query,
        'SERVER_PROTOCOL': 'HTTP/1.1', 'SERVER_NAME': host,
        'SERVER_PORT': '443', 'HTTP_HOST': host, 'HTTPS': 'on',
        'HTTP_X_FORWARDED_PROTO': 'https', 'HTTP_ACCEPT': 'text/html',
        'HTTP_USER_AGENT': 'Trapbox local compatibility test',
        'GATEWAY_INTERFACE': 'CGI/1.1', 'REMOTE_ADDR': '127.0.0.1',
    }
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(90)
        connection.connect(str(path))
        connection.sendall(record(1, struct.pack('!HB5x', 1, 0))
                           + record(4, b''.join(pair(k, v) for k, v in params.items()))
                           + record(4, b'') + record(5, b''))
        def read(size):
            data = b''
            while len(data) < size:
                chunk = connection.recv(size - len(data))
                if not chunk:
                    raise RuntimeError('Incomplete FastCGI response')
                data += chunk
            return data
        output, errors = b'', b''
        while True:
            version, kind, ident, size, padding, _ = struct.unpack('!BBHHBB', read(8))
            assert version == 1 and ident == 1
            content = read(size)
            read(padding)
            if kind == 6:
                output += content
            elif kind == 7:
                errors += content
            elif kind == 3:
                app_status, protocol_status = struct.unpack('!IB3x', content)
                assert app_status in allowed_statuses and protocol_status == 0
                break
    headers, body = output.split(b'\r\n\r\n', 1)
    status = re.search(rb'^Status: (\d+)', headers, re.M)
    title = re.search(rb'<title>(.*?)</title>', body, re.S)
    return {'status': int(status[1]) if status else 200,
            'title': title[1].decode() if title else None,
            'bytes': len(body), 'body': body.decode(errors='replace'),
            'headers': headers.decode(), 'stderr': errors.decode(errors='replace')}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--container', default='ideavinos-php-fpm-1')
    parser.add_argument('--host', default='ideavinos.localhost')
    parser.add_argument('--route', action='append')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    work = Path(tempfile.mkdtemp(prefix='trapbox-local-magento.'))
    work.chmod(0o755)
    print(f'Artifacts: {work}', flush=True)
    runtime = ['sudo', 'nerdctl']
    image = run(*runtime, 'inspect', '--format', '{{.Config.Image}}', args.container)
    # Copy only the two environment settings needed by the application's prepend
    # and local mail transport; never print the container environment.
    config = json.loads(run(*runtime, 'inspect', args.container))[0]['Config']
    environment = [entry for entry in config.get('Env', [])
                   if entry.split('=', 1)[0] in ('PHP_AUTO_PREPEND_FILE', 'SENDMAIL_COMMAND')]
    subprocess.run(['python3', str(root / 'experiments/php7-error-handler/prepare.py'),
                    str(root), str(work)], check=True)
    shutil.copy(Path(__file__).with_name('magento-probe.php'), work)
    (work / 'entry.php').write_text('''<?php
header('X-Trapbox-Test-Pid: ' . getmypid());
header('X-Trapbox-Test-Loaded: ' . (extension_loaded('trapbox') ? 'yes' : 'no'));
header('X-Trapbox-Test-Opcache: ' . (opcache_get_status(false)['opcache_enabled'] ? 'yes' : 'no'));
require '/var/www/pub/index.php';
''')
    (work / 'fpm.conf').write_text('''[global]
daemonize=no
error_log=/probe/fpm.log
[www]
user=1000
group=1000
listen=/probe/fpm.sock
listen.mode=0666
pm=static
pm.max_children=1
clear_env=no
catch_workers_output=yes
chdir=/var/www
''')
    common = runtime + ['run', '--volumes-from', args.container,
                        '--network', 'container:' + args.container,
                        '-v', str(work) + ':/probe', '-w', '/probe']
    for entry in environment:
        common += ['-e', entry]
    with (work / 'build.log').open('w') as log:
        subprocess.run(common + ['--rm', '--entrypoint', 'sh', image, '-c',
                       'phpize && ./configure && make -j2'], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    routes = args.route or ['/', '/vino-envinate-taganan-tinto.html', '/customer/account/login/',
                            '/catalogsearch/result/?q=rioja', '/trapbox-missing-page']
    summaries = {}
    for mode in ('native', 'prototype'):
        name = 'trapbox-magento-' + mode + '-' + str(os.getpid())
        flags = ['-dauto_prepend_file=/usr/local/share/php/prepend.php.inc']
        if mode == 'prototype':
            flags += ['-dextension=/probe/modules/trapbox.so',
                      '-dtrapbox.log_path=/probe/events.jsonl']
            flags += ['-dtrapbox.' + category + '=bypass' for category in
                      ('exec', 'eval', 'include', 'file_write', 'secret_read', 'unserialize', 'errors')]
        try:
            (work / 'fpm.log').touch()
            (work / 'fpm.log').chmod(0o666)
            run(*(common + ['-d', '--name', name, '--entrypoint', 'php-fpm',
                            image, '-R', '-y', '/probe/fpm.conf'] + flags))
            for attempt in range(100):
                if (work / 'fpm.sock').exists():
                    break
                time.sleep(0.1)
            results = []
            for iteration in range(3):
                for uri in routes:
                    result = request(work / 'fpm.sock', args.host, uri, '/probe/entry.php')
                    result.update(uri=uri, iteration=iteration)
                    results.append(result)
                    print(f'{mode}: {uri} -> {result["status"]}, {result["bytes"]} bytes', flush=True)
                probe = request(work / 'fpm.sock', args.host, '/trapbox-internal-probe',
                                '/probe/magento-probe.php')
                results.append(dict(probe, uri='handler-probe', iteration=iteration))
            (work / (mode + '.json')).write_text(json.dumps(results, indent=2))
            summaries[mode] = results
        finally:
            subprocess.run(runtime + ['stop', '-t', '5', name], stdout=subprocess.DEVNULL, check=False)
            subprocess.run(runtime + ['rm', name], stdout=subprocess.DEVNULL, check=False)
            if (work / 'fpm.log').exists():
                shutil.copy(work / 'fpm.log', work / (mode + '-fpm.log'))
                (work / 'fpm.log').unlink()
    for mode, results in summaries.items():
        pids = set()
        for result in results:
            if result['uri'] == 'handler-probe':
                data = json.loads(result['body'])
                assert data['exception'][0] == 'Exception' and data['identity'] and data['restored']
                assert data['suppressed'] is False and data['fallback'] is False
            else:
                if result['uri'] == '/catalogsearch/result/?q=rioja' and result['status'] == 500:
                    assert 'no mapping found for field [tm_value]' in result['body']
                    print(f'KNOWN BASELINE FAILURE ({mode}): Elasticsearch tm_value mapping')
                else:
                    assert result['status'] == (404 if result['uri'] == '/trapbox-missing-page' else 200)
                    assert result['title'] and result['bytes'] > 1000
                pids.add(re.search(r'X-Trapbox-Test-Pid: (\d+)', result['headers'])[1])
                expected = 'yes' if mode == 'prototype' else 'no'
                assert 'X-Trapbox-Test-Loaded: ' + expected in result['headers']
                assert 'X-Trapbox-Test-Opcache: yes' in result['headers']
        assert len(pids) == 1, (mode, 'worker changed')
        assert not re.search(r'exited on signal|SIGSEGV|zend_mm_heap',
                             (work / (mode + '-fpm.log')).read_text())
    for native, proxy in zip(summaries['native'], summaries['prototype']):
        for field in ('status', 'title'):
            assert native[field] == proxy[field], (native['uri'], field)
        if native['uri'] == 'handler-probe':
            assert native['body'] == proxy['body']
        else:
            assert normalize_body(native['body']) == normalize_body(proxy['body']), native['uri']
    events = [json.loads(line) for line in (work / 'events.jsonl').read_text().splitlines()]
    handler_events = [event for event in events if event.get('category') == 'error'
                      and 'magento-handler.php' in json.dumps(event)]
    assert len(handler_events) == 9, len(handler_events)
    print('PASS: real routes status/title/normalized HTML, native handler behavior, 9 warning events, stable workers.')
    print('HTML normalization: form keys, remember-me IDs and captcha timestamps only.')


if __name__ == '__main__':
    main()
