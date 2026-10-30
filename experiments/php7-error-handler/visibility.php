<?php
// Compare the actual dispatch, fallback error and late static binding with
// native PHP. A handler registered in a class must not retain access outside it.
$log = '/tmp/trapbox-proxy-events.jsonl';
@unlink($log);
$received = [];
$expectedEvents = [];
$fallback = [];
function expectWarning($name) { $GLOBALS['expectedEvents'][] = $name; }
function emitOutside($name) {
    expectWarning($name);
    @file_get_contents('/nonexistent-trapbox/' . $name . '.php');
}
function recordFallback($name) {
    $error = error_get_last();
    if (strpos($error['message'] ?? '', '/' . $name . '.php') === false) {
        echo "FAIL: native fallback did not report $name\n";
        exit(1);
    }
    $GLOBALS['fallback'][] = [$error['type'], $error['message'], basename($error['file']), $error['line']];
}
class AccessBase {
    private function privateHandler() { $GLOBALS['received'][] = 'private'; return true; }
    protected function protectedHandler() { $GLOBALS['received'][] = 'protected'; return true; }
    protected static function staticHandler() { $GLOBALS['received'][] = 'static:' . static::class; return true; }
    public function install($method) { return set_error_handler([$this, $method]); }
    public function emit($name) {
        expectWarning($name);
        @file_get_contents('/nonexistent-trapbox/' . $name . '.php');
    }
    public function privateClosure() { return Closure::fromCallable([$this, 'privateHandler']); }
    public static function relative($callback, $name) {
        set_error_handler($callback);
        expectWarning($name);
        @file_get_contents('/nonexistent-trapbox/' . $name . '.php');
        restore_error_handler();
    }
}
class AccessChild extends AccessBase {
    public function emitChild($name) {
        expectWarning($name);
        @file_get_contents('/nonexistent-trapbox/' . $name . '.php');
    }
    public static function relativeParent() {
        set_error_handler('parent::staticHandler');
        expectWarning('relative_parent');
        @file_get_contents('/nonexistent-trapbox/relative_parent.php');
        restore_error_handler();
    }
}
class AccessForeign {
    public function emit() {
        expectWarning('private_foreign');
        @file_get_contents('/nonexistent-trapbox/private_foreign.php');
    }
}
$object = new AccessChild();
$object->install('privateHandler');
$object->emit('private_inside');
emitOutside('private_outside');
recordFallback('private_outside');
(new AccessForeign())->emit();
recordFallback('private_foreign');
$object->emit('private_inside_again');
restore_error_handler();

$object->install('protectedHandler');
$object->emitChild('protected_child');
emitOutside('protected_outside');
recordFallback('protected_outside');
restore_error_handler();

AccessChild::relative('self::staticHandler', 'relative_self');
AccessChild::relative('static::staticHandler', 'relative_static');
AccessChild::relativeParent();
set_error_handler($object->privateClosure());
emitOutside('bound_closure');
restore_error_handler();

class AccessMagic {
    private function hidden() { $GLOBALS['received'][] = 'hidden'; return true; }
    public function install() { set_error_handler([$this, 'hidden']); }
    public function emit() {
        expectWarning('magic_inside');
        @file_get_contents('/nonexistent-trapbox/magic_inside.php');
    }
    public function __call($name, $args) { $GLOBALS['received'][] = 'magic:' . $name . ':' . count($args); return false; }
    public static function __callStatic($name, $args) { $GLOBALS['received'][] = 'magic_static:' . static::class . ':' . $name . ':' . count($args); return true; }
}
class AccessMagicChild extends AccessMagic {}
$magic = new AccessMagic();
$magic->install();
$magic->emit();
emitOutside('magic_outside'); // Must select __call instead of gaining private access.
recordFallback('magic_outside');
restore_error_handler();
set_error_handler([$magic, 'missing']);
emitOutside('magic_missing');
recordFallback('magic_missing');
restore_error_handler();
set_error_handler(['AccessMagicChild', 'missing']);
emitOutside('magic_static');
restore_error_handler();

// Plain function names remain valid callables.
function namedHandler() { $GLOBALS['received'][] = 'named'; return true; }
set_error_handler('namedHandler');
emitOutside('named_function');
restore_error_handler();

$expected = ['private', 'private', 'protected', 'static:AccessChild', 'static:AccessChild',
    'static:AccessChild', 'private', 'hidden', 'magic:hidden:5', 'magic:missing:5',
    'magic_static:AccessMagicChild:missing:5', 'named'];
if ($received !== $expected) { echo 'FAIL: dispatch: ', json_encode($received), "\n"; exit(1); }
if (class_exists('TrapboxErrorProxy', false)) {
    $names = [];
    foreach (file($log, FILE_IGNORE_NEW_LINES) as $line) {
        $event = json_decode($line, true);
        if (!is_array($event)) { echo "FAIL: invalid event\n"; exit(1); }
        preg_match('~/nonexistent-trapbox/([a-z_]+)\.php~', $event['args'][0] ?? '', $match);
        $names[] = $match[1] ?? 'unexpected';
    }
    if ($names !== $expectedEvents) { echo 'FAIL: visibility events: ', json_encode($names), "\n"; exit(1); }
}
echo 'RESULT:', json_encode(['received' => $received, 'fallback' => $fallback]), "\n";
