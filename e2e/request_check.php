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
// php -r itself produces an eval event, so the category must be checked, not just the log's existence
$hasCategory = function ($category) use ($log) {
    return is_file($log) && strpos(file_get_contents($log), "\"category\":\"$category\"") !== false;
};
if (!$hasCategory('exec')) $fail('invalid mode did not keep logging');
@unlink($log);

// A fatal error bails out of the engine, so logging from it must not crash
$fatal = '/tmp/trapbox-fatal.php';
file_put_contents($fatal, '<?php eval("trapbox_no_such_function();");');
@unlink($log);
exec(escapeshellarg(PHP_BINARY) . " -dextension=$ext -dtrapbox.log_path=$log $fatal 2>&1", $fatalOutput, $fatalCode);
if ($fatalCode !== 255) $fail("uncaught error in eval exited with $fatalCode");
if (!$hasCategory('error') || strpos(file_get_contents($log), '"function":"E_ERROR"') === false) $fail('fatal error in eval not logged');
@unlink($log);
@unlink($fatal);

$anyPath = PHP_BINARY . " -dextension=$ext -dtrapbox.log_path=$log";
$write = "-r 'file_put_contents(\"/tmp/trapbox-any.txt\", \"y\");'";
shell_exec("$anyPath $write 2>&1");
if ($hasCategory('file_write')) $fail('a plain write was logged without file_write_any_path');
@unlink($log);
shell_exec("$anyPath -dtrapbox.file_write_any_path=1 $write 2>&1");
if (!$hasCategory('file_write')) $fail('file_write_any_path did not log a plain write');
@unlink($log);
@unlink('/tmp/trapbox-any.txt');

if ($failed) exit(1);
echo "OK: real HTTP request logged correctly\n";
