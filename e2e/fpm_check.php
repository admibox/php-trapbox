<?php
// Usage: php -n e2e/fpm_check.php <mode> <jit setting or "none">
// Reuses one FPM worker across requests with OPcache on, to expose pointers kept between requests.

$mode = $argv[1];
$jit = $argv[2];
$label = "fpm[$mode,jit=$jit]";
$port = 9123;
$log = '/tmp/trapbox-fpm.jsonl';
$fpmLog = '/tmp/trapbox-php-fpm.log';
$conf = '/tmp/trapbox-php-fpm.conf';
$target = __DIR__ . '/fpm_target.php';
$ext = dirname(__DIR__) . '/modules/trapbox.so';
$requests = 100;
$failed = false;
$fail = function ($msg) use (&$failed, $label) { echo "FAIL: $label: $msg\n"; $failed = true; };

@unlink($log);
@unlink($fpmLog);
@unlink('/tmp/trapbox-fpm.phar');
$phar = new Phar('/tmp/trapbox-fpm.phar');
$phar['x.php'] = '<?php return "trapbox_phar_marker";';
unset($phar);
file_put_contents('/tmp/trapbox-fpm-env.php', 'secret');
file_put_contents($conf, "[global]\nerror_log=$fpmLog\ndaemonize=no\n[www]\nlisten=127.0.0.1:$port\npm=static\npm.max_children=1\ncatch_workers_output=yes\n");

$flags = "-dextension=$ext -dtrapbox.log_path=$log -dtrapbox.exec=$mode -dtrapbox.eval=$mode -dtrapbox.include=$mode -dtrapbox.file_write=$mode -dtrapbox.secret_read=$mode -dopcache.enable=1 -dopcache.validate_timestamps=0 -dopcache.file_update_protection=0";
if (strpos((string) shell_exec('php-fpm -m'), 'Zend OPcache') === false) {
    $flags = "-dzend_extension=opcache $flags";
}
if ($jit !== 'none') {
    $flags .= " -dopcache.jit_buffer_size=32M -dopcache.jit=$jit -dopcache.jit_hot_func=1 -dopcache.jit_hot_loop=1";
}
$server = proc_open("exec php-fpm -R -y $conf $flags", [1 => ['file', '/dev/null', 'w'], 2 => ['file', $fpmLog, 'a']], $pipes);

function fcgi_record($type, $content)
{
    return pack('CCnnCx', 1, $type, 1, strlen($content), 0) . $content;
}

function fcgi_pair($name, $value)
{
    $encode = function ($length) { return $length < 128 ? chr($length) : pack('N', $length | 0x80000000); };
    return $encode(strlen($name)) . $encode(strlen($value)) . $name . $value;
}

function fcgi_post($port, $script, $body)
{
    $socket = @stream_socket_client("tcp://127.0.0.1:$port", $errno, $error, 5);
    if (!$socket) return null;
    stream_set_timeout($socket, 15);

    $params = [
        'SCRIPT_FILENAME' => $script,
        'REQUEST_METHOD' => 'POST',
        'REQUEST_URI' => '/fpm_target.php',
        'CONTENT_TYPE' => 'application/json',
        'CONTENT_LENGTH' => (string) strlen($body),
        'REMOTE_ADDR' => '192.0.2.1',
        'HTTP_X_FORWARDED_FOR' => '6.6.6.6',
    ];
    $encoded = '';
    foreach ($params as $name => $value) $encoded .= fcgi_pair($name, $value);

    fwrite($socket, fcgi_record(1, pack('nCx5', 1, 0))
        . fcgi_record(4, $encoded) . fcgi_record(4, '')
        . fcgi_record(5, $body) . fcgi_record(5, ''));

    $stdout = '';
    $ended = false;
    while (!$ended && !feof($socket)) {
        $header = fread($socket, 8);
        if ($header === false || strlen($header) < 8) break;
        $record = unpack('Cversion/Ctype/nid/nlength/Cpadding/Creserved', $header);
        $remaining = $record['length'] + $record['padding'];
        $content = '';
        while ($remaining > 0 && !feof($socket)) {
            $chunk = fread($socket, $remaining);
            if ($chunk === false || $chunk === '') break;
            $content .= $chunk;
            $remaining -= strlen($chunk);
        }
        if ($record['type'] === 6) $stdout .= substr($content, 0, $record['length']);
        if ($record['type'] === 3) $ended = true;
    }
    fclose($socket);
    if (!$ended) return null;

    $split = strpos($stdout, "\r\n\r\n");
    return $split === false ? $stdout : substr($stdout, $split + 4);
}

for ($i = 0; $i < 50; $i++) {
    $socket = @fsockopen('127.0.0.1', $port);
    if ($socket) {
        fclose($socket);
        break;
    }
    usleep(100000);
}

$body = '{"query":"mutation attack"}';
$shouldRun = in_array($mode, ['off', 'bypass'], true);
$pids = [];
$last = null;
for ($i = 0; $i < $requests; $i++) {
    $response = fcgi_post($port, $target, $body);
    $data = $response === null ? null : json_decode($response, true);
    if (!is_array($data)) {
        $fail("request $i got no valid response: " . var_export($response, true));
        break;
    }
    $pids[$data['pid']] = true;
    if ($data['ran'] !== $shouldRun || $data['evaluated'] !== $shouldRun || $data['included'] !== $shouldRun || $data['read'] !== $shouldRun || $data['wrote'] !== $shouldRun) {
        $fail("request $i: " . json_encode($data));
        break;
    }
    $last = $data;
}

proc_terminate($server);
proc_close($server);

if (count($pids) > 1) $fail('worker was replaced: ' . count($pids) . ' pids');
if ($last) {
    if (!$last['opcache'] || !$last['cached']) $fail('OPcache was not active, the test proves nothing');
    if ($jit !== 'none' && !$last['jit']) $fail('JIT was not active, the test proves nothing');
    if ($mode === 'bypass' && !$last['phar_cached']) $fail('phar was not cached, the event count proves nothing');
}

$workerLog = is_file($fpmLog) ? file_get_contents($fpmLog) : '';
if (preg_match('/SIG[A-Z]+|Segmentation|core dumped|exited on signal|zend_mm_heap/', $workerLog, $match)) {
    $fail("fpm log reports {$match[0]}");
}

$lines = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];
// 3 exec + 3 eval + 1 include + 1 read + 1 write per request. The include count proves OPcache does not hide cached phars.
$expected = $mode === 'off' ? 0 : $requests * 9;
if (count($lines) !== $expected) $fail('expected ' . $expected . ' events, got ' . count($lines));

$rids = [];
foreach ($lines as $line) {
    $event = json_decode($line, true);
    if (!is_array($event)) {
        $fail("invalid JSON: $line");
        break;
    }
    $rids[$event['rid']] = true;
    if (($event['request']['body'] ?? null) !== $body || ($event['request']['ip'] ?? null) !== '192.0.2.1') {
        $fail('wrong request context: ' . $line);
        break;
    }
}
if ($mode !== 'off' && count($rids) !== $requests) $fail('expected one rid per request, got ' . count($rids));

@unlink($log);
if ($failed) {
    echo substr($workerLog, -1500), "\n";
    exit(1);
}
echo "OK: $label\n";
