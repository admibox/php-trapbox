<?php

trapbox_intercept('get_loaded_extensions', function ($original) {
    return [];
});

trapbox_intercept('file_put_contents', function ($original, $filename, $content = null, $flags = null) {

    printf("Call to file_put_contents\n");

    print_r(get_loaded_extensions());

    $ext = pathinfo($filename, PATHINFO_EXTENSION);
    if ($ext === 'php') {
        file_put_contents('./audit.log', "Hack attempt!\n", FILE_APPEND);
        return true;
    }
    return $original($filename, $content, $flags);
});

print_r(get_loaded_extensions());
file_put_contents('app.php', '<?php echo "This is a hack"');



