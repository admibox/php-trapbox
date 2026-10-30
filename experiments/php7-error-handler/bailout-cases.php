<?php
// Exercise abandoned handler references at different shutdown/unwind stages.
$case = $argv[1];
register_shutdown_function(function () { echo "SHUTDOWN\n"; });
class BailoutDestructor {
    public function __destruct() {
        echo "DESTRUCTOR\n";
        @file_get_contents('/nonexistent-trapbox/destructor.php');
    }
}
set_error_handler(function () use ($case) {
    echo "OUTER\n";
    if ($case === 'nested') {
        set_error_handler(function () {
            echo "INNER\n";
            exit(17);
        });
        @file_get_contents('/nonexistent-trapbox/nested.php');
    }
    if ($case === 'replace') {
        set_error_handler(function () { echo "REPLACEMENT\n"; return true; });
    }
    exit(17);
});
if ($case === 'shutdown') {
    register_shutdown_function(function () {
        @file_get_contents('/nonexistent-trapbox/shutdown.php');
    });
} elseif ($case === 'destructor') {
    $object = new BailoutDestructor();
} else {
    @file_get_contents('/nonexistent-trapbox/outer.php');
}
