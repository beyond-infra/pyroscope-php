<?php
/**
 * A/B overhead benchmark: measure perf impact of pyroscope_php.
 *
 * Scenarios:
 *   A) Single-tick work — flat call tree, CPU-heavy
 *   B) Deep stack — 20-frame recursion
 *   C) Coroutine burst — N concurrent coroutines
 *   D) HTTP server QPS — real Swoole server, wrk-style load
 *
 * Each scenario runs ONCE. To A/B, run this script twice:
 *   php benchmarks/overhead_bench.php          # baseline (extension loaded or not)
 *   # Compares against previous run stored in /tmp
 */

declare(strict_types=1);

const WARMUP = 3;
const ITER   = 10;

$has_ext = extension_loaded('pyroscope_php');
echo str_repeat('=', 60) . "\n";
printf("  pyroscope_php Overhead Benchmark (%s)\n", $has_ext ? "EXTENSION LOADED" : "NO EXTENSION");
echo str_repeat('=', 60) . "\n\n";

function time_it(string $label, callable $fn, int $warmup = WARMUP, int $iter = ITER): array {
    for ($i = 0; $i < $warmup; $i++) $fn();
    $times = [];
    $start_total = hrtime(true);
    for ($i = 0; $i < $iter; $i++) {
        $t0 = hrtime(true);
        $fn();
        $times[] = (hrtime(true) - $t0) / 1e6; // ms
    }
    $total = (hrtime(true) - $start_total) / 1e6;
    sort($times);
    $n = count($times);
    return [
        'label'  => $label,
        'total'  => $total,
        'avg'    => array_sum($times) / $n,
        'p50'    => $times[(int)($n * 0.5)],
        'p99'    => $times[(int)($n * 0.99)],
        'min'    => $times[0],
        'max'    => $times[$n - 1],
    ];
}

function report(array $r): void {
    printf("  %-30s avg=%7.3fms  p50=%7.3fms  p99=%7.3fms  min=%7.3fms  max=%7.3fms\n",
        $r['label'], $r['avg'], $r['p50'], $r['p99'], $r['min'], $r['max']);
}

// ─────────────────────────────────────────────────
// Scenario A: Shallow work — many flat function calls
// ─────────────────────────────────────────────────
echo "[A] Shallow — 100K flat function calls\n";

function bench_shallow(): void {
    $x = 0.0;
    for ($i = 0; $i < 100_000; $i++) {
        $x += sqrt((float)($i % 1000));
    }
    if ($x < 0) echo "x"; // prevent optimize-away
}

report(time_it('shallow_100k', 'bench_shallow'));

// ─────────────────────────────────────────────────
// Scenario B: Deep stack — 50-frame recursion
// ─────────────────────────────────────────────────
echo "\n[B] Deep — 50-frame recursion × 1000\n";

function deep_leaf(int $n): float {
    if ($n <= 0) return sqrt((float)$n + 1.0);
    return deep_leaf($n - 1) + sqrt((float)$n);
}

function bench_deep(): void {
    $x = 0.0;
    for ($i = 0; $i < 1000; $i++) {
        $x += deep_leaf(50);
    }
    if ($x < 0) echo "x";
}

report(time_it('deep_50_1k', 'bench_deep'));

// ─────────────────────────────────────────────────
// Scenario C: Coroutine burst — 200 coroutines
// ─────────────────────────────────────────────────
echo "\n[C] Coroutine burst — 200 coroutines, 500 calls each\n";

function coro_worker(int $n): void {
    $x = 0.0;
    for ($i = 0; $i < $n; $i++) {
        $x += sqrt((float)($i % 50));
    }
    if ($x < 0) echo "x";
}

function bench_coro_burst(): void {
    \Swoole\Coroutine\run(function () {
        $wg = new \Swoole\Coroutine\WaitGroup();
        for ($i = 0; $i < 200; $i++) {
            $wg->add();
            \Swoole\Coroutine::create(function () use ($wg) {
                coro_worker(500);
                $wg->done();
            });
        }
        $wg->wait();
    });
}

report(time_it('coro_200x500', 'bench_coro_burst'));

// ─────────────────────────────────────────────────
// Scenario D: String manipulation + sort
// ─────────────────────────────────────────────────
echo "\n[D] Mixed — string + sort + hash\n";

function bench_mixed(): void {
    for ($i = 0; $i < 5000; $i++) {
        $s = "the quick brown fox jumps over the lazy dog " . $i;
        $parts = explode(' ', $s);
        sort($parts);
        $j = implode('-', $parts);
        $h = hash('sha256', $j);
        strlen($h);
    }
}

report(time_it('mixed_5k', 'bench_mixed'));

summary:
echo "\n" . str_repeat('=', 60) . "\n";
echo $has_ext ? "  EXTENSION ACTIVE — measure overhead vs baseline\n" : "  BASELINE — no extension\n";
echo str_repeat('=', 60) . "\n";
