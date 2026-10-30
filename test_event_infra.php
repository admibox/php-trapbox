<?php
// Phase 1: verify ini settings, correlation ID, and event infrastructure

$ok = true;

// Check ini defaults are registered
$checks = [
    'trapbox.enabled' => '1',
    'trapbox.exec' => 'block',
    'trapbox.eval' => 'bypass',
    'trapbox.file_write_extensions' => 'php,phtml,inc,htaccess,user.ini,js,html',
    'trapbox.max_arg_size' => '65536',
    'trapbox.max_body_size' => '1048576',
];

foreach ($checks as $key => $expected) {
    $actual = ini_get($key);
    if ($actual === false) {
        echo "FAIL: ini $key not registered\n";
        $ok = false;
    } elseif ($actual !== $expected) {
        echo "FAIL: ini $key = '$actual', expected '$expected'\n";
        $ok = false;
    }
}

if ($ok) {
    echo "OK: event infrastructure ini settings registered\n";
}
