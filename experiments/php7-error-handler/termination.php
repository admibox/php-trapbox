<?php
// Exercise bailout while the proxy owns argument references. Compare process
// status and shutdown/destructor effects with native PHP in a separate process.
class TerminationProbe {
    public function __destruct() { echo "DESTRUCT\n"; }
}
register_shutdown_function(function () { echo "SHUTDOWN\n"; });
$object = new TerminationProbe();
$mode = $argv[1];
set_error_handler(function () use ($mode, $object) {
    echo "HANDLER\n";
    if ($mode === 'exit') exit(17);
    trapbox_undefined_from_handler();
});
@file_get_contents('/nonexistent-trapbox/termination.php');
echo "UNREACHABLE\n";
