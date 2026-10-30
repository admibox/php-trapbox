<?php
// Cost of the path that does NOT match any filter. Compare runs with and without the extension.

$dir = sys_get_temp_dir();
$plain = "$dir/trapbox-bench.txt";
file_put_contents($plain, str_repeat('x', 1024));

$measure = function ($label, $iterations, $callback) {
    $best = INF;
    for ($round = 0; $round < 5; $round++) {
        $start = hrtime(true);
        for ($i = 0; $i < $iterations; $i++) $callback();
        $best = min($best, hrtime(true) - $start);
    }
    printf("%-22s %8.0f ns/call\n", $label, $best / $iterations);
};

$sink = fopen('/dev/null', 'w');
$measure('fwrite', 300000, function () use ($sink) { fwrite($sink, 'x'); });
$measure('file_get_contents', 30000, function () use ($plain) { file_get_contents($plain); });
$measure('fopen+fclose', 30000, function () use ($plain) { fclose(fopen($plain, 'r')); });
$measure('file_put_contents', 30000, function () use ($plain) { file_put_contents($plain, 'x'); });
unlink($plain);
