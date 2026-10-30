<?php
// Runs WITHOUT the extension and drives a real HTTP request through php -S.

$ext = getcwd() . '/modules/trapbox.so';
$log = '/tmp/trapbox-request.jsonl';
$port = 8099;
$failed = false;
$fail = function ($msg) use (&$failed) { echo "FAIL: request: $msg\n"; $failed = true; };

@unlink($log);

$command = escapeshellarg(PHP_BINARY) . " -dextension=$ext -dtrapbox.log_path=$log -dtrapbox.exec=block -S 127.0.0.1:$port -t " . escapeshellarg(__DIR__);
$server = proc_open(
    "exec $command",
    [1 => ['file', '/dev/null', 'w'], 2 => ['file', '/dev/null', 'w']],
    $pipes
);

for ($i = 0; $i < 50; $i++) {
    $socket = @fsockopen('127.0.0.1', $port);
    if ($socket) {
        fclose($socket);
        break;
    }
    usleep(100000);
}

$body = '{"query":"mutation \"x\""}' . "\xff\x00";
$response = @file_get_contents("http://127.0.0.1:$port/request_target.php?a=1", false, stream_context_create(['http' => [
    'method' => 'POST',
    'header' => "Content-Type: application/json\r\nX-Forwarded-For: 6.6.6.6\r\nCookie: session=secret\r\nAuthorization: Bearer secret\r\n",
    'content' => $body,
]]));

proc_terminate($server);
proc_close($server);

if ($response === false) $fail('no response from server');
if (strpos((string) $response, 'trapbox_marker') !== false) $fail('command ran in block mode');

$lines = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];
if (count($lines) !== 1) {
    $fail('expected 1 event, got ' . count($lines));
} else {
    $event = json_decode($lines[0], true);
    if (!is_array($event)) {
        $fail("invalid JSON: {$lines[0]}");
    } else {
        $request = $event['request'];
        if ($request['method'] !== 'POST') $fail('wrong method');
        if ($request['uri'] !== '/request_target.php?a=1') $fail('wrong uri: ' . $request['uri']);
        if ($request['ip'] !== '127.0.0.1') $fail('ip taken from a forged header: ' . $request['ip']);
        if (($request['headers']['x-forwarded-for'] ?? '') !== '6.6.6.6') $fail('forwarding header not logged');
        if (($request['headers']['content-type'] ?? '') !== 'application/json') $fail('content-type not logged');
        if (isset($request['headers']['cookie']) || isset($request['headers']['authorization'])) $fail('credentials logged');
        if (($request['body'] ?? null) !== '{"query":"mutation \"x\""}' . "\u{00ff}\u{0000}") $fail('body not preserved: ' . json_encode($request['body'] ?? null));
    }
}
@unlink($log);

$typo = shell_exec(escapeshellarg(PHP_BINARY) . " -dextension=$ext -dtrapbox.log_path=$log -dtrapbox.exec=blokc -r 'echo shell_exec(\"echo ran\");' 2>&1");
if (strpos((string) $typo, 'ran') === false) $fail('invalid mode changed behavior');
if (strpos((string) $typo, 'invalid value') === false) $fail('invalid mode raised no warning');
if (!is_file($log)) $fail('invalid mode did not keep logging');
@unlink($log);

if ($failed) exit(1);
echo "OK: real HTTP request logged correctly\n";
