<?php
// Test: double free in replacement_function when intercepted function
// is called from within an intercepted handler (internal_call_context > 0)

trapbox_intercept('strtolower', function ($original, $str) {
    return $original($str);
});

trapbox_intercept('strtoupper', function ($original, $str) {
    // This nested call to strtolower enters internal_call_context > 0
    // where fci.params = args (same pointer), then both get efree'd → double free
    $lower = strtolower($str);
    return $original($str);
});

// Hammer it to increase chance of exposing the double free
for ($i = 0; $i < 1000; $i++) {
    strtoupper("test string $i");
}

echo "OK: no crash after 1000 iterations\n";
