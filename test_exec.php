<?php
// Runs under whichever trapbox.exec mode the command line sets.
// Requires -dtrapbox.log_path pointing at a writable file.

// The ini parser turns an unquoted `off` into an empty string
$mode = ini_get('trapbox.exec') ?: 'off';
$log = ini_get('trapbox.log_path');
$fail = function ($msg) use ($mode) { echo "FAIL: exec[$mode]: $msg\n"; };

@unlink($log);

$cmd = 'echo trapbox_marker';
$shell = shell_exec($cmd);
$backtick = `echo trapbox_marker`;
$out = [];
$exec = exec($cmd, $out);
ob_start();
$system = system($cmd);
$systemOutput = ob_get_clean();
ob_start();
$passthru = passthru($cmd);
$passthruOutput = ob_get_clean();
$popen = popen($cmd, 'r');
$pipes = [];
$proc = proc_open($cmd, [1 => ['pipe', 'w']], $pipes);

$real = $mode === 'off' || $mode === 'bypass';

if ($real) {
    if ($shell !== "trapbox_marker\n") $fail('shell_exec did not run the command');
    if ($backtick !== "trapbox_marker\n") $fail('backtick did not run the command');
    if ($exec !== 'trapbox_marker' || $out !== ['trapbox_marker']) $fail('exec lost its return value or by-ref output');
    if ($system !== 'trapbox_marker' || $systemOutput !== "trapbox_marker\n") $fail('system did not run the command');
    if ($passthruOutput !== "trapbox_marker\n") $fail('passthru did not run the command');
    if (!is_resource($popen)) $fail('popen returned no resource');
    if (!is_resource($proc) || !isset($pipes[1])) $fail('proc_open lost its resource or by-ref pipes');
} else {
    if (strpos((string) $shell, 'trapbox_marker') !== false) $fail('shell_exec ran the command');
    if (strpos((string) $backtick, 'trapbox_marker') !== false) $fail('backtick ran the command');
    if ($out !== [] && strpos(implode('', $out), 'trapbox_marker') !== false) $fail('exec ran the command');
    if (strpos($systemOutput, 'trapbox_marker') !== false) $fail('system ran the command');
    if (strpos($passthruOutput, 'trapbox_marker') !== false) $fail('passthru ran the command');
    if (is_resource($popen)) $fail('popen ran the command');
    if (is_resource($proc)) $fail('proc_open ran the command');
}

if (is_resource($popen)) pclose($popen);
if (is_resource($proc)) proc_close($proc);

$lines = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];

if ($mode === 'off') {
    if ($lines !== []) $fail('off mode wrote events');
} else {
    $functions = [];
    foreach ($lines as $line) {
        $event = json_decode($line, true);
        if (!is_array($event)) {
            $fail("invalid JSON: $line");
            continue;
        }
        foreach (['ts', 'rid', 'category', 'mode', 'function', 'args', 'request', 'backtrace'] as $key) {
            if (!array_key_exists($key, $event)) $fail("event lacks $key");
        }
        $functions[] = $event['function'];
        if ($event['category'] !== 'exec') $fail('wrong category');
        if ($event['mode'] !== $mode) $fail('wrong mode in event');
        if ($event['args'] !== [$cmd]) $fail('wrong args: ' . json_encode($event['args']));
        if (strlen($event['rid']) !== 12) $fail('bad rid');
        $frame = $event['backtrace'][0] ?? [];
        if (basename($frame['file'] ?? '') !== 'test_exec.php' || ($frame['line'] ?? 0) < 1) {
            $fail('backtrace does not name the caller: ' . json_encode($event['backtrace']));
        }
    }

    $expected = ['shell_exec', 'shell_exec', 'exec', 'system', 'passthru', 'popen', 'proc_open'];
    if ($functions !== $expected) $fail('logged ' . json_encode($functions));

    $rids = [];
    foreach ($lines as $line) {
        $event = json_decode($line, true);
        if (is_array($event)) $rids[$event['rid']] = true;
    }
    if (count($rids) > 1) $fail('rid changed within one request');
}

@unlink($log);
echo "exec[$mode] checks finished\n";
