<?php
// Phase 0: verify the skeleton extension loads and does nothing

$ok = true;

// Extension is loaded
if (!extension_loaded('trapbox')) {
    echo "FAIL: extension not loaded\n";
    $ok = false;
}

// Old API is gone
foreach (['trapbox_intercept', 'trapbox_seal', 'trapbox_set_exit_handler'] as $fn) {
    if (function_exists($fn)) {
        echo "FAIL: $fn still exists\n";
        $ok = false;
    }
}

// Version is reported
$version = phpversion('trapbox');
if ($version !== '2.0.0-dev') {
    echo "FAIL: version is '$version', expected '2.0.0-dev'\n";
    $ok = false;
}

// Basic functions still work (nothing is intercepted)
$tmp = tempnam(sys_get_temp_dir(), 'tb_');
file_put_contents($tmp, 'test');
if (file_get_contents($tmp) !== 'test') {
    echo "FAIL: file_put_contents/file_get_contents broken\n";
    $ok = false;
}
unlink($tmp);

if ($ok) {
    echo "OK: skeleton loads clean\n";
}
