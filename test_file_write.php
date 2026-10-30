<?php
// Runs under whichever trapbox.file_write mode the command line sets.
// Requires -dtrapbox.log_path pointing at a writable file.

// The ini parser turns an unquoted `off` into an empty string
$mode = ini_get('trapbox.file_write') ?: 'off';
$log = ini_get('trapbox.log_path');
$fail = function ($msg) use ($mode) { echo "FAIL: file_write[$mode]: $msg\n"; };

$dir = sys_get_temp_dir() . '/trapbox-write-' . getmypid();
mkdir($dir);
file_put_contents("$dir/source.txt", 'source');
// Through the shell: this write would itself be blocked or trapped
shell_exec('printf existing > ' . escapeshellarg("$dir/existing.php"));
$stringable = new class($dir) {
    private $dir;
    public function __construct($dir) { $this->dir = $dir; }
    public function __toString() { return "$this->dir/object.php"; }
};

@unlink($log);

$real = $mode === 'off' || $mode === 'bypass';

$put = file_put_contents("$dir/a.php", 'payload-a');
$upper = file_put_contents("$dir/UPPER.PHP", 'payload-b');
$object = file_put_contents($stringable, 'payload-c');
$htaccess = file_put_contents("$dir/.htaccess", 'payload-d');
$handle = fopen("$dir/b.phtml", 'w');
$written = $handle ? fwrite($handle, 'payload-e') : null;
$fputs = $handle ? fputs($handle, 'payload-f') : null;
if ($handle) fclose($handle);
$renamed = rename("$dir/source.txt", "$dir/moved.php");
$copied = copy("$dir/existing.php", "$dir/copied.inc");
$linked = symlink("$dir/existing.php", "$dir/linked.php");
$hard = link("$dir/existing.php", "$dir/hard.php");
$chmod = chmod("$dir/existing.php", 0644);
$made = mkdir("$dir/dir.php");

if (file_put_contents("$dir/plain.txt", 'plain') !== 5) $fail('a plain write was affected');
$plainHandle = fopen("$dir/plain2.txt", 'w');
if (!$plainHandle || fwrite($plainHandle, 'plain') !== 5) $fail('a plain stream write was affected');
fclose($plainHandle);
$reader = fopen("$dir/existing.php", 'r');
if (!$reader || fread($reader, 8) !== 'existing') $fail('reading a code file was affected');
fclose($reader);

$created = ["a.php", "UPPER.PHP", "object.php", ".htaccess", "b.phtml", "moved.php", "copied.inc", "linked.php", "hard.php", "dir.php"];
clearstatcache();

if ($real) {
    foreach ($created as $name) {
        if (!file_exists("$dir/$name")) $fail("$name was not created");
    }
    if (file_get_contents("$dir/b.phtml") !== 'payload-epayload-f') $fail('stream content was lost');
    if ($put !== 9 || $written !== 9 || $renamed !== true) $fail('wrong return value');
} else {
    foreach ($created as $name) {
        if (file_exists("$dir/$name")) $fail("$name was created");
    }
    if ($mode === 'block') {
        foreach ([$put, $upper, $object, $htaccess, $handle, $renamed, $copied, $linked, $hard, $chmod, $made] as $value) {
            if ($value !== false) $fail('expected false, got ' . var_export($value, true));
        }
    } else {
        if ($put !== 9 || $upper !== 9 || $object !== 9 || $htaccess !== 9) $fail('fake write length is wrong');
        if ($written !== 9 || $fputs !== 9) $fail('fake stream write length is wrong');
        foreach ([$renamed, $copied, $linked, $hard, $chmod, $made] as $value) {
            if ($value !== true) $fail('expected true, got ' . var_export($value, true));
        }
    }
}

$lines = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];

if ($mode === 'off') {
    if ($lines !== []) $fail('off mode wrote events');
} else {
    $functions = [];
    $payloads = [];
    foreach ($lines as $line) {
        $event = json_decode($line, true);
        if (!is_array($event)) {
            $fail("invalid JSON: $line");
            continue;
        }
        $functions[] = $event['function'];
        $payloads[$event['function']][] = $event['args'][1] ?? null;
        if ($event['category'] !== 'file_write' || $event['mode'] !== $mode) $fail('wrong category or mode');
        if (basename($event['backtrace'][0]['file'] ?? '') !== 'test_file_write.php') $fail('backtrace does not name the caller');
    }
    $expected = ['file_put_contents', 'file_put_contents', 'file_put_contents', 'file_put_contents', 'fopen'];
    if ($mode !== 'block') {
        $expected[] = 'fwrite';
        $expected[] = 'fputs';
    }
    $expected = array_merge($expected, ['rename', 'copy', 'symlink', 'link', 'chmod', 'mkdir']);
    if ($functions !== $expected) $fail('logged ' . json_encode($functions));
    if (($payloads['file_put_contents'][0] ?? null) !== 'payload-a') $fail('file_put_contents payload not logged');
    if ($mode !== 'block' && ($payloads['fwrite'][0] ?? null) !== 'payload-e') $fail('fwrite payload not logged');
    if (($payloads['rename'][0] ?? null) !== "$dir/moved.php") $fail('rename destination not logged');
}

@unlink($log);
foreach (scandir($dir) as $name) {
    if ($name === '.' || $name === '..') continue;
    is_dir("$dir/$name") && !is_link("$dir/$name") ? rmdir("$dir/$name") : unlink("$dir/$name");
}
rmdir($dir);
echo "file_write[$mode] checks finished\n";
