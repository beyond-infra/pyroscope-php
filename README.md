# pyroscope-php

[![CI](https://github.com/beyond-infra/pyroscope-php/actions/workflows/ci.yml/badge.svg)](https://github.com/beyond-infra/pyroscope-php/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

PHP 函数级 CPU profiler。一个 `.so` 扩展，零 PHP 代码改动，后台线程自动推送 pprof 到 Pyroscope。

## 快速开始

```bash
# 1. 安装
cd extension
phpize && ./configure --enable-pyroscope-php && make -j$(nproc) && make install
echo "extension=pyroscope_php.so" > $(php -r 'echo PHP_CONFIG_FILE_SCAN_DIR;')/pyroscope_php.ini

# 2. 验证
php -m | grep pyroscope_php

# 3. 运行
export PYROSCOPE_APP_NAME=my-app
export PYROSCOPE_ENDPOINT=http://pyroscope:4040
php server.php
```

打开 Pyroscope 选 `my-app` 即见 CPU 火焰图。

依赖：`libcurl-dev`、`phpize`。

## 环境变量

| 变量 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `PYROSCOPE_APP_NAME` | 是 | — | Pyroscope app name |
| `PYROSCOPE_ENDPOINT` | 否 | `http://127.0.0.1:4040` | Pyroscope 地址 |
| `PYROSCOPE_INTERVAL` | 否 | `10` | 推送间隔，秒（1–3600） |
| `PYROSCOPE_SAMPLING_INTERVAL_US` | 否 | `10000` | SIGVTALRM 采样间隔，微秒（1000–1000000） |
| `PYROSCOPE_WALL` | 否 | `0` | 开启 wall-clock 采样（补同步阻塞盲区），`1` 启用。仅 Linux |
| `PYROSCOPE_WALL_INTERVAL_US` | 否 | `10000` | wall 采样间隔，微秒（1000–1000000） |

## API

#### CPU
| 函数 | 说明 |
|------|------|
| `pyroscope_php_folded()` | `string[]` — `"root;mid;leaf count"` |
| `pyroscope_php_dump()` | `string[]` — 原始栈字符串 |
| `pyroscope_php_reset()` | — |
| `pyroscope_php_count()` | `int` — 当前采集数 |
| `pyroscope_php_buffer_cap()` | `int` — 容量（65536） |

#### 运行时控制
| 函数 | 说明 |
|------|------|
| `pyroscope_php_enable(bool $enabled)` | `bool` — 开关采样，无需重启 PHP |
| `pyroscope_php_is_enabled()` | `bool` |

#### Mode
| 函数 | 说明 |
|------|------|
| `pyroscope_php_mode()` | `"cpu"` |

## 原理

```
PHP 主线程              SIGVTALRM 每 10ms            信号 handler
业务代码执行中  ────────────────────────▶  walk_stack EG(current_execute_data)
                                         写 g_ring[head++]  (lock-free SPSC, 满→丢)
                                                    │
                                                    ▼  push 线程每 N 秒 drain
                                         push thread: merge + pprof + POST
                                         /ingest?name=app&format=pprof
```

1. `PHP_MINIT` 装 `SIGVTALRM` handler + `setitimer(ITIMER_VIRTUAL, 10ms)`，起后台推送线程
2. 每 10ms 信号触发，沿 `EG(current_execute_data)->prev_execute_data` 回溯构建 `root;mid;leaf` 栈，写入 lock-free SPSC ring（满则丢）
3. 推送线程每 N 秒 drain ring，合并相同栈（value = 样本数 × 周期 ns），编码 pprof protobuf，HTTP POST

`ITIMER_VIRTUAL`/`SIGVTALRM` 不被 PHP（`SIGPROF`/max_execution_time）或 Swoole（`SIGALRM`/timer）占用，只计用户态 CPU。FPM/Swoole prefork 后 worker 经 `pthread_atfork` 重装采样器 + 重启推送线程。

### wall-clock 模式（可选，`PYROSCOPE_WALL=1`）

CPU 模式只计用户态 CPU，**同步阻塞调用**（`sleep()`、同步 curl、阻塞 mysql）在 Swoole 协程里会卡死整个 worker 却不耗 CPU——火焰图上一片空白，正是盲区。开启 wall 模式后，第二路采样按真实时间触发（`timer_create` + `CLOCK_REALTIME` + 实时信号 `SIGRTMIN+8`），进程阻塞在 syscall 时信号仍会打断并采集栈。在 Swoole 协作式单线程下语义干净：正常的异步等待（`Coroutine::sleep`、协程化 curl）经 epoll 挂起、worker 跑别的，信号打不到它们；只有真把 worker 卡住的同步阻塞会反复落在同一栈上，火焰图一条粗柱子直接定位。

两路独立 ring + 独立推送线程，推到**同一个 app 名**，Pyroscope 按 `sample_type`（`cpu` vs `wall`）区分——一个应用两个指标，UI 切换查看。代价：开启后每个 worker 多 256MB 常驻 ring；仅 Linux（macOS/BSD 实时信号语义弱）。

## 指标

| 指标 | 值 |
|------|-----|
| 采样开销 | 每 10ms 一次信号 handler（µs 级），无 per-call 开销 |
| buffer 常驻 | CPU 256MB(65536×4096B) + wall 32MB(8192×4096B) + sigaltstack |
| 编码依赖 | 无（100 行 wire encoder） |
| 测试覆盖 | PHP 22 组 + C pprof 编码 + valgrind + 浸泡测试 + wall 集成 |
## 兼容

- PHP 8.0+
- Linux x86_64 / ARM64
- Pyroscope v2.x
- 不依赖 Swoole

## 开发

```bash
make build      # 构建 .so
make test       # PHP 集成测试
make test-pprof # pprof 编码单元测试
make docker-demo # Docker 完整环境
```

CI 矩阵：PHP 集成 / pprof 编码 / valgrind / 浸泡测试 / push 集成 / wall 集成。

## License

MIT
