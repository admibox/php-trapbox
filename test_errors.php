<?php
// Runs under whichever trapbox.errors mode the command line sets.
// Requires -dtrapbox.log_path pointing at a writable file.

// The ini parser turns an unquoted `off` into an empty string
$mode = ini_get('trapbox.errors') ?: 'off';
$log = ini_get('trapbox.log_path');
$fail = function ($msg) use ($mode) { echo "FAIL: errors[$mode]: $msg\n"; };

$missing = '/nonexistent-trapbox';

@unlink($log);

// The cases of the original incident: failures that name the attacker's target
@file_get_contents("$missing/app/etc/env.php");
@fopen("$missing/page/require_js.phtml", 'w');
eval('@file_get_contents("' . $missing . '/from-eval.txt");');

// Ordinary application noise
@file_get_contents("$missing/plain.txt");
@$undefined['x'];

$errorEvents = function () use ($log, $fail) {
    $found = [];
    foreach (is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [] as $line) {
        $event = json_decode($line, true);
        if (!is_array($event)) {
            $fail("invalid JSON: $line");
            continue;
        }
        if ($event['category'] === 'error') $found[] = $event;
    }
    return $found;
};

$events = $errorEvents();
if ($mode === 'off') {
    if ($events !== []) $fail('off mode wrote events');
} else {
    if (count($events) !== 3) $fail('expected 3 events, got ' . json_encode(array_column($events, 'args')));
    foreach (['env.php', 'require_js.phtml', 'from-eval.txt'] as $i => $needle) {
        $event = $events[$i] ?? [];
        if (strpos($event['args'][0] ?? '', $needle) === false) $fail("event $i does not mention $needle");
        if (($event['function'] ?? '') !== 'E_WARNING') $fail("event $i has the wrong level");
        if (($event['mode'] ?? '') !== 'bypass') $fail("event $i must report bypass, errors are only observed");
        if (basename(explode(':', $event['args'][1] ?? '')[0]) !== 'test_errors.php' && $i < 2) $fail("event $i has the wrong location");
    }
    if (strpos($events[2]['args'][1] ?? '', "eval()'d code") === false) $fail('eval origin not reported');
    if (basename($events[0]['backtrace'][0]['file'] ?? '') !== 'test_errors.php') $fail('backtrace does not name the caller');
}

// With an application error handler
@unlink($log);
$handled = 0;
set_error_handler(function () use (&$handled) { $handled++; return true; });
file_get_contents("$missing/handled/env.php");
restore_error_handler();

if ($handled !== 1) $fail('the application error handler was not called');
$events = $errorEvents();
// PHP 7 does not call zend_error_cb when a user handler takes the error
$expected = ($mode === 'off' || PHP_VERSION_ID < 80000) ? 0 : 1;
if (count($events) !== $expected) $fail("with a user handler expected $expected events, got " . count($events));

@unlink($log);
echo "errors[$mode] checks finished\n";
