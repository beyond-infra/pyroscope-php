<?php
/* Swoole prefork repro. Master loads ext (push thread spawns in Master).
 * Manager forks Worker. Question: does pthread_atfork fire in the Worker?
 * If yes → Worker gets a push thread → periodic pushes from Worker pid.
 * Worker generates traffic via timer (no external requests needed). */
function inner(){ return 1; }
function outer(){ for($i=0;$i<300;$i++) inner(); }
function burn(){ $e=hrtime(true)+5000000; $x=0; while(hrtime(true)<$e) $x++; }

$serv = new Swoole\Http\Server('0.0.0.0', 9501, SWOOLE_PROCESS);
$serv->set(['worker_num' => 1, 'daemonize' => false, 'log_file' => '/dev/null']);

$serv->on('WorkerStart', function($serv, $wid) {
    fwrite(STDERR, "[WorkerStart] pid=".getmypid()." wid=$wid time=".date('H:i:s')."\n");
    Swoole\Timer::tick(50, function() { burn(); });
});
$serv->on('WorkerError', function($serv, $wid, $wpid, $code, $sig) {
    fwrite(STDERR, "[WorkerError] wid=$wid pid=$wpid exit_code=$code signal=$sig\n");
});
$serv->on('request', function($req, $resp) { $resp->end('ok'); });

fwrite(STDERR, "[Master start] pid=".getmypid()." time=".date('H:i:s')."\n");
$serv->start();
