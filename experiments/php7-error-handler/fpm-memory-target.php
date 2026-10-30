<?php
// Every request starts with a clean handler stack, including after bailout.
$previous = set_error_handler(null);
restore_error_handler();
echo 'PID:' . getmypid() . "\n";
echo 'CLEAN:' . ($previous === null && error_get_last() === null ? 'yes' : 'no') . "\n";
echo 'OPCACHE:' . (opcache_get_status(false)['opcache_enabled'] ? 'yes' : 'no') . "\n";
$mode = $_GET['mode'];
class FpmMemoryObject {
    public function __destruct() { echo "DESTRUCT\n"; }
}
register_shutdown_function(function () { echo "SHUTDOWN\n"; });
$object = new FpmMemoryObject();
set_error_handler(function () use ($object, $mode) {
    echo "HANDLER\n";
    if ($mode === 'exit') exit(17);
    if ($mode === 'fatal') trapbox_undefined_from_fpm_handler();
    if ($mode === 'throw') throw new RuntimeException('caught');
    return false;
});
try {
    @file_get_contents('/nonexistent-trapbox/fpm-memory.php');
} catch (RuntimeException $error) {
    echo "CAUGHT\n";
}
echo "AFTER\n";
