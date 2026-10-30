<?php
// The same application runs with and without the prototype. Only observations
// are expected to change; callback results, identity and lifetime must agree.
$log = '/tmp/trapbox-proxy-events.jsonl';
@unlink($log);
$results = [];
$proxyFrames = false;
function warning($name) { return @file_get_contents('/nonexistent-trapbox/' . $name . '.php'); }
function check($condition, $name) {
    if (!$condition) { echo "FAIL: $name\n"; exit(1); }
}
foreach (['true', 'null', 'false', 'throw'] as $kind) {
    $calls = 0;
    $handler = function ($type, $message, $file, $line, $context) use ($kind, &$calls, &$proxyFrames) {
        $calls++;
        check($type === E_WARNING && is_string($message) && is_string($file) && is_int($line) && is_array($context), 'callback arguments');
        foreach (debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS) as $frame) {
            if (($frame['function'] ?? '') === 'trapbox_error_proxy') $proxyFrames = true;
        }
        if ($kind === 'throw') throw new RuntimeException('original exception');
        if ($kind === 'true') return true;
        if ($kind === 'false') return false;
    };
    check(set_error_handler($handler) === null, 'empty previous handler');
    $caught = null;
    try { $value = warning($kind); } catch (RuntimeException $error) { $caught = $error->getMessage(); }
    check($calls === 1, 'exactly one callback');
    check($kind === 'throw' ? $caught === 'original exception' : $value === false, 'original result or exception');
    $results[$kind] = [$calls, $caught];
    restore_error_handler();
}

$aCalls = 0;
$bCalls = 0;
$a = function () use (&$aCalls) { $aCalls++; return true; };
$b = function () use (&$bCalls) { $bCalls++; return true; };
check(set_error_handler($a, E_WARNING) === null, 'install A');
check(set_error_handler($b, E_NOTICE) === $a, 'previous callback identity');
warning('mask'); // Native callback handles this warning because B excludes it.
check($aCalls === 0 && $bCalls === 0, 'mask respected');
restore_error_handler();
warning('restored');
check($aCalls === 1, 'restore mask and callback');
check(set_error_handler(null) === $a, 'null returns original');
warning('disabled');
restore_error_handler();
warning('restored_after_null');
check($aCalls === 2, 'restore after null');
$invalid = @set_error_handler('trapbox_callback_does_not_exist');
check($invalid === null, 'invalid callback native result');
warning('after_invalid');
check($aCalls === 4, 'invalid registration warning and subsequent warning use original handler');
restore_error_handler();
$results['stack'] = [$aCalls, $bCalls];

$nestedCalls = 0;
set_error_handler(function () use (&$nestedCalls) {
    $nestedCalls++;
    warning('inner'); // PHP disables the active handler while it is running.
    return false;
});
warning('outer');
check($nestedCalls === 1, 'recursive warning does not call handler again');
restore_error_handler();

$changedCalls = 0;
$newHandler = function () use (&$changedCalls) { $changedCalls++; return true; };
set_error_handler(function () use ($newHandler) {
    check(set_error_handler($newHandler) === null, 'handler temporarily unset by engine');
    warning('changed_inner');
    return false;
});
warning('changed_outer');
warning('changed_after');
check($changedCalls === 2, 'handler installed during error remains active');
restore_error_handler();
restore_error_handler();
$results['nested'] = [$nestedCalls, $changedCalls];

$contextValue = 'before';
set_error_handler(function ($type, $message, $file, $line, &$context) {
    $context['contextValue'] = 'after';
    return true;
});
warning('context_reference');
restore_error_handler();
$results['context'] = $contextValue;

class ProxyProbeHandler {
    public static $destroyed = 0;
    public $cycle;
    public $calls = 0;
    public function __invoke() { $this->calls++; return true; }
    public function handle() { $this->calls++; return true; }
    public function __destruct() { self::$destroyed++; }
}
$object = new ProxyProbeHandler();
set_error_handler($object);
warning('invokable');
$method = [$object, 'handle'];
check(set_error_handler($method) === $object, 'invokable identity');
warning('method');
check(set_error_handler($a) === $method, 'method identity');
restore_error_handler();
restore_error_handler();
restore_error_handler();
check($object->calls === 2, 'object callbacks');
unset($method, $object);
check(ProxyProbeHandler::$destroyed === 1, 'callback released after restore');
$object = new ProxyProbeHandler();
$object->cycle = $object;
set_error_handler($object);
unset($object);
restore_error_handler();
gc_collect_cycles();
check(ProxyProbeHandler::$destroyed === 2, 'cyclic callback released');
$results['destroyed'] = ProxyProbeHandler::$destroyed;

class ProxyPrivateHandler {
    public $calls = 0;
    private function handle() { $this->calls++; return true; }
    public function run() {
        set_error_handler([$this, 'handle']);
        @file_get_contents('/nonexistent-trapbox/private_method.php');
        restore_error_handler();
    }
}
$private = new ProxyPrivateHandler();
$private->run();
check($private->calls === 1, 'private callback preserves native invocation scope');
$results['private'] = $private->calls;

// Repeated registration/restoration must not retain request-lifetime wrappers.
$before = memory_get_usage();
for ($i = 0; $i < 10000; $i++) {
    set_error_handler(static function () { return true; });
    restore_error_handler();
}
gc_collect_cycles();
check(memory_get_usage() - $before < 1024 * 1024, 'bounded memory after repeated registration');

$events = is_file($log) ? array_map(function ($line) { return json_decode($line, true); }, file($log, FILE_IGNORE_NEW_LINES)) : [];
$names = [];
foreach ($events as $event) {
    check(is_array($event) && ($event['category'] ?? '') === 'error', 'valid error event');
    preg_match('~/nonexistent-trapbox/([a-z_]+)\.php~', $event['args'][0], $match);
    $names[] = $match[1] ?? 'unknown';
}
if (class_exists('TrapboxErrorProxy', false)) {
    foreach ([new TrapboxErrorProxy(), unserialize('O:17:"TrapboxErrorProxy":0:{}')] as $empty) {
        check(!is_callable($empty), 'empty proxy is not callable');
        check($empty->__invoke() === false, 'direct empty proxy invocation is safe');
        $rejected = false;
        try { Closure::fromCallable($empty); } catch (Throwable $error) { $rejected = true; }
        check($rejected, 'empty proxy cannot expose an internal closure');
    }
    check($names === ['true', 'null', 'false', 'throw', 'mask', 'restored', 'disabled', 'restored_after_null', 'after_invalid', 'outer', 'inner', 'changed_outer', 'changed_inner', 'changed_after', 'context_reference', 'invokable', 'method', 'private_method'], 'one event per warning: ' . json_encode($names));
    check($proxyFrames, 'prototype frame is visible in application backtraces');
}
echo 'RESULT:', json_encode($results), "\n";
echo 'OBSERVATION:', json_encode(['events' => count($events), 'proxy_frame_visible' => $proxyFrames, 'private_callback_calls' => $private->calls]), "\n";
