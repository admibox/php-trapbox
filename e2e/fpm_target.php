<?php
function hot($command)
{
    $evaluated = eval('return "trapbox_eval_marker";');
    return [shell_exec($command), $evaluated];
}

$result = null;
for ($i = 0; $i < 3; $i++) {
    $result = hot('echo trapbox_marker');
}

$status = function_exists('opcache_get_status') ? opcache_get_status(false) : false;

echo json_encode([
    'pid' => getmypid(),
    'ran' => strpos((string) $result[0], 'trapbox_marker') !== false,
    'evaluated' => $result[1] === 'trapbox_eval_marker',
    'opcache' => is_array($status) && !empty($status['opcache_enabled']),
    'cached' => is_array($status) && $status['opcache_statistics']['num_cached_scripts'] > 0,
    'jit' => is_array($status) && !empty($status['jit']['on']),
]);
