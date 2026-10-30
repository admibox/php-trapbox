<?php
// Runs under whichever trapbox.secret_read mode the command line sets.
// Requires -dtrapbox.log_path and -dtrapbox.canary_env_path.

// The ini parser turns an unquoted `off` into an empty string
$mode = ini_get('trapbox.secret_read') ?: 'off';
$log = ini_get('trapbox.log_path');
$canary = ini_get('trapbox.canary_env_path');
$fail = function ($msg) use ($mode) { echo "FAIL: secret_read[$mode]: $msg\n"; };

$dir = sys_get_temp_dir() . '/trapbox-read-' . getmypid();
mkdir($dir);
$secret = "$dir/env.php";
$dotenv = "$dir/.env";
$plain = "$dir/plain.txt";
file_put_contents($secret, 'KEY=REAL');
file_put_contents($dotenv, 'KEY=REAL');
file_put_contents($plain, 'plain');
file_put_contents($canary, 'KEY=CANARY');

@unlink($log);

$contents = file_get_contents($secret);
ob_start();
$readLength = readfile($secret);
$readOutput = ob_get_clean();
$lines = file($secret);
$handle = fopen($secret, 'r');
$streamed = $handle ? fread($handle, 100) : null;
if ($handle) fclose($handle);
$ini = parse_ini_file($dotenv);
$copied = copy($secret, "$dir/stolen.txt");
clearstatcache();
$stolen = file_exists("$dir/stolen.txt") ? file_get_contents("$dir/stolen.txt") : null;

if (file_get_contents($plain) !== 'plain') $fail('a plain read was affected');

$real = $mode === 'off' || $mode === 'bypass';
if ($real || $mode === 'trap') {
    $value = $real ? 'KEY=REAL' : 'KEY=CANARY';
    if ($contents !== $value) $fail('file_get_contents returned ' . var_export($contents, true));
    if ($readOutput !== $value || $readLength !== strlen($value)) $fail('readfile returned ' . var_export($readOutput, true));
    if ($lines !== [$value]) $fail('file returned ' . json_encode($lines));
    if ($streamed !== $value) $fail('fopen read ' . var_export($streamed, true));
    if ($ini !== ['KEY' => substr($value, 4)]) $fail('parse_ini_file returned ' . json_encode($ini));
    if ($copied !== true || $stolen !== $value) $fail('copy produced ' . var_export($stolen, true));
} else {
    foreach ([$contents, $readLength, $lines, $handle, $ini, $copied] as $value) {
        if ($value !== false) $fail('expected false, got ' . var_export($value, true));
    }
    if ($stolen !== null) $fail('copy created a file');
}

$events = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];

if ($mode === 'off') {
    if ($events !== []) $fail('off mode wrote events');
} else {
    $functions = [];
    foreach ($events as $line) {
        $event = json_decode($line, true);
        if (!is_array($event)) {
            $fail("invalid JSON: $line");
            continue;
        }
        $functions[] = $event['function'];
        if ($event['category'] !== 'secret_read' || $event['mode'] !== $mode) $fail('wrong category or mode');
        if (basename($event['backtrace'][0]['file'] ?? '') !== 'test_secret_read.php') $fail('backtrace does not name the caller');
    }
    if ($functions !== ['file_get_contents', 'readfile', 'file', 'fopen', 'parse_ini_file', 'copy']) {
        $fail('logged ' . json_encode($functions));
    }
}

@unlink($log);
@unlink($canary);
foreach (scandir($dir) as $name) {
    if ($name !== '.' && $name !== '..') unlink("$dir/$name");
}
rmdir($dir);
echo "secret_read[$mode] checks finished\n";
