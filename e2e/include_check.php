<?php
// Run without trapbox; each construct executes in a separate child process.
$ext = dirname(__DIR__) . '/modules/trapbox.so';
$target = __DIR__ . '/include_target.php';
$log = '/tmp/trapbox-include-check.jsonl';
$pharPath = '/tmp/trapbox-include-check.phar';
@unlink($pharPath);
$phar = new Phar($pharPath);
$phar['x.php'] = '<?php echo "PAYLOAD\n"; return 42;';
unset($phar);
$failed = false;
foreach (['off', 'bypass', 'block', 'trap'] as $mode) {
    foreach (['include', 'include_once', 'require', 'require_once'] as $construct) {
        foreach ([0, 1] as $allow) {
            @unlink($log);
            $path = "phar://$pharPath/x.php";
            // Also test disabled URL includes before the native stream opening.
            if (strpos($construct, '_once') === false) {
                $path = 'data:text/plain,<?php echo "PAYLOAD\n"; return 42;';
            }
            $command = escapeshellarg(PHP_BINARY) . ' -dextension=' . escapeshellarg($ext)
                . ' -dtrapbox.log_path=' . escapeshellarg($log)
                . " -dtrapbox.include=$mode -dtrapbox.errors=off -dallow_url_include=$allow "
                . escapeshellarg($target) . ' ' . escapeshellarg($construct) . ' ' . escapeshellarg($path);
            $output = [];
            exec($command . ' 2>&1', $output, $code);
            $text = implode("\n", $output);
            $blocked = $mode === 'block';
            $trapped = $mode === 'trap';
            $nativeFailure = !$blocked && !$trapped && !$allow && strpos($construct, '_once') === false;
            $requiredFailure = ($blocked || $nativeFailure) && strpos($construct, 'require') === 0;
            $ran = !$blocked && !$trapped && !$nativeFailure;
            $expectedCode = $requiredFailure && PHP_VERSION_ID < 80000 ? 255 : 0;
            $valid = $code === $expectedCode && (preg_match('/^PAYLOAD$/m', $text) === 1) === $ran;
            if ($requiredFailure) {
                $valid = $valid && strpos($text, 'RESULT:') === false;
                $valid = $valid && (PHP_VERSION_ID < 80000
                    ? strpos($text, 'AFTER') === false
                    : strpos($text, 'CAUGHT:Error') !== false);
            } else {
                $expected = $ran ? '42' : ($trapped ? '1' : 'false');
                $valid = $valid && strpos($text, 'RESULT:' . $expected) !== false;
            }
            $lines = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];
            $valid = $valid && count($lines) === ($mode === 'off' ? 0 : 1);
            if ($lines) {
                $event = json_decode($lines[0], true);
                $valid = $valid && ($event['category'] ?? '') === 'include'
                    && ($event['mode'] ?? '') === $mode && ($event['args'][0] ?? '') === $path;
            }
            if (!$valid) {
                echo "FAIL: $construct mode=$mode allow_url_include=$allow exit=$code\n$text\n";
                $failed = true;
            }
        }
    }
}
@unlink($log);
@unlink($pharPath);
if ($failed) exit(1);
echo "OK: include/require behavior and URL policy\n";
