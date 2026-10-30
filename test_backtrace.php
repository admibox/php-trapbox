<?php
$log = ini_get('trapbox.log_path');
@unlink($log);
function trapbox_trace_probe(&$text, $integer, $boolean, $nil, $array, $object, $resource, $float)
{
    eval('return 1;');
}
$recursive = [];
$recursive[] = &$recursive;
$object = new class {
    public function __toString() { throw new Exception('Logging called user code'); }
};
$resource = fopen('php://memory', 'w+');
$limit = (int) ini_get('trapbox.max_arg_size');
$text = str_repeat('x', $limit) . "\0tail";
trapbox_trace_probe($text, 42, true, null, $recursive, $object, $resource, INF, 'extra');
function trapbox_trace_variadic($first, ...$rest) { eval('return 2;'); }
trapbox_trace_variadic('first', 'second', 'third');
// Exercise truncation at the event level as well as in frames.
eval('/*' . str_repeat('x', $limit) . '*/ return 3;');
function trapbox_trace_unset($value) { unset($value); eval('return 4;'); }
trapbox_trace_unset('removed');
$constructor = new class('building') {
    public function __construct($value) { eval('return 5;'); }
};
function trapbox_trace_generator($value) { eval('return 6;'); yield $value; }
foreach (trapbox_trace_generator('resumed') as $value) {}
$events = array_map(function ($line) { return json_decode($line, true); }, file($log, FILE_IGNORE_NEW_LINES));
$probe = $events[0]['backtrace'][0]['args'] ?? [];
$expected = [str_repeat('x', $limit) . '...[truncated]', 42, true, null,
    ['type' => 'array'], ['type' => 'object'], ['type' => 'resource'],
    ['type' => 'float', 'value' => 'INF'], 'extra'];
if ($probe !== $expected) echo "FAIL: backtrace arguments or truncation\n";
if (($events[1]['backtrace'][0]['args'] ?? []) !== ['first', 'second', 'third']) echo "FAIL: variadic arguments\n";
if (($events[2]['args'][0] ?? '') !== substr('/*' . str_repeat('x', $limit), 0, $limit) . '...[truncated]') echo "FAIL: event truncation\n";
if ($text !== str_repeat('x', $limit) . "\0tail") echo "FAIL: logging mutated reference\n";
if (($events[3]['backtrace'][0]['args'] ?? []) !== [['type' => 'unavailable']]) echo "FAIL: unset argument\n";
if (($events[4]['backtrace'][0]['args'] ?? []) !== ['building']) echo "FAIL: constructor argument\n";
if (($events[5]['backtrace'][0]['args'] ?? []) !== ['resumed']) echo "FAIL: generator argument\n";
fclose($resource);
@unlink($log);
echo "OK: backtrace checks finished\n";
