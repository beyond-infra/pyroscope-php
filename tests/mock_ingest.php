<?php
/**
 * Minimal /ingest receiver for the wall-clock integration test.
 *
 * Stores each pprof body to a file tagged by sample_type. The sample_type
 * string ("wall" / "cpu") appears as plaintext in the pprof string_table, so
 * we classify by substring. The test then greps the stored bins for the
 * expected function names (sync_block / cpu_burn).
 *
 * Usage: php tests/mock_ingest.php [port] [outdir]
 */
$port = $argv[1] ?? '4040';
$dir  = $argv[2] ?? __DIR__;
@unlink("$dir/ingest.log");

$srv = @stream_socket_server("tcp://0.0.0.0:$port", $e, $m) or die("bind: $m\n");
echo "mock ingest on :$port\n";
while ($c = @stream_socket_accept($srv)) {
    $req = "";
    while (!feof($c)) {
        $chunk = fread($c, 65536);
        if ($chunk === false || $chunk === "") break;
        $req .= $chunk;
        if (strpos($req, "\r\n\r\n") !== false) break;
    }
    $body = substr($req, strpos($req, "\r\n\r\n") + 4);
    $tag = (strpos($body, "wall") !== false) ? "wall" : "cpu";
    file_put_contents("$dir/ingest.$tag.bin", $body);
    file_put_contents("$dir/ingest.log", "got $tag len=" . strlen($body) . "\n", FILE_APPEND);
    fwrite($c, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    fclose($c);
}
