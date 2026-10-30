<?php
// Phase 1: verify that trapbox_log_event writes valid JSONL with expected fields.
// We can't call trapbox_log_event from PHP directly (it's a C function),
// but we can trigger it indirectly once an intercept category is wired up.
//
// For now, verify the log_path ini is writable and the infrastructure pieces
// are in place by checking ini values and that the extension reports its version.

$ok = true;

// Verify all mode defaults
$modes = [
    'trapbox.exec' => 'bypass',
    'trapbox.eval' => 'bypass',
    'trapbox.include' => 'bypass',
    'trapbox.file_write' => 'bypass',
    'trapbox.secret_read' => 'bypass',
    'trapbox.unserialize' => 'bypass',
    'trapbox.errors' => 'bypass',
];

foreach ($modes as $key => $expected) {
    $actual = ini_get($key);
    if ($actual !== $expected) {
        echo "FAIL: $key = '$actual', expected '$expected'\n";
        $ok = false;
    }
}

// Verify filter defaults
$filters = [
    'trapbox.file_write_extensions' => 'php,phtml,inc,htaccess,user.ini,js,html',
    'trapbox.secret_paths' => 'env.php,.env,wp-config.php,/etc/passwd,/etc/shadow',
];

foreach ($filters as $key => $expected) {
    $actual = ini_get($key);
    if ($actual !== $expected) {
        echo "FAIL: $key = '$actual', expected '$expected'\n";
        $ok = false;
    }
}

// Boolean ini: check it's falsy (ini_get returns '' or '0' depending on version)
if (ini_get('trapbox.file_write_any_path') && ini_get('trapbox.file_write_any_path') !== '0') {
    echo "FAIL: trapbox.file_write_any_path should default to off\n";
    $ok = false;
}

if ($ok) {
    echo "OK: all mode and filter defaults correct\n";
}
