<?php
function hot($command, &$context, ...$extra)
{
    $evaluated = eval('return "trapbox_eval_marker";');
    return [shell_exec($command), $evaluated];
}

$context = ['recursive' => null];
$context['recursive'] = &$context;
$result = null;
for ($i = 0; $i < 3; $i++) {
    $result = hot('echo trapbox_marker', $context, new stdClass(), str_repeat('x', 80));
}

$included = include 'phar:///tmp/trapbox-fpm.phar/x.php';

$read = @file_get_contents('/tmp/trapbox-fpm-env.php');
$wrote = @file_put_contents('/tmp/trapbox-fpm-out.php', 'x');

class FpmUnserializeProbe
{
    public function __wakeup() { $GLOBALS['wakeups']++; }
}
$GLOBALS['wakeups'] = 0;
$payload = serialize(new FpmUnserializeProbe());
$restored = unserialize($payload);
$restricted = unserialize($payload, ['allowed_classes' => false]);
$scalars = unserialize('a:1:{i:0;i:42;}');

// These are code paths, but not secret paths, so read interception cannot hide
// the warnings even when secret_read is blocked or trapped.
$missing = @file_get_contents('/nonexistent-trapbox/unhandled.php');
$handled = 0;
set_error_handler(function () use (&$handled) { $handled++; return true; });
$handledMissing = file_get_contents('/nonexistent-trapbox/handled.php');
restore_error_handler();
// Ordinary application errors must not generate forensic events.
@file_get_contents('/nonexistent-trapbox/plain.txt');

$status = function_exists('opcache_get_status') ? opcache_get_status(false) : false;

echo json_encode([
    'pid' => getmypid(),
    'restored' => $restored instanceof FpmUnserializeProbe,
    'wakeups' => $GLOBALS['wakeups'],
    'restricted' => $restricted instanceof __PHP_Incomplete_Class,
    'scalars' => $scalars,
    'handled' => $handled,
    'missing' => $missing,
    'handled_missing' => $handledMissing,
    'ran' => strpos((string) $result[0], 'trapbox_marker') !== false,
    'evaluated' => $result[1] === 'trapbox_eval_marker',
    'included' => $included === 'trapbox_phar_marker',
    'read' => $read === 'secret',
    'wrote' => $wrote === 1,
    'phar_cached' => function_exists('opcache_is_script_cached') && opcache_is_script_cached('phar:///tmp/trapbox-fpm.phar/x.php'),
    'opcache' => is_array($status) && !empty($status['opcache_enabled']),
    'cached' => is_array($status) && $status['opcache_statistics']['num_cached_scripts'] > 0,
    'jit' => is_array($status) && !empty($status['jit']['on']),
]);
