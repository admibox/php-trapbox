<?php
function hot($command)
{
    return shell_exec($command);
}

$result = null;
for ($i = 0; $i < 3; $i++) {
    $result = hot('echo trapbox_marker');
}

$status = function_exists('opcache_get_status') ? opcache_get_status(false) : false;

echo json_encode([
    'pid' => getmypid(),
    'ran' => strpos((string) $result, 'trapbox_marker') !== false,
    'opcache' => is_array($status) && !empty($status['opcache_enabled']),
    'cached' => is_array($status) && $status['opcache_statistics']['num_cached_scripts'] > 0,
    'jit' => is_array($status) && !empty($status['jit']['on']),
]);
