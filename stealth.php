<?php
trapbox_intercept('get_loaded_extensions', function ($original) {
    return array_filter($original(), function($item) { return $item !== 'trapbox'; });
});

trapbox_intercept('extension_loaded', function ($original, $extension = null) {
    return $extension === 'trapbox' ? false : $original($extension);
});

trapbox_intercept('phpinfo', function ($original, $flags = INFO_ALL) {
    ob_start();
    $original($flags);

    echo implode(PHP_EOL, array_filter(explode(PHP_EOL, ob_get_clean()), function($line) {
        return strpos($line, 'trapbox') === false;
    }));
});