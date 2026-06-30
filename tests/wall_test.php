<?php
/**
 * wall-clock sampling integration test.
 *
 * Runs a sync blocking call (usleep — a real syscall) and a CPU-bound loop in
 * a loop, keeping the process alive long enough for the push thread to drain.
 * With PYROSCOPE_WALL=1, the wall sampler interrupts usleep and captures the
 * sync_block frame; the CPU sampler (SIGVTALRM/ITIMER_VIRTUAL) cannot, because
 * usleep runs in kernel mode. The mock receiver classifies the two pprof
 * streams by sample_type; the CI step greps the bins to prove:
 *   - wall bin contains sync_block  (wall sees the blocking call)
 *   - cpu  bin lacks   sync_block  (cpu blind spot confirmed)
 *
 * Usage (against tests/mock_ingest.php on :4040):
 *   PYROSCOPE_APP_NAME=wallt PYROSCOPE_ENDPOINT=http://127.0.0.1:4040 \
 *   PYROSCOPE_INTERVAL=1 PYROSCOPE_WALL=1 php tests/wall_test.php
 */

function sync_block() { usleep(50000); }   // 50ms blocking syscall
function cpu_burn()  { $end = hrtime(true) + 50_000_000; $x = 0; while (hrtime(true) < $end) $x++; }

$end = hrtime(true) + 5_000_000_000;       // stay alive 5s so push thread drains
while (hrtime(true) < $end) { sync_block(); cpu_burn(); }
echo "done\n";
