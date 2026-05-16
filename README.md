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
- Reports check delay statistics in status output: last, average, minimum, and
  maximum delay in milliseconds.
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
ip_hash where applicable, and least_conn.

## Installation

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

Context: `http/upstream`, `stream/upstream`

Parameters:

- `interval`: check interval in milliseconds.
- `fall`: mark the peer down after this many consecutive check failures.
- `rise`: mark the peer up after this many consecutive check successes.
- `timeout`: timeout for one health check in milliseconds.
- `default_down`: initial peer state. `true` means the peer starts as down until
  it passes enough checks.
- `type`: check protocol, either `tcp` or `udp`.
- `port`: optional check port. If omitted, the upstream server port is used.
- `reuse`: reuse TCP check connections. The default is `off`; `reuse=on` is
  valid only with `type=tcp`. When reuse is enabled, the first check on a new
  connection performs a real TCP connect to the upstream. Later checks on the
  saved connection verify the kernel-maintained connection state by peeking from
  the socket instead of opening a new upstream connection every time.

TCP checks connect to the peer and peek one byte. UDP checks send a small
default payload and use the resulting receive path to detect ICMP errors.

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

### check_status

```nginx
check_status [html|csv|json];
```

Default: `html`

Context: `http/server`, `http/location`

`check_status` is the legacy HTTP-only status directive inherited from the
original module. New deployments should use `healthcheck_status`, which is the
maintained status interface and supports Prometheus output.

## Status Output

JSON output uses this shape:

```json
{
  "servers": {
    "total": 2,
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

## Runtime Notes

- With `reuse=off`, each worker keeps its existing health-check timer behavior
  and closes each TCP check connection after the check finishes.
- With `type=tcp reuse=on`, peers are assigned by
  `peer_index % worker_processes`; each checked peer is owned by one worker for
  health checks, keeping the reused health-check connection count close to the
  peer count instead of `peer count * worker_processes`.
- TCP reuse changes what a repeated check proves. The first check on a new TCP
  connection validates that Nginx can connect to the upstream. Reused checks call
  `recv(..., MSG_PEEK)` on the saved socket; if the socket is still readable or
  would block with `EAGAIN`, the peer is treated as healthy. This confirms the
  existing kernel connection has not observed a close or socket error, but it
  does not create a new connection for every interval.
- Health-check TCP reuse is independent from normal upstream keepalive
  connections.
- UDP checks keep the existing semantics: a timeout without an ICMP error is
  treated as success.
- Do not configure `type=http`, `type=fastcgi`, `type=mysql`, `type=ajp`,
  `type=ssl_hello`, `check_http_*`, or `check_fastcgi_*` directives in this
  maintained version.

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
