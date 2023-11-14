<?php
var_dump(function_exists('trapbox_intercept'));

trapbox_intercept('file_put_contents', function ($original, $filename, $content = null, $flags = null) {
    $ext = pathinfo($filename, PATHINFO_EXTENSION);
    if ($ext === 'php') {
        file_put_contents('./audit.log', "Hack attempt!\n", FILE_APPEND);
        return true;
    }
    return $original($filename, $content, $flags);
});

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

trapbox_seal();

var_dump(function_exists('trapbox_intercept'));

var_dump(file_put_contents('app.php', '<?php echo "This is a hack"'));

print_r(get_loaded_extensions());
var_dump(extension_loaded('trapbox'));

//phpinfo();
