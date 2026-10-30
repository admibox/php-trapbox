<?php
// Internal FastCGI fixture; never install this in the public document root.
require '/var/www/app/bootstrap.php';
$handler = [new \Magento\Framework\App\ErrorHandler(), 'handler'];
set_error_handler($handler);
$results = [];
try {
    file_get_contents('/nonexistent-trapbox/magento-handler.php');
    $results['exception'] = null;
} catch (\Exception $error) {
    $results['exception'] = [get_class($error), $error->getMessage()];
}
$results['suppressed'] = @file_get_contents('/nonexistent-trapbox/magento-handler.php');
$previous = set_error_handler(static function () { return false; });
$results['identity'] = $previous === $handler;
$results['fallback'] = file_get_contents('/nonexistent-trapbox/magento-handler.php');
restore_error_handler();
$previous = set_error_handler(null);
$results['restored'] = $previous === $handler;
restore_error_handler();
header('Content-Type: application/json');
echo json_encode($results);
