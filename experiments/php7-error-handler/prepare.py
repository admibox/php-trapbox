from pathlib import Path
import sys

source = Path(sys.argv[1])
destination = Path(sys.argv[2])
destination.mkdir(parents=True, exist_ok=True)
for name in ('trapbox.c', 'php_trapbox.h', 'config.m4'):
    (destination / name).write_bytes((source / name).read_bytes())
(destination / 'e2e').mkdir(exist_ok=True)
for name in ('fpm_check.php', 'fpm_target.php', 'fpm_fatal.php'):
    content = (source / 'e2e' / name).read_text()
    if name == 'fpm_check.php':
        content = content.replace("'error' => 2", "'error' => 6")
        content = content.replace("|| !$data['restricted']", "|| !$data['scope_checks'] || !$data['restricted']")
    if name == 'fpm_target.php':
        scope_probe = (source / 'experiments/php7-error-handler/fpm_scope.inc').read_text()
        content = content.replace("$status = function_exists", scope_probe + "\n$status = function_exists")
        content = content.replace("'pid' => getmypid(),", "'pid' => getmypid(), 'scope_checks' => $scopeChecks,")
        content += "\n// Leave an active wrapped closure for engine request cleanup.\nset_error_handler(static function () { return true; });\n"
    (destination / 'e2e' / name).write_text(content)
