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
- Keeps TCP check connections to reduce probe traffic. `reuse` controls periodic
  reconnection; `keepalive` also enables kernel TCP keepalive probes. Both can be
  enabled together.

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

        check interval=1000 timeout=900 type=tcp reuse=30s keepalive=30s:5s:1s:3;
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
      [reuse=off|on|time] [keepalive=off|on|rehandshake[:idle[:intvl[:cnt]]]]
```

Default parameters when omitted:
`interval=5000 fall=3 rise=2 default_down=false type=tcp reuse=off keepalive=off`.
An omitted `timeout` is `min(5000, interval - interval / 10)`, using integer
division. With the default interval, the timeout is `4500`ms. Defaults and
cross-parameter checks are evaluated after the whole directive is parsed, so
parameter order does not matter. The directive requires at least one parameter;
`check type=tcp;` uses the other defaults.

Context: `http/upstream`, `stream/upstream`

Parameters:

- `interval`: milliseconds between the starts of consecutive rounds for one
  peer; minimum `100`. Rounds remain serial: a new round starts after the previous
  result is committed. If a round overruns the interval, the next starts after
  its result is committed, without adding another full interval.
- `fall`: mark the peer down after this many consecutive check failures.
- `rise`: mark the peer up after this many consecutive check successes.
- `timeout`: timeout in milliseconds, positive and strictly less than `interval`.
  The 5000ms cap applies to the default only; explicit values may be larger.
  UDP preparation, sending, and receiving share a fixed whole-round budget.
- `default_down`: initial active-check state, default `false` for both TCP and
  UDP. Newly started or added peers begin as up; `true` starts them as down until
  the rise threshold is reached. Matching peers inherit their state on reload.
- `type`: check protocol, either `tcp` or `udp`.
- `port`: optional check port in `1`-`65535`; omitted values use the upstream
  server's port. Unix socket servers have no port, so configuring `port` for them
  fails `nginx -t`.
- `reuse`: keep TCP check connections and reconnect periodically. `off` opens
  and closes a connection each round when keepalive is also off. `on` uses
  `max(30s, 2 × interval)` as the reconnection interval. A time value sets a
  custom interval, which must be greater than `interval`. Bare numbers are
  seconds; Nginx second-based units such as `30s` and `5m` are accepted.
- `keepalive`: keep TCP check connections and let the kernel probe the peer.
  The format is `rehandshake:idle:intvl:cnt`; compared with Nginx's
  `so_keepalive=idle:intvl:cnt`, it adds the reconnection interval as the first
  field. Trailing fields may be omitted, and empty fields use module defaults:

  | Field | Purpose | Module default |
  |---|---|---|
  | `rehandshake` | Connection lifetime before periodic reconnection | `max(30s, 2 × interval)` |
  | `idle` | Idle time before a kernel keepalive probe (`TCP_KEEPIDLE`) | `interval` rounded up to seconds, adjusted as described below |
  | `intvl` | Retry interval for unanswered probes (`TCP_KEEPINTVL`) | `1s` |
  | `cnt` | Unanswered probes before the kernel drops the connection (`TCP_KEEPCNT`) | `3` |

  `keepalive=on` uses all module defaults. The first field can also be `on` or
  empty, or `off` to reconnect only after the connection breaks. Bare time values
  are seconds; `cnt` is an integer count. `idle` and `intvl` must be whole seconds
  in `1`-`32767`; `cnt` must be in `1`-`127`, and `intvl` must not exceed `idle`.

  A finite reconnection interval must exceed `interval`, and an explicit `idle`
  must be smaller than the effective reconnection interval. For an omitted
  `idle`, the module chooses the smaller of the rounded-up interval and the
  largest whole second below the effective reconnection interval, capped at
  32767 seconds. If no positive idle time fits, configuration fails.

  All kernel options are set explicitly, so omitted fields never use the
  operating system's defaults. The platform must support `TCP_KEEPIDLE`,
  `TCP_KEEPINTVL`, and `TCP_KEEPCNT`; otherwise enabling keepalive fails
  `nginx -t`. Linux supports these options; the tested macOS build rejects them.

`reuse` and `keepalive` are TCP-only. They may be enabled together; the effective
reconnection interval is the smaller of `reuse` and the first keepalive field,
with `off` treated as infinite. Once the connection reaches that age, it is
replaced on the next check round. This is independent of ordinary upstream
keepalive connections used by business requests.

| Mode | Each round | Kernel keepalive probes | Periodic reconnection |
|---|---|---|---|
| Both off (default) | New connection, then close | None | Every round |
| `reuse` only | Peek local connection state | None | After the configured lifetime |
| `keepalive` only | Peek, including kernel-reported connection errors | Per `idle`/`intvl`/`cnt` | After its first-field lifetime |
| Both enabled | Same as above | Same as above | After the smaller lifetime |

```nginx
# 每 1s 检查，健康期约每 5s 一个 keepalive 往返，连接存活 30s 后重新握手。
check interval=1000 timeout=900 type=tcp reuse=30s keepalive=30s:5s:1s:3;

# keepalive 首位 off 不贡献有限存活时间，由 reuse 控制重新握手。
check interval=1000 type=tcp reuse=30s keepalive=off:5s:1s:3;

# idle 省略时自动调小，确保早于重新握手发生。
check interval=1500 type=tcp reuse=2s keepalive=on;
```

Keepalive probes carry no application data and are scheduled by the kernel,
independently of check rounds. A round reads the kernel's connection state; it
does not force a probe or require a new ACK for that particular round. After
about `idle + intvl × cnt` without a reply (plus scheduling delay), the kernel
drops the connection. A count of 3 tolerates two unanswered probes while the
connection remains open; the third unanswered probe reaches the failure limit.

A broken check connection alone is not a failed health sample. If peeking fails
during a round, that round reconnects and counts the new connection's result.
FIN, RST, or keepalive timeout during idle closes the old connection, and the next
round reconnects. This avoids marking a healthy server down simply because one
of its idle connections expired.

Keepalive detects silent host/network loss that local peeking alone cannot
observe until reconnection. Reconnecting additionally checks that new connections
can still be established. Existing connections may survive a listener shutdown
or a firewall rule that blocks new connections; `keepalive=off:...` without a
finite reuse lifetime cannot check new-connection availability. Neither approach
verifies application health beyond TCP.

Server data received during idle is read and discarded while the connection
stays open. More than 4KB accumulated on one connection closes it. Services that
send a banner may still close the connection when their application-handshake
timeout expires; the next round reconnects.

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

UDP delay fields contain numeric `0` as a not-applicable value; consumers can
distinguish protocols using `type`. Each status request collects complete
published peer records and fixes the filter result before formatting output.
JSON `total` matches the emitted array length; peer records may have different
sample times. Short lock contention is retried asynchronously; missing shared
state, allocation failures, or formatting failures return HTTP 500. Static
`server down`, active health, and passive failure cooldown independently affect
routing eligibility; an up status describes active-check state only.

### check_status

```nginx
check_status [html|csv|json];
```

Default: `html`

Context: `http/server`, `http/location`

`check_status` is the legacy HTTP-only status directive inherited from the
original module. New deployments should use `healthcheck_status`, which is the
maintained status interface and supports Prometheus output.

### Status request contention and lifecycle

Both status endpoints collect records incrementally. A batch visits at most 256
records, yields for 1ms when exhausted, or retries the current peer after 5ms
when its lock is busy. Collected rows and filter counts remain in the request
pool until the complete response is generated. Client cancellation cleans up
retry events; core request-body handling supports GET/HEAD, and a completed
subrequest wakes its parent.

After waiting 1s on one peer, a `healthcheck status waiting` diagnostic includes
the module, peer, generation, holder PID, and `elapsed_ms`; later messages for
the same request are at least 5s apart. Live lock holders remain protected;
confirmed nonexistent PIDs use the dead-holder recovery path.

During a normal master/worker reload, existing requests keep their old data
source and new requests use the new generation. Collection timers belong to
active requests and participate in graceful draining. The core terminates waits
when `worker_shutdown_timeout` expires; its default of `0` waits for completion
or client cancellation, including when a live lock holder is stalled.

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
- Initial checks are staggered in `[0,max(interval,1000ms))`. Later timers target
  the previous round's start plus `interval`. Event-loop scheduling and
  `timer_resolution` still affect actual timing.
- Each worker has one watchdog per module with checked peers. A batch visits at
  most 256 records or consumes a 1ms budget. Stage progress deadlines plus
  `max(1000ms,2×timer_resolution)` grace identify stalled owners. Recently active
  backups use stable preference scores, then bounded-window fallback competition.
  The new owner continues across rounds; a recovered old owner cleans up stale
  resources. Successful reload reshards peers in the new configuration.
- The watchdog retries pending work and recovers checks after worker stalls or
  crashes. At its normal 100ms tick, each batch visits up to 256 peers with a
  nominal 1ms processing budget. A full pass requires approximately
  `ceil(N/256) × 100ms` when the batch limit is reached; budget exhaustion or a
  busy event loop can lengthen it. Larger lists can delay takeover detection.
- Each peer has one valid result publisher. A paused process may temporarily
  retain an old socket; configuration, instance, term, and round identity isolate
  it on recovery. Takeover requires another runnable worker. After a crash or
  stall, workers may temporarily own uneven peer counts; a successful reload
  assigns the initial shards again.
- With both connection-retention options off, each TCP round opens and closes a
  connection. Retained connections follow the configured lifetime and kernel
  state. Stable periodic timer count
  is approximately the peer count, plus in-flight deadlines and watchdogs. Small
  probe contexts are allocated on first actual ownership and reused within the
  configuration; their worst-case memory count remains peers × workers.
- Each retained check connection consumes one connection slot and FD in its
  owning worker. Include this in `worker_connections` and open-file sizing:
  normally about peers/workers, potentially more after takeover. Nginx can
  reclaim idle reusable check connections under connection pressure; subsequent
  rounds reconnect, increasing traffic. In-flight checks also occupy slots, so
  leave room for business traffic.
- Peeking a retained connection confirms that the local TCP stack has not
  observed closure or an error. Keepalive periodically refreshes reachability
  through kernel round trips; periodic reconnection tests new connections.
- Health-check TCP reuse is independent from normal upstream keepalive
  connections.
- Idle TCP connections keep read monitoring. Server data is read and discarded
  within the 4KB lifetime limit. FIN, RST, socket errors, excess data, or
  reclamation close them for reconnection next round; idle cleanup itself
  contributes no failure sample. Select/poll discard redundant idle write interest.
- An L4 check closes without completing an application handshake. Some services
  count that as a connection error; for example MySQL can block a host after
  `max_connect_errors` consecutive interrupted connections. Retention reduces
  connection churn; protocol-level verification requires an application check.
- Silent UDP deadlines appear in committed-result debug details. HTTP state
  transitions log at ERR and Stream transitions at NOTICE. Log levels and
  `--with-debug` affect diagnostics; checks, counters, cleanup, and routing run
  independently of those switches.

## Development Validation

Build the module against the target Nginx source tree, then run `nginx -t` and
upstream traffic checks before deployment.

## Upgrading from count-based reuse

Rebuild Nginx with this module and the matching upstream patch, then validate the
updated configuration with `nginx -t` before reloading.

| Setting | Previous default/meaning | Current default/meaning |
|---|---|---|
| `interval` | 30000ms, measured from result submission | 5000ms, measured from round start; minimum 100ms |
| `timeout` | 1000ms | `min(5000, interval - interval / 10)`; strictly below interval |
| `fall` / `rise` | 5 / 2 | 3 / 2 |
| TCP `default_down` | `true` | `false`, matching UDP |
| `reuse=on` | Reconnect after a number of checks | Reconnect after `max(30s, 2 × interval)` |

The `check_keepalive_requests` directive has been removed. To migrate a count
limit of `N`, use a connection lifetime of approximately
`N × interval / 1000` seconds, accounting for the millisecond interval unit.
For example:

```nginx
# 原配置：interval=3000，check_keepalive_requests 10。
check interval=3000 timeout=1000 type=tcp reuse=30s;
```

The explicit lifetime must exceed `interval`; fractional-second lifetimes need
to be adjusted to a supported second-based value. Existing `interval < 100`,
`timeout >= interval`, out-of-range ports, and Unix-server check-port overrides
now fail configuration checks. A genuine registration failure also rejects the
configuration instead of silently bypassing health filtering.

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
