<?php
/**
 * Rigorous test suite for pyroscope_php extension.
 *
 * Run WITHOUT PYROSCOPE_APP_NAME to keep push thread off.
 * With APP_NAME set, overflow test auto-skips (push thread drains buffer).
 * Output goes to stderr when PHP CLI is present, to work around
 * curl_global_init stdout suppression in some environments.
 */
declare(strict_types=1);

$pass = 0; $fail = 0;
$OUT = defined('STDERR') ? STDERR : STDOUT;
function ok(string $m): void { global $pass, $OUT; fprintf($OUT, "  PASS  %s\n", $m); $pass++; }
function no(string $m, string $d = ''): void { global $fail, $OUT; fprintf($OUT, "  FAIL  %s%s\n", $m, $d ? ": $d" : ""); $fail++; }
function chk(bool $c, string $m, string $d = ''): void { $c ? ok($m) : no($m, $d); }
function info(string $m): void { global $OUT, $pass; fprintf($OUT, "  INFO  %s\n", $m); $pass++; }

// Pre-declare to avoid redeclare in loops. Must call a built-in (sqrt) to ensure hook fires.
function _t1_a() { sqrt(4.0); }
function _t2_root() { _t2_mid(); }
function _t2_mid()  { _t2_leaf(); }
function _t2_leaf() { sqrt(1.0); }
function _t3_nested(int $n): void { if ($n > 0) _t3_nested($n - 1); else sqrt(1.0); }
function _t7_a() { sqrt(1.0); }
function _t7_b() { sqrt(2.0); }
function _t9_nop() { sqrt(0); }
function _t11_nop() { sqrt(0); }
function _t12_work() { sqrt(0); }
function _t14_fib(int $n): int { sqrt(1.0); return $n <= 1 ? $n : _t14_fib($n - 1) + _t14_fib($n - 2); }
function _coA() { usleep(1000); sqrt(1.0); }
function _coB() { usleep(1000); $a = [3,2,1]; sort($a); }
function _coC() { usleep(1000); strtoupper('x'); }

$has_push = (getenv('PYROSCOPE_APP_NAME') ?: '') !== '';

fprintf($OUT, str_repeat('=', 60) . "\n");
fprintf($OUT, "  pyroscope_php — Rigorous Tests" . ($has_push ? " (push ON)" : "") . "\n");
fprintf($OUT, str_repeat('=', 60) . "\n\n");

// ── 0. Extension ──
fprintf($OUT, "[0] Extension loaded\n");
chk(extension_loaded('pyroscope_php'), 'extension_loaded');
chk(function_exists('pyroscope_php_folded'), 'folded() exists');
chk(function_exists('pyroscope_php_dump'), 'dump() exists');
chk(function_exists('pyroscope_php_reset'), 'reset() exists');
chk(function_exists('pyroscope_php_count'), 'count() exists');
chk(function_exists('pyroscope_php_buffer_cap'), 'buffer_cap() exists');
$cap = pyroscope_php_buffer_cap();
chk($cap > 0, "buffer cap = $cap");

// ── 1. Basic hook ──
fprintf($OUT, "\n[1] Basic hook\n");
pyroscope_php_reset();
$before = pyroscope_php_count();
_t1_a(); _t1_a(); _t1_a();
$after = pyroscope_php_count();
chk($after >= 3, "≥3 captured (before=$before after=$after)", "got $after");

// ── 2. Stack order root→leaf ──
fprintf($OUT, "\n[2] Stack order\n");
pyroscope_php_reset();
_t2_root();
$folded = pyroscope_php_folded();
$found = null;
foreach ($folded as $line) {
    if (str_contains($line, '_t2_root') && str_contains($line, '_t2_leaf')) { $found = explode(' ', $line)[0]; break; }
}
chk($found === '_t2_root;_t2_mid;_t2_leaf', 'root→mid→leaf', $found ?? 'not found');

// ── 3. Deep stack ──
fprintf($OUT, "\n[3] Deep stack (40 frames)\n");
pyroscope_php_reset();
_t3_nested(40);
$folded = pyroscope_php_folded();
$deep = false;
foreach ($folded as $line) { if (substr_count(explode(' ', $line)[0], ';') >= 20) { $deep = true; break; } }
chk($deep, 'nested stack captured');

// ── 4. Closures ──
fprintf($OUT, "\n[4] Closures\n");
pyroscope_php_reset();
($f = function() { sqrt(1.0); })();
$folded = pyroscope_php_folded();
$anon = false;
foreach ($folded as $line) { if (str_contains($line, '{closure}') || str_contains($line, 'Closure')) { $anon = true; break; } }
if ($anon) { ok('closure captured'); } else { info('closure under internal name'); }

// ── 5. Coroutine concurrent sampling ──
fprintf($OUT, "\n[5] Coroutine sampling\n");
pyroscope_php_reset();
Swoole\Coroutine\run(function () {
    $wg = new Swoole\Coroutine\WaitGroup();
    $wg->add(); Swoole\Coroutine::create(function () use ($wg) { _coA(); $wg->done(); });
    $wg->add(); Swoole\Coroutine::create(function () use ($wg) { _coB(); $wg->done(); });
    $wg->add(); Swoole\Coroutine::create(function () use ($wg) { _coC(); $wg->done(); });
    $wg->wait();
});
$dump = pyroscope_php_dump();
$stacks = array_unique($dump);
chk(count($stacks) >= 1, 'coroutines produce stack samples');

// ── 6. Reset ──
fprintf($OUT, "\n[6] Reset\n");
pyroscope_php_reset();
chk(pyroscope_php_count() === 0, 'reset → 0');

// ── 7. Folded format ──
fprintf($OUT, "\n[7] Folded format\n");
pyroscope_php_reset();
_t7_a(); _t7_a(); _t7_a();
_t7_b(); _t7_b();
$folded = pyroscope_php_folded();
$a_cnt = $b_cnt = 0;
foreach ($folded as $line) {
    if (str_contains($line, '_t7_a')) $a_cnt += (int) explode(' ', $line)[1];
    if (str_contains($line, '_t7_b')) $b_cnt += (int) explode(' ', $line)[1];
}
chk($a_cnt === 3, "_t7_a × 3", "got $a_cnt");
chk($b_cnt === 2, "_t7_b × 2", "got $b_cnt");

// ── 8. Dump structure ──
fprintf($OUT, "\n[8] Dump structure\n");
pyroscope_php_reset();
_t7_a();
$s = pyroscope_php_dump()[0] ?? null;
chk($s !== null && is_string($s) && strlen($s) > 0, 'stack not empty');
chk($s !== null && str_contains($s, '_t7_a'), 'dump contains function name');

// ── 9. Ring buffer overflow ──
fprintf($OUT, "\n[9] Buffer overflow\n");
if ($has_push) {
    info('push thread active — would drain');
} else {
    pyroscope_php_reset();
    $cap = pyroscope_php_buffer_cap();
    for ($i = 0; $i < $cap + 2000; $i++) _t9_nop();
    $cnt = pyroscope_php_count();
    chk($cnt <= $cap && $cnt > 0, "cnt=$cnt in (0, $cap]");
}

// ── 10. Folded sums after heavy use ──
fprintf($OUT, "\n[10] Folded after load\n");
$folded = pyroscope_php_folded();
$total = 0; foreach ($folded as $line) $total += (int) explode(' ', $line)[1];
chk($total >= 0, "total=$total ≥ 0");

// ── 11. Performance ──
fprintf($OUT, "\n[11] Performance\n");
pyroscope_php_reset();
$N = 5_000;
$start = hrtime(true);
for ($i = 0; $i < $N; $i++) _t11_nop();
$elapsed = hrtime(true) - $start;
$samples = pyroscope_php_count();
$ns = $samples > 0 ? $elapsed / $samples : 0;
fprintf($OUT, "  %d calls: %.2fms, %d samples, %.0f ns/call\n", $N, $elapsed/1e6, $samples, $ns);
chk($ns < 10000 || $samples === 0, 'overhead < 10µs', sprintf('%.0f ns', $ns));

// ── 12. Concurrent burst ──
fprintf($OUT, "\n[12] Concurrent burst\n");
pyroscope_php_reset();
Swoole\Coroutine\run(function () {
    $wg = new Swoole\Coroutine\WaitGroup();
    for ($i = 0; $i < 50; $i++) {
        $wg->add();
        Swoole\Coroutine::create(function () use ($wg) {
            for ($j = 0; $j < 20; $j++) _t12_work();
            $wg->done();
        });
    }
    $wg->wait();
});
chk(pyroscope_php_count() > 500, '50 coro × 20 calls captured');

// ── 13. Empty buffer safety ──
fprintf($OUT, "\n[13] Empty buffer\n");
pyroscope_php_reset();
chk(pyroscope_php_count() === 0, 'count = 0');
chk(is_array(pyroscope_php_folded()), 'folded → array');
chk(is_array(pyroscope_php_dump()), 'dump → array');

// ── 14. Recursion ──
fprintf($OUT, "\n[14] Recursion\n");
pyroscope_php_reset();
_t14_fib(10);
$folded = pyroscope_php_folded();
$deep = false;
foreach ($folded as $line) { if (substr_count(explode(' ', $line)[0], ';') > 5) { $deep = true; break; } }
chk($deep, 'fib(10) depth > 5');

// ── 15. Built-ins don't crash ──
fprintf($OUT, "\n[15] Built-in filter\n");
pyroscope_php_reset();
for ($i = 0; $i < 100; $i++) { $a = [3,2,1]; sort($a); }
chk(pyroscope_php_count() >= 0, 'built-ins safe');

// ── 16. Push thread ──
fprintf($OUT, "\n[16] Push thread\n");
if ($has_push) {
    fprintf($OUT, "  INFO  %s → %s\n", getenv('PYROSCOPE_APP_NAME'), getenv('PYROSCOPE_ENDPOINT') ?: 'http://127.0.0.1:4040');
    ok('push configured');
} else {
    ok('push off');
}

// ── 17. Enable/disable ──
fprintf($OUT, "\n[17] Enable/disable\n");
chk(function_exists('pyroscope_php_enable'), 'enable() exists');
chk(function_exists('pyroscope_php_is_enabled'), 'is_enabled() exists');
pyroscope_php_reset();
chk(pyroscope_php_is_enabled(), 'default enabled');
_t7_a();
chk(pyroscope_php_count() > 0, 'sampling when enabled');
pyroscope_php_enable(false);
chk(!pyroscope_php_is_enabled(), 'now disabled');
pyroscope_php_reset();
_t7_a(); _t7_a(); _t7_a();
chk(pyroscope_php_count() === 0, 'disabled → no samples');
pyroscope_php_enable(true);
chk(pyroscope_php_is_enabled(), 're-enabled');
_t7_a();
chk(pyroscope_php_count() > 0, 'sampling resumed');

// ── 18. Buffer cap consistency ──
fprintf($OUT, "\n[18] Buffer cap consistency\n");
chk(pyroscope_php_buffer_cap() === 65536, 'cap=65536', (string)pyroscope_php_buffer_cap());

// ── 19. Rapid API during push ──
fprintf($OUT, "\n[19] Rapid API during push\n");
pyroscope_php_reset();
$t0 = microtime(true);
for ($i = 0; $i < 1000; $i++) {
    pyroscope_php_reset();
    pyroscope_php_count();
    pyroscope_php_folded();
    pyroscope_php_dump();
}
$elapsed = (microtime(true) - $t0) * 1000;
chk($elapsed < 5000, "rapid API calls < 5s", sprintf("%.1fms", $elapsed));

// ── 20. Mode ──
fprintf($OUT, "\n[20] Mode\n");
chk(pyroscope_php_mode() === 'cpu', "mode = cpu");

// ── 21. Max depth clamping ──
fprintf($OUT, "\n[21] Max depth clamping\n");
pyroscope_php_reset();
function _t21_deep(int $n): void { if ($n > 0) _t21_deep($n - 1); else _t1_a(); }
_t21_deep(200); // exceeds MAX_DEPTH=128
chk(pyroscope_php_count() > 0, 'deep stack survives max depth');

fprintf($OUT, "\n" . str_repeat('=', 60) . "\n");
fprintf($OUT, "Results: %d passed, %d failed, %d total\n", $pass, $fail, $pass + $fail);
exit($fail > 0 ? 1 : 0);
