#!/bin/bash
make clean && phpize && ./configure && make && php -dextension=modules/trapbox.so test.php