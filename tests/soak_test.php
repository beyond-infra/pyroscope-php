<?php
/**
 * Soak test: 60-second sustained profiling with concurrent API calls.
 *
 * Run with PYROSCOPE_APP_NAME + PYROSCOPE_ENDPOINT to exercise push thread
 * while main thread continuously samples + queries the buffer API.
 */
declare(strict_types=1);

function soak_work(): void { sqrt(1.0); }

$duration = (int)(getenv('SOAK_SECONDS') ?: 60);
$deadline = time() + $duration;
$iterations = 0;
$api_calls = 0;
$resets = 0;

echo "soak: {$duration}s, endpoint=" . (getenv('PYROSCOPE_ENDPOINT') ?: 'none') . "\n";

while (time() < $deadline) {
    // Burst of work
    for ($i = 0; $i < 5000; $i++) { soak_work(); }
    usleep(100000); // 100ms between bursts

    // Exercise buffer API concurrently with push thread
    pyroscope_php_count();     $api_calls++;
    pyroscope_php_folded();    $api_calls++;
    pyroscope_php_dump();      $api_calls++;

    if ($iterations % 10 === 0) {
        pyroscope_php_reset(); $resets++;
    }
    $iterations++;
}

$final = pyroscope_php_count();
echo "soak: {$iterations} bursts, {$api_calls} API calls, {$resets} resets, final count={$final}\n";
echo "PASS\n";
