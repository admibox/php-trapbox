<?php
// Runs under whichever trapbox.include mode the command line sets.
// Requires -dtrapbox.log_path, -dallow_url_include=1 and -dphar.readonly=0.

// The ini parser turns an unquoted `off` into an empty string
$mode = ini_get('trapbox.include') ?: 'off';
$log = ini_get('trapbox.log_path');
$fail = function ($msg) use ($mode) { echo "FAIL: include[$mode]: $msg\n"; };

$local = tempnam(sys_get_temp_dir(), 'tb_inc');
file_put_contents($local, '<?php return "local";');
$pharPath = sys_get_temp_dir() . '/trapbox-test-' . getmypid() . '.phar';
$phar = new Phar($pharPath);
$phar['x.php'] = '<?php $GLOBALS["trapbox_included"]++; return 88;';
unset($phar);

@unlink($log);

$GLOBALS['trapbox_included'] = 0;
$dataUrl = 'data:text/plain;base64,' . base64_encode('<?php $GLOBALS["trapbox_included"]++; return 77;');
$pharUrl = "phar://$pharPath/x.php";

$localResult = include $local;
if (filter_var($local, FILTER_VALIDATE_URL)) $fail('fixture path looks like a URL');
$dataResult = include $dataUrl;
$pharResult = include $pharUrl;

if ($localResult !== 'local') $fail('a plain include was affected: ' . var_export($localResult, true));

$real = $mode === 'off' || $mode === 'bypass';
if ($real) {
    if ($dataResult !== 77) $fail('data: include returned ' . var_export($dataResult, true));
    if ($pharResult !== 88) $fail('phar: include returned ' . var_export($pharResult, true));
    if ($GLOBALS['trapbox_included'] !== 2) $fail('included code did not run');
} else {
    $expected = $mode === 'block' ? false : 1;
    if ($dataResult !== $expected) $fail('data: include returned ' . var_export($dataResult, true));
    if ($pharResult !== $expected) $fail('phar: include returned ' . var_export($pharResult, true));
    if ($GLOBALS['trapbox_included'] !== 0) $fail('included code ran');
}

$lines = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];

if ($mode === 'off') {
    if ($lines !== []) $fail('off mode wrote events');
} else {
    $paths = [];
    foreach ($lines as $line) {
        $event = json_decode($line, true);
        if (!is_array($event)) {
            $fail("invalid JSON: $line");
            continue;
        }
        $paths[] = $event['args'][0] ?? null;
        if ($event['category'] !== 'include' || $event['mode'] !== $mode) $fail('wrong category or mode');
        $frame = $event['backtrace'][0] ?? [];
        if (basename($frame['file'] ?? '') !== 'test_include.php' || ($frame['line'] ?? 0) < 1) {
            $fail('backtrace does not name the caller: ' . json_encode($event['backtrace']));
        }
    }
    if ($paths !== [$dataUrl, $pharUrl]) $fail('logged ' . json_encode($paths));
}

@unlink($log);
@unlink($local);
@unlink($pharPath);
echo "include[$mode] checks finished\n";
