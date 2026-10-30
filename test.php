<?php
// Main test runner: includes all test_*.php files.
// Each test prints "OK: ..." on success or "FAIL: ..." on failure.
// Exit code 0 = no failures found in output.

$dir = __DIR__;
$tests = glob("$dir/test_*.php");
$output = '';

foreach ($tests as $test) {
    ob_start();
    include $test;
    $output .= ob_get_clean();
}

echo $output;

if (strpos($output, 'FAIL') !== false) {
    exit(1);
}
