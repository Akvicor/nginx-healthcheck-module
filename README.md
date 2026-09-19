# nginx-healthcheck-module

[中文文档](README-zh_CN.md)

[Blog](https://www.ksyaki.com/archives/nginx-shang-you-jian-kang-jian-cha-cha-jian)

Active upstream health checks for Nginx 1.26+.

This project is maintained by Akvicor and is based on
[yaoweibin/nginx_upstream_check_module](https://github.com/yaoweibin/nginx_upstream_check_module).
The current branch keeps the active health check model, adapts the Nginx patch
for newer upstream internals, and focuses on TCP/UDP checks for both `http` and
`stream` upstreams.

## Features

- Supports Nginx 1.26+ with the included upstream patch.
- Supports active `type=tcp` and `type=udp` checks.
- Supports `http` upstreams and `stream` upstreams.
- Filters unhealthy peers from Nginx upstream load balancing.
- Supports status output in `html`, `csv`, `json`, and `prometheus` formats.
- Reports TCP check delay statistics: last, average, minimum, and maximum in
  milliseconds. UDP delay fields contain numeric zero as a not-applicable value.
- Supports TCP health-check connection reuse with `reuse=on`.

HTTP, FastCGI, MySQL, AJP, and SSL hello layer-7 checks from the original
project are not supported in this maintained version.

## Compatibility

- Target Nginx version: 1.26+.
- The module must be built statically with `--add-module`.
- Dynamic module loading is not supported.
- `stream` health checks require Nginx to be built with `--with-stream`.

The included patch adds active-check peer filtering to the built-in HTTP and
Stream upstream balancers, including round robin, hash, consistent hash,
HTTP ip_hash, least_conn, random, and random two.
The [nginx-upstream-fair](https://github.com/Akvicor/nginx-upstream-fair) module uses
joint static builds to check both primary and backup peers continuously.

## Installation

To install prebuilt packages from the Debian repository, see the
[installation guide on the blog](https://www.ksyaki.com/archives/nginx-shang-you-jian-kang-jian-cha-cha-jian).

```bash
git clone https://github.com/nginx/nginx.git
git clone https://github.com/Akvicor/nginx-healthcheck-module.git

cd nginx
git checkout release-1.26.3
git apply ../nginx-healthcheck-module/nginx_healthcheck_for_nginx_1.26+.patch

./auto/configure --with-stream --add-module=../nginx-healthcheck-module
make
make install
```

Use the same configure options that your Nginx build normally needs. Keep
`--with-stream` if you want `stream {}` health checks.

## Example

```nginx
worker_processes auto;

events {
    worker_connections 1024;
}

http {
    check_shm_size 2m;

    server {
        listen 8080;

        location /status {
            healthcheck_status json;
        }

        location / {
            proxy_pass http://web_backends;
        }
    }

    upstream web_backends {
        server 127.0.0.1:8081;
        server 127.0.0.2:8081;

        check interval=3000 rise=2 fall=5 timeout=1000 default_down=true type=tcp;
    }
}

stream {
    check_shm_size 2m;

    upstream tcp_backends {
        server 127.0.0.1:22;
        server 192.0.2.10:22;

        check interval=3000 rise=2 fall=5 timeout=1000 default_down=true type=tcp reuse=on;
        check_keepalive_requests 10;
    }

    server {
        listen 5222;
        proxy_pass tcp_backends;
    }

    upstream udp_backends {
        server 127.0.0.1:53;
        server 8.8.8.8:53;

        check interval=3000 rise=2 fall=5 timeout=1000 default_down=true type=udp;
    }

    server {
        listen 5353 udp;
        proxy_pass udp_backends;
    }
}
```

## Directives

### check

```nginx
check interval=milliseconds [fall=count] [rise=count] [timeout=milliseconds]
      [default_down=true|false] [type=tcp|udp] [port=check_port]
      [reuse=on|off]
```

Default parameters when omitted:
`interval=30000 fall=5 rise=2 timeout=1000 default_down=true type=tcp reuse=off`

With `type=udp`, an omitted `default_down` defaults to `false`. An explicit value
takes precedence regardless of the order of the `type` and `default_down` parameters.

Context: `http/upstream`, `stream/upstream`

Parameters:

- `interval`: minimum milliseconds from the last committed result to the next
  round's start; `1` is supported.
- `fall`: mark the peer down after this many consecutive check failures.
- `rise`: mark the peer up after this many consecutive check successes.
- `timeout`: timeout for one health check in milliseconds. UDP preparation,
  sending, and receiving share one fixed whole-round budget.
- `default_down`: initial peer state. `true` means the peer starts as down until
  it passes enough checks.
- `type`: check protocol, either `tcp` or `udp`.
- `port`: optional check port. If omitted, the upstream server port is used.
- `reuse`: reuse TCP check connections. The default is `off`; `reuse=on` is
  valid only with `type=tcp`. When reuse is enabled, the first check on a new
  connection performs a real TCP connect to the upstream. Later checks on the
  saved connection verify the kernel-maintained connection state by peeking from
  the socket instead of opening a new upstream connection every time.

TCP checks connect to the peer and peek one byte. Each UDP round uses a separate
connected socket and sends the fixed `NGX_UDP_CHECKER` payload to the actual check
address. Failures of send/recv and the value from a successful `SO_ERROR` query
provide error evidence. Only `ECONNREFUSED`, observed after a send attempt in a
still-valid round before its deadline, counts as failure. Empty datagrams, ordinary
replies, silent deadlines without confirmed failure, and local resource exceptions
count as success when the round finishes. Success means “no failure confirmed in
this round”; it does not prove application health. `EAGAIN`/`EINTR` wait or retry
within the original deadline, and a successfully sent datagram is sent once per round.

A failure increments fall and clears rise; a success increments rise and clears
fall. The configured thresholds control down/up transitions. Ordinary socket error
feedback depends on the kernel and network; late errors after port reuse have
limited network-level association guarantees.

### check_keepalive_requests

```nginx
check_keepalive_requests number;
```

Default: `10`

Context: `http/upstream`, `stream/upstream`

This directive limits how many health checks can be performed on one reused TCP
health-check connection. It is useful only with `type=tcp reuse=on`. A reused
connection is also closed when the backend closes it, the check fails or times
out, Nginx drains reusable connections, or the worker exits.

### check_shm_size

```nginx
check_shm_size size;
```

Default: `1m`

Context: `http`, `stream`

This directive sets the shared memory size used to store health-check state. Use
a larger value when checking many upstream servers.

### healthcheck_status

```nginx
healthcheck_status [html|csv|json|prometheus];
```

Default: `html`

Context: `http/server`, `http/location`

This directive exposes the health-check status from an HTTP endpoint. It can
display both HTTP and Stream upstream check state.

The output format and status filter can also be selected with query parameters:

```text
/status?format=html
/status?format=csv
/status?format=json
/status?format=prometheus
/status?format=json&status=down
/status?format=json&status=up
```

`status` accepts `up` or `down`.

The HTML, CSV, and JSON outputs include delay statistics for each peer:

- `last_delay_ms`: last successful check delay.
- `avg_delay_ms`: average successful check delay.
- `min_delay_ms`: minimum successful check delay.
- `max_delay_ms`: maximum successful check delay.

The Prometheus output currently exposes total/up/down/generation gauges and
per-peer rise, fall, and active metrics.

UDP 延迟字段固定为数值 `0`，表示不适用，消费端通过 `type` 区分。
每个状态请求逐 peer 读取完整的已发布记录，采集时保存筛选结果，读全后统一输出；
JSON `total` 与输出数组长度一致。各 peer 可以来自不同采样时刻。
短暂读竞争通过事件循环异步重试；共享状态缺失、分配或格式化失败仍返回 HTTP 500。
静态 `server down`、主动健康状态和被动失败冷却分别参与选路资格判断，状态页的 up 表示主动检查状态。

### check_status

```nginx
check_status [html|csv|json];
```

Default: `html`

Context: `http/server`, `http/location`

`check_status` is the legacy HTTP-only status directive inherited from the
original module. New deployments should use `healthcheck_status`, which is the
maintained status interface and supports Prometheus output.

### 状态请求的竞争与生命周期

两个状态入口共用请求级增量采集：每轮最多处理 256 条记录，批量用尽后等待 1ms 继续；
竞争时等待 5ms 再尝试当前 peer，已读记录及筛选计数保存在请求池中。
成功响应包含本次请求所需的完整数据，采集等待随请求生命周期结束。
客户端取消会清理重试事件，GET/HEAD 请求体由核心丢弃流程处理，子请求完成后唤醒父请求。

当前 peer 等待达到 1s 时记录 `healthcheck status waiting`，包括 module、peer、generation、
holder PID 和 `elapsed_ms`；同一请求后续日志至少间隔 5s。该阈值用于诊断。
活持有者继续受互斥协议保护，确认 PID 不存在后沿用死亡锁恢复。

正常 master/worker reload 时，旧请求保持旧代数据源，新请求使用新代。
采集 timer 属于活跃请求并参与优雅排空；配置了 `worker_shutdown_timeout` 时由核心在到期后
结束等待，默认值 `0` 则继续等待请求完成或客户端取消。该行为也适用于长期停顿的活持有者。

## Status Output

JSON output uses this shape:

```json
{
  "servers": {
    "total": 1,
    "generation": 1,
    "http": [
      {
        "index": 0,
        "upstream": "web_backends",
        "name": "127.0.0.1:8081",
        "status": "up",
        "rise": 2,
        "fall": 0,
        "type": "tcp",
        "port": 0,
        "last_delay_ms": 1,
        "avg_delay_ms": 1,
        "min_delay_ms": 1,
        "max_delay_ms": 2
      }
    ],
    "stream": []
  }
}
```

CSV output starts with this header:

```text
index,upstream_type,upstream_name,host,rise,fall,check_type,check_port,last_delay,avg_delay,min_delay,max_delay,status
```

Prometheus scraping example:

```nginx
location /metrics/upstream {
    healthcheck_status prometheus;
}
```

## Reload and Process Lifecycle

The module uses Nginx's native configuration, shared-memory, and process
callbacks. With the default `master_process on`, a failed configuration load
keeps the existing configuration and workers active. A successful reload starts
workers with the new configuration while old workers finish their existing
requests and connections under Nginx's graceful shutdown rules.

Health-check data belongs to its configuration. The native `init_module`
callback releases any old process-local check resources before Nginx frees the
old shared-memory zone. The native `init_process` callback binds the effective
worker/single-process configuration and starts checks; `exit_process` cleans up
checks when the process exits.

Normal reload matches old peers one-to-one by module, upstream name, business
address, actual check address, and type. It inherits complete health values and
TCP delays while rebuilding runtime ownership. If an old snapshot cannot be read
within its per-peer 1ms budget, that peer starts with the new `default_down` and
zero counters. Configuration and shared-memory initialization errors still reject
the load. Check timers are cancelable, cleanup is idempotent, and published health
remains available while existing business connections drain.

### Single-process compatibility

`master_process off` is a developer mode. In the tested Nginx 1.26.3 core, a
successful reload in this mode does not run process initialization again, and
a control build without healthcheck also loses normal HTTP service afterward.
The module releases its old check resources on configuration commit; new checks
start when Nginx runs process initialization. Use the normal master/worker mode
for operational reloads. `worker_processes 1` with the default master setting
uses that normal lifecycle.

## Runtime Notes

- HTTP and Stream each use their own peer index space. TCP reuse on/off and UDP
  all initially shard by `peer_index % worker_processes`. Effective workers
  share health results; single-process mode owns all peers, and native role
  checks exclude cache manager/loader helpers.
- Initial checks are staggered in `[0,max(interval,1000ms))`. Subsequent periodic
  wakeups are at least 1ms. Event-loop scheduling and `timer_resolution` affect
  actual timing; `interval=1` does not promise exact 1ms probes.
- Each worker has one watchdog per module with checked peers. A batch visits at
  most 256 records or consumes a 1ms budget. Stage progress deadlines plus
  `max(1000ms,2×timer_resolution)` grace identify stalled owners. Recently active
  backups use stable preference scores, then bounded-window fallback competition.
  The new owner continues across rounds; a recovered old owner cleans up stale
  resources. Successful reload reshards peers in the new configuration.
- Each peer has one valid result publisher. A paused process may temporarily
  retain an old socket; configuration, instance, term, and round identity isolate
  it on recovery. Takeover requires another runnable worker.
- `reuse=off` opens and closes a TCP connection per round. `reuse=on` retains it
  according to its request limit and connection state. Stable periodic timer count
  is approximately the peer count, plus in-flight deadlines and watchdogs. Small
  probe contexts are allocated on first actual ownership and reused within the
  configuration; their worst-case memory count remains peers × workers.
- TCP reuse changes what a repeated check proves. The first check on a new TCP
  connection validates that Nginx can connect to the upstream. Reused checks call
  `recv(..., MSG_PEEK)` on the saved socket; if the socket is still readable or
  would block with `EAGAIN`, the peer is treated as healthy. This confirms the
  existing kernel connection has not observed a close or socket error, but it
  does not create a new connection for every interval.
- Health-check TCP reuse is independent from normal upstream keepalive
  connections.
- Idle TCP connections keep read monitoring. FIN, RST, socket errors, backend
  data, or reclamation close them for reconnection next round; idle cleanup itself
  contributes no failure sample. Select/poll discard redundant idle write interest.
- Silent UDP deadlines appear in committed-result debug details. HTTP state
  transitions log at ERR and Stream transitions at NOTICE. Log levels and
  `--with-debug` affect diagnostics; checks, counters, cleanup, and routing run
  independently of those switches.

## Development Validation

Build the module against the target Nginx source tree, then run `nginx -t` and
upstream traffic checks before deployment.

## Credits and License

Maintainer:

- Akvicor

Based on:

- [yaoweibin/nginx_upstream_check_module](https://github.com/yaoweibin/nginx_upstream_check_module)

Original authors:

- Weibin Yao / Yao Weibin
- Matthieu Tourne

Historical copyright and design notes from the upstream README:

- The upstream module borrowed the health-check design from Jack Lindamood's
  `healthcheck_nginx_upstreams`.
- The upstream README template was copied from agentzh.
- Copyright (C) 2014 by Weibin Yao.
- Copyright (C) 2010-2014 Alibaba Group Holding Limited.
- Copyright (C) 2014 by LiangBin Li.
- Copyright (C) 2014 by Zhuo Yuan.
- Copyright (C) 2012 by Matthieu Tourne.

This project also includes code with copyright notices from Changxun Zhou's
maintenance branch.

The upstream project is licensed under the BSD license. Redistribution and use
must retain the copyright notice, license conditions, and disclaimer from the
original project.
