<?php
// Verify the existing settings alone control callback wrapping and observation.
$active = (bool) ini_get('trapbox.enabled') && (ini_get('trapbox.errors') ?: 'off') !== 'off';
$log = ini_get('trapbox.log_path');
@unlink($log);
$calls = 0;
$handler = function () use (&$calls) { $calls++; return true; };
set_error_handler($handler);
file_get_contents('/nonexistent-trapbox/activation.php');
$previous = set_error_handler(null);
restore_error_handler();
restore_error_handler();
$events = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];
$wrapped = class_exists('TrapboxErrorProxy', false);
if ($calls !== 1 || $previous !== $handler || count($events) !== ($active ? 1 : 0)
    || $wrapped !== ($active && PHP_VERSION_ID < 80000)) {
    throw new RuntimeException('Error observation activation or callback identity changed');
}
echo 'OK: existing settings: ' . ($active ? 'active' : 'inactive') . "\n";
