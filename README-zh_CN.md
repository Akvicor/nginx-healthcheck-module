# nginx-healthcheck-module

[English documentation](README.md)

[博客链接](https://www.ksyaki.com/archives/nginx-shang-you-jian-kang-jian-cha-cha-jian)

用于 Nginx 1.26+ 的主动 upstream 健康检查模块。

本项目由 Akvicor 维护，修改自
[yaoweibin/nginx_upstream_check_module](https://github.com/yaoweibin/nginx_upstream_check_module)。
当前版本保留主动健康检查模型，适配新版 Nginx upstream 内部结构，并聚焦于
`http` 与 `stream` upstream 的 TCP/UDP 检查。

## 功能

- 支持 Nginx 1.26+，仓库内提供对应 upstream 补丁。
- 支持主动 `type=tcp` 和 `type=udp` 健康检查。
- 支持 `http` upstream 和 `stream` upstream。
- 可在 Nginx upstream 负载均衡过程中跳过不健康后端。
- 状态接口支持 `html`、`csv`、`json`、`prometheus` 输出。
- 状态输出中包含检查延迟统计：最后一次、平均、最小、最大延迟，单位毫秒。
- 支持 TCP 健康检查连接复用：`reuse=on`。

原项目中的 HTTP、FastCGI、MySQL、AJP、SSL hello 等七层检查，在当前维护版本中不再支持。

## 兼容性

- 目标 Nginx 版本：1.26+。
- 模块必须通过 `--add-module` 静态编译。
- 暂不支持动态模块加载。
- `stream` 健康检查要求 Nginx 编译时启用 `--with-stream`。

仓库内补丁会为 Nginx 内置 HTTP 和 Stream upstream 负载均衡器添加主动健康检查过滤，
包括 round robin、hash、consistent hash、适用场景下的 ip_hash，以及 least_conn。

## 安装

如果希望通过 Debian 软件源安装预编译的软件包，请参阅
[博客中的安装指南](https://www.ksyaki.com/archives/nginx-shang-you-jian-kang-jian-cha-cha-jian)。

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

请保留你当前 Nginx 构建所需的其他 configure 参数。如果需要 `stream {}` 健康检查，
需要保留 `--with-stream`。

## 配置示例

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

## 指令

### check

```nginx
check interval=milliseconds [fall=count] [rise=count] [timeout=milliseconds]
      [default_down=true|false] [type=tcp|udp] [port=check_port]
      [reuse=on|off]
```

参数省略时默认值：
`interval=30000 fall=5 rise=2 timeout=1000 default_down=true type=tcp reuse=off`

上下文：`http/upstream`、`stream/upstream`

参数说明：

- `interval`：健康检查间隔，单位毫秒。
- `fall`：连续失败达到该次数后，后端被标记为 down。
- `rise`：连续成功达到该次数后，后端被标记为 up。
- `timeout`：单次健康检查超时时间，单位毫秒。
- `default_down`：初始后端状态。`true` 表示后端初始为 down，直到连续成功次数达标。
- `type`：检查协议，只支持 `tcp` 或 `udp`。
- `port`：可选检查端口。省略时使用 upstream server 的端口。
- `reuse`：是否复用 TCP 检查连接。默认 `off`；`reuse=on` 只允许搭配 `type=tcp`。
  开启复用后，新连接上的第一次检查会真正向上游发起 TCP connect；后续复用同一连接的检查
  会通过 socket peek 验证内核维护的连接状态，而不是每次都重新建立一条到上游的连接。

TCP 检查会连接到后端并尝试 peek 一个字节。UDP 检查会发送一段默认数据，并通过接收路径检测
ICMP 错误。

### check_keepalive_requests

```nginx
check_keepalive_requests number;
```

默认值：`10`

上下文：`http/upstream`、`stream/upstream`

该指令限制单条复用 TCP 健康检查连接最多执行多少次检查，只在
`type=tcp reuse=on` 时有意义。后端关闭连接、检查失败或超时、Nginx 回收 reusable
connection，或 worker 退出时，也会关闭该复用连接。

### check_shm_size

```nginx
check_shm_size size;
```

默认值：`1m`

上下文：`http`、`stream`

该指令设置保存健康检查状态的共享内存大小。如果检查的 upstream server 数量很多，可以调大该值。

### healthcheck_status

```nginx
healthcheck_status [html|csv|json|prometheus];
```

默认值：`html`

上下文：`http/server`、`http/location`

该指令通过 HTTP endpoint 暴露健康检查状态，可展示 HTTP 和 Stream upstream 的检查状态。

也可以通过 URL 参数临时指定输出格式和状态过滤：

```text
/status?format=html
/status?format=csv
/status?format=json
/status?format=prometheus
/status?format=json&status=down
/status?format=json&status=up
```

`status` 支持 `up` 或 `down`。

HTML、CSV、JSON 输出包含每个后端的延迟统计：

- `last_delay_ms`：最后一次成功检查延迟。
- `avg_delay_ms`：成功检查平均延迟。
- `min_delay_ms`：成功检查最小延迟。
- `max_delay_ms`：成功检查最大延迟。

Prometheus 输出当前包含总数/up/down/generation 指标，以及每个后端的 rise、fall、active 指标。

### check_status

```nginx
check_status [html|csv|json];
```

默认值：`html`

上下文：`http/server`、`http/location`

`check_status` 是继承自原模块的旧 HTTP-only 状态指令。新部署建议使用
`healthcheck_status`，它是当前维护的状态接口，并支持 Prometheus 输出。

## 状态输出

JSON 输出结构如下：

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

CSV 输出表头：

```text
index,upstream_type,upstream_name,host,rise,fall,check_type,check_port,last_delay,avg_delay,min_delay,max_delay,status
```

Prometheus 抓取示例：

```nginx
location /metrics/upstream {
    healthcheck_status prometheus;
}
```

## 运行说明

- `reuse=off` 时，每个 worker 保持原有健康检查定时器行为，每次 TCP 检查结束后关闭连接。
- `type=tcp reuse=on` 时，peer 按 `peer_index % worker_processes` 分配；每个被检查的
  peer 由一个 worker 负责健康检查，从而使复用健康检查连接数接近 peer 数，而不是
  `peer 数 * worker_processes`。
- TCP 复用会改变重复检查所验证的内容。新 TCP 连接上的第一次检查会验证 Nginx 是否能连接到
  上游；复用检查会在已保存的 socket 上调用 `recv(..., MSG_PEEK)`，如果 socket 仍可读或返回
  `EAGAIN`，则认为该 peer 健康。这确认的是现有内核连接尚未观察到关闭或 socket 错误，
  但不会在每个检查周期都新建连接。
- 健康检查 TCP 复用与普通 upstream keepalive 相互独立。
- UDP 检查保持当前语义：超时但未收到 ICMP 错误时按成功处理。
- 当前维护版本不要配置 `type=http`、`type=fastcgi`、`type=mysql`、`type=ajp`、
  `type=ssl_hello`、`check_http_*` 或 `check_fastcgi_*` 指令。

## 致谢与许可

当前维护者：

- Akvicor

来源项目：

- [yaoweibin/nginx_upstream_check_module](https://github.com/yaoweibin/nginx_upstream_check_module)

原项目作者：

- Weibin Yao / 姚伟斌
- Matthieu Tourne

原项目 README 中的历史版权与设计说明：

- 原 upstream 模块的健康检查设计借鉴自 Jack Lindamood 的 `healthcheck_nginx_upstreams`。
- 原 upstream README 模板来自 agentzh。
- Copyright (C) 2014 by Weibin Yao.
- Copyright (C) 2010-2014 Alibaba Group Holding Limited.
- Copyright (C) 2014 by LiangBin Li.
- Copyright (C) 2014 by Zhuo Yuan.
- Copyright (C) 2012 by Matthieu Tourne.

本项目也包含带有 Changxun Zhou 维护分支版权注记的代码。

原 upstream 项目使用 BSD license。分发和使用时需要保留原项目版权声明、许可条件和免责声明。
