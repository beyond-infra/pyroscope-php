<?php
/**
 * Realistic Swoole HTTP service with coroutine profiling enabled.
 *
 * Architecture: Router → Controller → Service → Repository → Helper
 *
 * Profiling is zero-code: just load the extension + set env vars.
 * The extension samples the PHP stack via a SIGVTALRM timer and pushes to Pyroscope on a background thread.
 *
 * Env vars:
 *   PYROSCOPE_APP_NAME=my-shop-api     (required to enable auto-push)
 *   PYROSCOPE_ENDPOINT=http://pyroscope:4040  (default http://127.0.0.1:4040)
 *   PYROSCOPE_INTERVAL=10              (push interval, seconds, default 10)
 *   PYROSCOPE_SAMPLING_INTERVAL_US=10000  (SIGVTALRM sampling interval, microseconds, default 10000)
 *
 * Endpoints:
 *   GET /health
 *   GET /api/products?page=1&size=20
 *   GET /api/order?id=1
 *   GET /api/dashboard
 *
 * Usage:
 *   PYROSCOPE_APP_NAME=my-shop-api php examples/demo_server.php
 *   curl http://localhost:9501/api/dashboard
 */

use Swoole\Http\Server;
use Swoole\Http\Request;
use Swoole\Http\Response;
use Swoole\Coroutine;

// ──── Layer 4: Helpers ────

function paginateHelper(int $page, int $size): array {
    usleep(50);
    return compact('page', 'size');
}

function encryptPassword(string $pw): string { return hash('sha256', $pw . 'salt'); }
function formatMoney(int $cents): string { return number_format($cents / 100, 2); }

// ──── Layer 3: Repository ────

function fetchProductsRepo(int $page, int $size): array {
    $p = paginateHelper($page, $size);
    Coroutine::sleep(0.002);
    $items = [];
    for ($i = 0; $i < $size; $i++) {
        $items[] = ['id' => $p['page'] * $size + $i, 'name' => "Product-" . ($p['page'] * $size + $i), 'price' => rand(100, 99999)];
    }
    return compact('items') + $p;
}

function fetchOrdersRepo(int $userId, int $page, int $size): array {
    $p = paginateHelper($page, $size);
    Coroutine::sleep(0.003);
    $items = [];
    for ($i = 0; $i < $size; $i++) {
        $items[] = ['id' => rand(1000, 9999), 'total' => rand(1000, 50000), 'status' => ['paid','shipped','done'][rand(0,2)]];
    }
    return compact('items') + $p;
}

function fetchUserRepo(int $userId): array {
    Coroutine::sleep(0.001);
    return ['id' => $userId, 'name' => "User-$userId", 'vip' => $userId % 3 === 0];
}

// ──── Layer 2: Service ────

function listProductsService(int $page, int $size): array {
    $products = fetchProductsRepo($page, $size);
    foreach ($products['items'] as &$item) {
        $item['price_fmt'] = formatMoney($item['price']);
    }
    $scores = [];
    foreach ($products['items'] as $item) {
        $s = 0.0;
        for ($k = 0; $k < 5000; $k++) $s += sqrt($item['id'] + $k) * log($k + 1);
        $scores[] = $s;
    }
    array_multisort($scores, SORT_DESC, $products['items']);
    return $products;
}

function getOrderDetailService(int $orderId): array {
    $orders = fetchOrdersRepo(1, 1, 1);
    $order = $orders['items'][0] ?? null;
    if (!$order) return ['error' => 'not found'];
    $userId = $order['id'] % 100;
    $order['user'] = fetchUserRepo($userId);
    $order['user']['password'] = encryptPassword('hidden');
    $order['tax'] = (int)($order['total'] * 0.13);
    $order['total_fmt'] = formatMoney($order['total'] + $order['tax']);
    return $order;
}

function dashboardStatsService(): array {
    $chan = new Coroutine\Channel(3);
    Coroutine::create(function () use ($chan) {
        $p = fetchProductsRepo(1, 20); $chan->push(['products_count' => count($p['items'])]);
    });
    Coroutine::create(function () use ($chan) {
        $o = fetchOrdersRepo(1, 1, 10); $chan->push(['orders_count' => count($o['items'])]);
    });
    Coroutine::create(function () use ($chan) {
        $u = fetchUserRepo(42); $chan->push(['vip_user' => $u]);
    });
    $stats = [];
    for ($i = 0; $i < 3; $i++) $stats[] = $chan->pop();
    $merged = array_merge(...$stats);
    $x = 0.0;
    for ($i = 0; $i < 300_000; $i++) $x += log($i + 1) / ($i + 2);
    $merged['relevance_score'] = round($x, 4);
    return $merged;
}

// ──── Layer 1: Controller ────

function productsController(Request $req): array {
    return listProductsService(
        (int)($req->get['page'] ?? 1),
        min((int)($req->get['size'] ?? 20), 100)
    );
}

function orderController(Request $req): array {
    return getOrderDetailService((int)($req->get['id'] ?? 1));
}

function dashboardController(): array {
    return dashboardStatsService();
}

// ──── Server ────

$server = new Server('0.0.0.0', 9501);
$server->set(['worker_num' => 1, 'enable_coroutine' => true]);

$server->on('request', function (Request $req, Response $res) {
    $path = $req->server['request_uri'] ?? '/';
    try {
        $data = match ($path) {
            '/api/products' => productsController($req),
            '/api/order'    => orderController($req),
            '/api/dashboard' => dashboardController(),
            '/health'        => ['status' => 'ok', 'ts' => time()],
            default          => ['error' => 'not found'],
        };
        $res->header('Content-Type', 'application/json');
        $res->end(json_encode($data, JSON_UNESCAPED_UNICODE));
    } catch (\Throwable $e) {
        $res->status(500);
        $res->end(json_encode(['error' => $e->getMessage()]));
    }
});

echo "my-shop-api starting on :9501 (profiler enabled)\n";
$server->start();
