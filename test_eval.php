<?php
// Runs under whichever trapbox.eval mode the command line sets.
// Requires -dtrapbox.log_path pointing at a writable file.

// The ini parser turns an unquoted `off` into an empty string
$mode = ini_get('trapbox.eval') ?: 'off';
$log = ini_get('trapbox.log_path');
$fail = function ($msg) use ($mode) { echo "FAIL: eval[$mode]: $msg\n"; };

@unlink($log);

$GLOBALS['trapbox_ran'] = 0;
$source = '$GLOBALS["trapbox_ran"]++; return 42;';
$nestedSource = 'return eval("return 7;");';

$result = eval($source);
$nested = eval($nestedSource);

$real = $mode === 'off' || $mode === 'bypass';
$expectedSources = [$source, $nestedSource];

if ($real) {
    if ($result !== 42 || $GLOBALS['trapbox_ran'] !== 1) $fail('eval did not run the code');
    if ($nested !== 7) $fail('nested eval returned ' . var_export($nested, true));
    $expectedSources[] = 'return 7;';
    try {
        eval('this is not php');
        $fail('parse error was swallowed');
    } catch (ParseError $e) {
    }
    $expectedSources[] = 'this is not php';
} else {
    if ($GLOBALS['trapbox_ran'] !== 0) $fail('eval ran the code');
    $expected = $mode === 'block' ? false : null;
    if ($result !== $expected) $fail('eval returned ' . var_export($result, true));
    if ($nested !== $expected) $fail('nested eval returned ' . var_export($nested, true));
}

$lines = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];

if ($mode === 'off') {
    if ($lines !== []) $fail('off mode wrote events');
} else {
    $sources = [];
    $rids = [];
    foreach ($lines as $line) {
        $event = json_decode($line, true);
        if (!is_array($event)) {
            $fail("invalid JSON: $line");
            continue;
        }
        $sources[] = $event['args'][0] ?? null;
        $rids[$event['rid']] = true;
        if ($event['category'] !== 'eval' || $event['function'] !== 'eval') $fail('wrong category or function');
        if ($event['mode'] !== $mode) $fail('wrong mode in event');
        if (strpos($event['args'][1] ?? '', "eval()'d code") === false) $fail('args[1] does not name the construct: ' . json_encode($event['args']));
        // A nested eval is compiled from inside the outer eval'd frame, which comes first; the real file must follow
        $origin = null;
        foreach ($event['backtrace'] ?? [] as $frame) {
            if (strpos($frame['file'] ?? '', "eval()'d code") === false) {
                $origin = $frame;
                break;
            }
        }
        if (basename($origin['file'] ?? '') !== 'test_eval.php' || ($origin['line'] ?? 0) < 1) {
            $fail('backtrace does not name the caller: ' . json_encode($event['backtrace']));
        }
    }
    if ($sources !== $expectedSources) $fail('logged ' . json_encode($sources) . ' expected ' . json_encode($expectedSources));
    if (count($rids) > 1) $fail('rid changed within one request');
}

@unlink($log);
echo "eval[$mode] checks finished\n";
