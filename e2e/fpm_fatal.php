<?php
// The next request must still be served by this worker after a fatal bailout.
echo "FATAL_START\n";
if (ini_get('trapbox.include') === 'block') {
    require 'phar:///tmp/trapbox-fpm.phar/x.php';
} else {
    eval('trapbox_fpm_undefined_function();');
}
echo "FATAL_AFTER\n";
