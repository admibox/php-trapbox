<?php
// Runs under whichever trapbox.unserialize mode the command line sets.
// Requires -dtrapbox.log_path pointing at a writable file.

class TrapboxUnserializeProbe
{
    public function __wakeup()
    {
        $GLOBALS['trapbox_wakeups']++;
    }
}

// The ini parser turns an unquoted `off` into an empty string
$mode = ini_get('trapbox.unserialize') ?: 'off';
$log = ini_get('trapbox.log_path');
$fail = function ($msg) use ($mode) { echo "FAIL: unserialize[$mode]: $msg\n"; };

$payload = serialize(new TrapboxUnserializeProbe());
$hidden = 'a:1:{s:1:"' . "\0" . '";' . $payload . '}';

@unlink($log);
$GLOBALS['trapbox_wakeups'] = 0;

$scalars = unserialize('a:1:{i:0;i:1;}');
$restricted = unserialize($payload, ['allowed_classes' => false]);
$object = unserialize($payload);
$nested = unserialize($hidden);

if ($scalars !== [1]) $fail('an unserialize without objects was affected');
if (!($restricted instanceof __PHP_Incomplete_Class)) $fail('allowed_classes=false was affected');

$real = $mode === 'off' || $mode === 'bypass';
if ($real) {
    if (!($object instanceof TrapboxUnserializeProbe) || $GLOBALS['trapbox_wakeups'] !== 2) $fail('object was not restored');
} else {
    if ($object !== false || $nested !== false) $fail('expected false, got ' . var_export($object, true));
    if ($GLOBALS['trapbox_wakeups'] !== 0) $fail('__wakeup ran');
}

$lines = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];

if ($mode === 'off') {
    if ($lines !== []) $fail('off mode wrote events');
} else {
    $logged = [];
    foreach ($lines as $line) {
        $event = json_decode($line, true);
        if (!is_array($event)) {
            $fail("invalid JSON: $line");
            continue;
        }
        $logged[] = $event['args'][0] ?? null;
        if ($event['category'] !== 'unserialize' || $event['mode'] !== $mode) $fail('wrong category or mode');
        if (basename($event['backtrace'][0]['file'] ?? '') !== 'test_unserialize.php') $fail('backtrace does not name the caller');
    }
    if ($logged !== [$payload, $hidden]) $fail('logged ' . json_encode($logged));
}

@unlink($log);
echo "unserialize[$mode] checks finished\n";
