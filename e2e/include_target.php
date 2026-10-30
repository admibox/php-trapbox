<?php
$path = $argv[2];
try {
    switch ($argv[1]) {
        case 'include': $value = include $path; break;
        case 'include_once': $value = include_once $path; break;
        case 'require': $value = require $path; break;
        case 'require_once': $value = require_once $path; break;
        default: exit(2);
    }
    echo 'RESULT:', json_encode($value), "\n";
} catch (Error $error) {
    echo "CAUGHT:Error\n";
}
echo "AFTER\n";
