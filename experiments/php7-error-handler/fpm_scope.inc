class FpmScopeBase {
    public $privateCalls = 0;
    public $protectedCalls = 0;
    private function privateHandler() { $this->privateCalls++; return true; }
    protected function protectedHandler() { $this->protectedCalls++; return true; }
    public function installPrivate() { set_error_handler([$this, 'privateHandler']); }
    public function ownScope() {
        $this->installPrivate();
        @file_get_contents('/nonexistent-trapbox/private_fpm.php');
        restore_error_handler();
    }
}
class FpmScopeChild extends FpmScopeBase {
    public function childScope() {
        set_error_handler([$this, 'protectedHandler']);
        @file_get_contents('/nonexistent-trapbox/protected_fpm.php');
        restore_error_handler();
    }
}
class FpmScopeMagic {
    public $calls = 0;
    public function __call($name, $args) { $this->calls++; return false; }
}
$scopeProbe = new FpmScopeChild();
$scopeProbe->ownScope();
$scopeProbe->childScope();
$scopeProbe->installPrivate();
@file_get_contents('/nonexistent-trapbox/rejected_fpm.php');
$rejectedError = error_get_last();
restore_error_handler();
$magicProbe = new FpmScopeMagic();
set_error_handler([$magicProbe, 'missing']);
@file_get_contents('/nonexistent-trapbox/magic_fpm.php');
restore_error_handler();
$scopeChecks = $scopeProbe->privateCalls === 1 && $scopeProbe->protectedCalls === 1
    && $magicProbe->calls === 1 && strpos($rejectedError['message'] ?? '', '/rejected_fpm.php') !== false;
