# nginx-healthcheck-module

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
- 状态输出包含 TCP 检查的最后一次、平均、最小、最大延迟，单位毫秒；UDP 对应字段为数字 0。
- 支持 TCP 健康检查连接复用：`reuse=on`。

原项目中的 HTTP、FastCGI、MySQL、AJP、SSL hello 等七层检查，在当前维护版本中不再支持。

## 兼容性

- 目标 Nginx 版本：1.26+。
- 模块必须通过 `--add-module` 静态编译。
- 暂不支持动态模块加载。
- `stream` 健康检查要求 Nginx 编译时启用 `--with-stream`。

仓库内补丁会为 Nginx 内置 HTTP 和 Stream upstream 负载均衡器添加主动健康检查过滤，
包括 round robin、hash、consistent hash、HTTP ip_hash、least_conn、random 和 random two。
[nginx-upstream-fair](https://github.com/Akvicor/nginx-upstream-fair) 通过共同静态构建接入检查，
支持主组优先、备用组后备，两组节点都持续探测。

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

选择 `type=udp` 且省略 `default_down` 时，默认值为 `false`；显式设置优先，
与 `type`、`default_down` 的参数顺序无关。

上下文：`http/upstream`、`stream/upstream`

参数说明：

- `interval`：上次结果实际提交至下一轮启动的最小间隔，单位毫秒，允许 `1`。
- `fall`：连续失败达到该次数后，后端被标记为 down。
- `rise`：连续成功达到该次数后，后端被标记为 up。
- `timeout`：单次健康检查超时时间，单位毫秒；UDP 的准备、发送和接收共用整轮固定预算。
- `default_down`：初始后端状态。`true` 表示后端初始为 down，直到连续成功次数达标。
- `type`：检查协议，只支持 `tcp` 或 `udp`。
- `port`：可选检查端口。省略时使用 upstream server 的端口。
- `reuse`：是否复用 TCP 检查连接。默认 `off`；`reuse=on` 只允许搭配 `type=tcp`。
  开启复用后，新连接上的第一次检查会真正向上游发起 TCP connect；后续复用同一连接的检查
  会通过 socket peek 验证内核维护的连接状态，而不是每次都重新建立一条到上游的连接。

TCP 检查会连接到后端并尝试 peek 一个字节。UDP 每轮使用独立 connected socket，
向实际检查地址发送固定负载 `NGX_UDP_CHECKER`；实际发送失败、接收失败或成功读取的
`SO_ERROR` 值用于识别错误。只有已尝试发送、仍有效且尚未截止的轮次观察到
`ECONNREFUSED` 才计失败；空数据报、普通回复、无确认失败的静默截止和本机资源异常
等终结结果计成功。这里的成功表示“本轮未确认失败”，不能证明应用服务健康。
`EAGAIN`/`EINTR` 在原截止内等待或有界重试，发送成功后每轮只发送一个数据报。

失败使 fall 递增并清零 rise，成功使 rise 递增并清零 fall，达到相应阈值才转换状态。
普通 socket 错误反馈受内核和网络条件影响；端口复用后迟到错误的网络关联能力有限。

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

UDP 的延迟字段始终为数字 `0`，表示不适用；消费方可根据 `type` 区分。
请求先逐 peer 读取一次完整已发布状态并固定筛选结果，再生成输出；JSON 的 `total`
等于实际输出数组长度。各 peer 的采样时刻可以不同，读取或分配失败返回 HTTP 500。
静态 `server down`、主动健康状态和被动失败冷却分别生效；状态页的 up 仅代表主动状态。

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

- HTTP/Stream 分别在自己的 peer 索引空间内，将 TCP reuse on/off 和 UDP
  统一按 `peer_index % worker_processes` 初始分片；有效 worker 共享健康结果，
  单进程负责全部 peer，缓存 manager/loader 等 helper 通过原生角色判断排除。
- 首轮在 `[0,max(interval,1000ms))` 错峰，后续周期唤醒最小为 1ms。
  实际时刻受事件循环和 `timer_resolution` 影响，`interval=1` 不承诺精确 1ms 探测。
- 每 worker、每个有检查节点的模块设置一条备用巡检 timer；每批最多 256 条或
  1ms 预算，按阶段进度期限加 `max(1000ms,2×timer_resolution)` 宽限判断停顿。
  近期活跃的备用按稳定评分优选，有限窗口后开放兜底竞争。接管者跨轮持续负责，
  原执行者恢复后清理失效资源；成功 reload 在新配置中重新分片。
- 每个 peer 只有一个有效结果发布者；暂停进程可能暂时保留旧 socket，恢复后按
  配置、实例、任期和轮次身份隔离。单 worker 或全部 worker 停顿时无法保证接管。
- `reuse=off` 每轮新建并关闭 TCP 检查连接；`reuse=on` 按次数上限和连接状态保留。
  正常稳定期周期 timer 总量约为 peer 数，另计在途 timeout 和巡检；小探测上下文
  按首次实际负责时分配并在配置内复用，最坏内存边界仍为 peer 数 × worker 数。
- TCP 复用会改变重复检查所验证的内容。新 TCP 连接上的第一次检查会验证 Nginx 是否能连接到
  上游；复用检查会在已保存的 socket 上调用 `recv(..., MSG_PEEK)`，如果 socket 仍可读或返回
  `EAGAIN`，则认为该 peer 健康。这确认的是现有内核连接尚未观察到关闭或 socket 错误，
  但不会在每个检查周期都新建连接。
- 健康检查 TCP 复用与普通 upstream keepalive 相互独立。
- TCP idle 阶段保留读监听；FIN、RST、socket 错误、服务端数据或回收通知关闭连接，
  下一轮重连，idle 清理本身不计失败。select/poll 的多余写关注在进入 idle 时撤销。
- UDP 静默原因随有效提交的 debug 结果明细记录；HTTP 状态转换为 ERR，Stream 为 NOTICE。
  日志级别及 `--with-debug` 仅影响诊断，探测、计数、清理和业务过滤始终执行。

## 配置重载与进程生命周期

模块使用 Nginx 原生的配置、共享内存和进程回调。默认 `master_process on`
模式下，配置加载失败时，旧配置和原有 worker 继续工作；加载成功后，新 worker
使用新配置，旧 worker 按 Nginx 原生优雅退出规则完成已有请求和连接。

健康检查数据归属于相应配置。原生 `init_module` 回调在 Nginx 释放旧共享区之前，
清理本进程旧检查资源；原生 `init_process` 回调绑定有效 worker/单进程配置并启动
检查；`exit_process` 在进程退出时完成检查资源清理。

正常 reload 按模块、upstream 名、业务地址、实际检查地址和类型一对一匹配旧状态，
继承完整健康值及 TCP 延迟，重建运行资格。旧快照在单 peer 1ms 预算内不可读时，
该 peer 使用新配置的 `default_down` 和零计数；真实配置或共享内存初始化错误仍拒绝加载。
探测 timer 可取消，退出清理幂等，已发布健康值继续供旧业务排空期读取。

### 单进程兼容性

`master_process off` 用于开发调试。已测试的 Nginx 1.26.3 核心在该模式成功 reload
后不会再次执行进程初始化，未编入 healthcheck 的对照构建也会失去正常 HTTP 服务。
模块在配置提交时释放旧检查资源，新检查由 Nginx 的进程初始化驱动。运行环境的
reload 使用正常 master/worker 模式；默认 master 配置下的 `worker_processes 1`
同样使用这一正常生命周期。

## 开发验收

针对目标 Nginx 源码完成构建后，使用 `nginx -t` 和实际 upstream 流量检查配置。

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
