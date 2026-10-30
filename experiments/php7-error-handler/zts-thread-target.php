<?php
$tag = 'thread-' . $TRAPBOX_THREAD . '-request-' . $TRAPBOX_ITERATION;
$mode = (int) $TRAPBOX_ITERATION % 4;
$old = set_error_handler(null);
restore_error_handler();
$result = ['tag' => $tag, 'mode' => $mode, 'clean' => $old === null && error_get_last() === null,
    'handler' => null, 'after' => false, 'caught' => false, 'zts' => PHP_ZTS, 'pid' => getmypid()];
register_shutdown_function(function () use (&$result, $tag) {
    file_put_contents('/ext/thread-results/' . $tag . '.json', json_encode($result));
});
$result['eval'] = eval('return "' . $tag . '";');
$result['include'] = include 'data://text/plain,<?php return "' . $tag . '";';
set_error_handler(function () use ($tag, $mode, &$result) {
    usleep(500);
    $result['handler'] = $tag;
    if ($mode === 1) throw new RuntimeException($tag);
    if ($mode === 2) exit(17);
    if ($mode === 3) trapbox_undefined_from_thread_handler();
    return false;
});
try {
    @file_get_contents('/nonexistent-trapbox/' . $tag . '.php');
} catch (RuntimeException $error) {
    $result['caught'] = $error->getMessage() === $tag;
}
$result['after'] = true;
