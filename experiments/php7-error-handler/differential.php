<?php
// Usage: php -n differential.php [seed-count] [operations] [first-seed]
// Failure artifacts contain the complete sequence, reduced reproducer, process
// output, event log and a replay command. Reduction compares native PHP again.
$count = isset($argv[1]) ? (int) $argv[1] : 100;
$length = isset($argv[2]) ? (int) $argv[2] : 100;
$first = isset($argv[3]) ? (int) $argv[3] : 1;
if (($argv[1] ?? '') !== '--replay' && ($count < 1 || $length < 1 || $first < 1)) {
    fwrite(STDERR, "Seed count, operation count and first seed must be positive.\n");
    exit(2);
}
$directory = __DIR__ . '/differential-results';
if (!is_dir($directory)) mkdir($directory, 0777, true);
$log = '/tmp/trapbox-sequence.jsonl';
if (!is_dir(__DIR__ . '/memory-cases')) mkdir(__DIR__ . '/memory-cases');
$flags = ['-n', '-ddisplay_errors=0', '-dlog_errors=0', '-derror_reporting=32767'];
$prototypeFlags = array_merge($flags, ['-dextension=' . __DIR__ . '/modules/trapbox.so',
    '-dtrapbox.log_path=' . $log, '-dtrapbox.exec=off', '-dtrapbox.eval=off',
    '-dtrapbox.include=off', '-dtrapbox.file_write=off', '-dtrapbox.secret_read=off',
    '-dtrapbox.unserialize=off', '-dtrapbox.errors=bypass']);
function executeSequence($path, $prototype) {
    global $flags, $prototypeFlags, $log;
    @unlink($log);
    $arguments = array_merge([PHP_BINARY], $prototype ? $prototypeFlags : $flags, [__DIR__ . '/sequence.php', $path]);
    $command = implode(' ', array_map('escapeshellarg', $arguments));
    $process = proc_open($command, [0 => ['file', '/dev/null', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
    $stdout = stream_get_contents($pipes[1]);
    $stderr = stream_get_contents($pipes[2]);
    fclose($pipes[1]); fclose($pipes[2]);
    $status = proc_close($process);
    return ['status' => $status, 'stdout' => $stdout, 'stderr' => $stderr,
        'log' => is_file($log) ? file_get_contents($log) : ''];
}
function evaluateSequence($operations, $path) {
    file_put_contents($path, json_encode($operations, JSON_PRETTY_PRINT) . "\n");
    $native = executeSequence($path, false);
    $prototype = executeSequence($path, true);
    $reason = null;
    $data = json_decode($native['stdout'], true);
    if ($native['status'] !== 0 || !is_array($data)) $reason = 'native process failed';
    elseif ($prototype['status'] !== $native['status'] || $prototype['stdout'] !== $native['stdout']
        || $prototype['stderr'] !== $native['stderr']) $reason = 'application behavior differs';
    else {
        $observed = [];
        foreach (explode("\n", trim($prototype['log'])) as $line) {
            if ($line === '') continue;
            $event = json_decode($line, true);
            if (!is_array($event) || ($event['category'] ?? '') !== 'error'
                || ($event['function'] ?? '') !== 'E_WARNING'
                || !preg_match('~/nonexistent-trapbox/([a-z_]+)\.php~', $event['args'][0] ?? '', $match)) {
                $reason = 'invalid or unexpected event'; break;
            }
            $observed[] = $match[1];
        }
        if ($reason === null && $observed !== $data['events']) $reason = 'missing, duplicate or reordered events';
    }
    return ['reason' => $reason, 'native' => $native, 'prototype' => $prototype];
}
function generateSequence($seed, $length) {
    mt_srand($seed);
    $behaviors = ['true', 'null', 'false', 'throw', 'nested', 'replace', 'restore', 'disable'];
    $masks = [E_ALL, E_WARNING, E_NOTICE, 0];
    $operations = [];
    for ($i = 0; $i < $length; $i++) {
        $choice = mt_rand(0, 14);
        if ($choice < 4) $operations[] = ['set', $behaviors[mt_rand(0, 7)], $masks[mt_rand(0, 3)]];
        elseif ($choice < 7) $operations[] = ['emit', mt_rand(0, 3) ? 'repeat' : 'other', (bool) mt_rand(0, 1)];
        elseif ($choice === 7) $operations[] = ['restore'];
        elseif ($choice === 8) $operations[] = ['disable'];
        elseif ($choice === 9) $operations[] = ['invalid'];
        elseif ($choice === 10) $operations[] = ['reuse', mt_rand(0, 100)];
        elseif ($choice === 11) $operations[] = ['manual', mt_rand(0, 100)];
        elseif ($choice === 12) $operations[] = ['reporting', $masks[mt_rand(0, 3)]];
        elseif ($choice === 13) $operations[] = ['clear'];
        else $operations[] = ['gc'];
    }
    return $operations;
}
if (($argv[1] ?? '') === '--replay') {
    $operations = json_decode(file_get_contents($argv[2]), true);
    $result = evaluateSequence($operations, $directory . '/replay.json');
    file_put_contents($directory . '/replay-result.json', json_encode($result, JSON_PRETTY_PRINT));
    echo $result['reason'] === null ? "OK: replay\n" : "FAIL: " . $result['reason'] . "\n";
    exit($result['reason'] === null ? 0 : 1);
}
foreach (glob(__DIR__ . '/regressions/*.json') as $fixture) {
    $result = evaluateSequence(json_decode(file_get_contents($fixture), true), $directory . '/case.json');
    if ($result['reason'] !== null) {
        file_put_contents($directory . '/regression-failure.json', json_encode($result, JSON_PRETTY_PRINT));
        echo 'FAIL: regression ', basename($fixture), ': ', $result['reason'], "\n"; exit(1);
    }
}
for ($seed = $first; $seed < $first + $count; $seed++) {
    $operations = generateSequence($seed, $length);
    if (in_array($seed, [1, 2, 3, 7, 11, 19, 31, 53], true)) {
        file_put_contents(__DIR__ . '/memory-cases/seed-' . $seed . '.json', json_encode($operations));
    }
    $result = evaluateSequence($operations, $directory . '/case.json');
    if ($result['reason'] === null) continue;
    $failure = $directory . '/seed-' . $seed;
    mkdir($failure);
    file_put_contents($failure . '/original.json', json_encode($operations, JSON_PRETTY_PRINT));
    file_put_contents($failure . '/original-result.json', json_encode($result, JSON_PRETTY_PRINT));
    $originalReason = $result['reason'];
    // Delta debugging, followed by single-operation deletion. Keep the same
    // failure class, so reduction cannot turn a log failure into a PHP crash.
    for ($chunk = max(1, intdiv(count($operations), 2)); $chunk >= 1; $chunk = intdiv($chunk, 2)) {
        for ($offset = 0; $offset < count($operations);) {
            $candidate = array_merge(array_slice($operations, 0, $offset), array_slice($operations, $offset + $chunk));
            $reduced = evaluateSequence($candidate, $directory . '/case.json');
            if ($reduced['reason'] === $originalReason) $operations = $candidate;
            else $offset += $chunk;
        }
    }
    $reduced = evaluateSequence($operations, $failure . '/minimal.json');
    file_put_contents($failure . '/minimal-result.json', json_encode($reduced, JSON_PRETTY_PRINT));
    echo "FAIL: seed=$seed: $originalReason; minimized to ", count($operations), " operations; artifacts: $failure\n";
    exit(1);
}
echo "OK: differential seeds $first..", $first + $count - 1, ", $length operations each\n";
