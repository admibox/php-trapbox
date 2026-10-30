<?php
// PHP 7 itself leaks during these bailout cases with USE_ZEND_ALLOC=0. Do not
// suppress those reports. Reject allocations abandoned by proxy dispatch,
// including proxy objects: their abandoned engine references are now released.
$report = file_get_contents($argv[1]);
$allocators = ['malloc', 'calloc', 'realloc', '__zend_malloc', '_emalloc', '_ecalloc', '_safe_emalloc'];
foreach (preg_split('/\n==\d+==\s*\n/', $report) as $record) {
    if (!preg_match('/(?:definitely|indirectly|possibly) lost/', $record)) continue;
    preg_match_all('/(?:at|by) 0x[0-9A-F]+: ([^ (]+)/i', $record, $matches);
    foreach ($matches[1] as $function) {
        if (in_array($function, $allocators, true)) continue;
        if (strpos($function, 'trapbox_') === 0 || strpos($function, 'zim_Trapbox') === 0) {
            fwrite(STDERR, "FAIL: proxy-owned allocation abandoned on bailout: $function\n$record\n");
            exit(1);
        }
        break;
    }
}
echo "OK: no abandoned proxy allocation, including engine-held wrappers\n";
