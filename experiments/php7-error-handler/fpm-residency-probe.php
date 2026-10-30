<?php
// Read-only health request after a batch, before sampling idle worker memory.
$previous = set_error_handler(null);
restore_error_handler();
$status = opcache_get_status(false);
header('Content-Type: application/json');
echo json_encode([
    'pid' => getmypid(),
    'clean' => $previous === null && error_get_last() === null,
    'zend_allocator' => getenv('USE_ZEND_ALLOC'),
    'extension' => extension_loaded('trapbox'),
    'opcache' => $status['opcache_enabled'],
    'cached' => opcache_is_script_cached(__DIR__ . '/fpm-memory-target.php'),
    'heap_used' => memory_get_usage(false),
    'heap_allocated' => memory_get_usage(true),
    'heap_peak' => memory_get_peak_usage(true),
]);
