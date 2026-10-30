<?php
// Executes a serialized operation sequence. All decisions come from the input,
// so a failing sequence can be replayed and reduced without a random generator.
$operations = json_decode(file_get_contents($argv[1]), true);
$trace = [];
$expectedEvents = [];
$callbacks = [];
$saved = [];
$depth = 0;
function identify($callback) {
    if ($callback === null) return null;
    foreach ($GLOBALS['callbacks'] as $id => $known) {
        if ($callback === $known) return $id;
    }
    return 'unexpected:' . gettype($callback);
}
function emitWarning($name = 'repeat', $suppressed = true) {
    $GLOBALS['expectedEvents'][] = $name;
    $GLOBALS['trace'][] = ['emit', $name, $suppressed];
    $path = '/nonexistent-trapbox/' . $name . '.php';
    // Repeated calls intentionally use exactly the same warning and location.
    try {
        $result = $suppressed ? @file_get_contents($path) : file_get_contents($path);
        $GLOBALS['trace'][] = ['result', $result];
    } catch (RuntimeException $error) {
        $GLOBALS['trace'][] = ['caught', $error->getMessage()];
    }
}
function makeHandler($id, $behavior) {
    return function ($type, $message, $file, $line) use ($id, $behavior) {
        $GLOBALS['trace'][] = ['callback', $id, $type, $message, basename($file), $line, error_reporting()];
        $GLOBALS['depth']++;
        try {
            if ($GLOBALS['depth'] > 3) return false;
            switch ($behavior) {
                case 'true': return true;
                case 'null': return null;
                case 'false': return false;
                case 'throw': throw new RuntimeException('handler:' . $id);
                case 'nested': emitWarning(); return false;
                case 'replace':
                    $previous = set_error_handler($GLOBALS['callbacks']['leaf'], E_WARNING);
                    $GLOBALS['trace'][] = ['replace_previous', identify($previous)];
                    emitWarning();
                    return false;
                case 'restore':
                    $GLOBALS['trace'][] = ['restore_inside', restore_error_handler()];
                    emitWarning();
                    return false;
                case 'disable':
                    $GLOBALS['trace'][] = ['disable_inside', identify(set_error_handler(null))];
                    emitWarning();
                    return false;
            }
        } finally {
            $GLOBALS['depth']--;
        }
    };
}
$callbacks['leaf'] = makeHandler('leaf', 'true');
foreach (['true', 'null', 'false', 'throw', 'nested', 'replace', 'restore', 'disable'] as $behavior) {
    $callbacks[$behavior] = makeHandler($behavior, $behavior);
}
foreach ($operations as $index => $operation) {
    $trace[] = ['operation', $index, $operation];
    try {
        switch ($operation[0]) {
            case 'set':
                $previous = set_error_handler($callbacks[$operation[1]], $operation[2]);
                $saved[] = $previous;
                $trace[] = ['previous', identify($previous)];
                break;
            case 'restore': $trace[] = ['restore', restore_error_handler()]; break;
            case 'disable':
                $previous = set_error_handler(null);
                $saved[] = $previous;
                $trace[] = ['previous', identify($previous)];
                break;
            case 'emit': emitWarning($operation[1], $operation[2]); break;
            case 'invalid': $trace[] = ['invalid', @set_error_handler('missing_sequence_callback')]; break;
            case 'reuse':
                if ($saved) {
                    $callback = $saved[$operation[1] % count($saved)];
                    $trace[] = ['reuse', identify($callback), identify(set_error_handler($callback, E_WARNING))];
                }
                break;
            case 'manual':
                if ($saved) {
                    $callback = $saved[$operation[1] % count($saved)];
                    if ($callback !== null) {
                        $trace[] = ['manual', identify($callback), $callback(E_WARNING, 'manual', __FILE__, __LINE__)];
                    }
                }
                break;
            case 'reporting': $trace[] = ['reporting', error_reporting($operation[1])]; break;
            case 'clear': error_clear_last(); break;
            case 'gc': gc_collect_cycles(); break;
        }
    } catch (RuntimeException $error) {
        $trace[] = ['operation_exception', $error->getMessage()];
    }
    $last = error_get_last();
    if ($last) $last['file'] = basename($last['file']);
    $trace[] = ['last_error', $last];
}
echo json_encode(['trace' => $trace, 'events' => $expectedEvents]), "\n";
