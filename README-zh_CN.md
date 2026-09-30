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
- 保留 TCP 检查连接以降低探测流量：`reuse` 控制定期重新握手，`keepalive` 同时启用
  内核 TCP keepalive 探测，两者可以组合使用。

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

## 指令

### check

```nginx
check interval=milliseconds [fall=count] [rise=count] [timeout=milliseconds]
      [default_down=true|false] [type=tcp|udp] [port=check_port]
      [reuse=off|on|time] [keepalive=off|on|rehandshake[:idle[:intvl[:cnt]]]]
```

参数省略时默认值：
`interval=5000 fall=3 rise=2 default_down=false type=tcp reuse=off keepalive=off`。
省略 `timeout` 时取 `min(5000, interval - interval / 10)`，使用整数除法；默认 interval
下为 `4500`ms。默认值和关联参数的校验在整条指令解析完成后计算，与参数顺序无关。
`check` 至少需要一个参数，例如 `check type=tcp;` 使用其余默认值。

上下文：`http/upstream`、`stream/upstream`

参数说明：

- `interval`：同一后端相邻两轮探测开始时刻的间隔，单位毫秒，最小 `100`。轮次保持
  串行：上一轮结果提交后才能开始下一轮；如果上一轮超过 interval，结果提交后启动
  下一轮，不再额外等待一个完整 interval。
- `fall`：连续失败达到该次数后，后端被标记为 down。
- `rise`：连续成功达到该次数后，后端被标记为 up。
- `timeout`：单次检查超时时间，单位毫秒，必须为正数且小于 interval。5000ms 上限仅作用
  于默认值，显式值可以更大。UDP 的准备、发送和接收共用整轮固定预算。
- `default_down`：初始主动检查状态，TCP 和 UDP 均默认 `false`，即新启动或新增的后端
  初始视为 up；`true` 表示初始为 down，直到 rise 达标。reload 时身份匹配的后端继承旧状态。
- `type`：检查协议，只支持 `tcp` 或 `udp`。
- `port`：可选检查端口，范围 `1`～`65535`；省略时使用 upstream server 的端口。
  unix socket server 没有端口，配置 port 时 `nginx -t` 报错。
- `reuse`：保留 TCP 检查连接并定期重新握手。keepalive 同时关闭时，`off` 每轮新建并关闭
  连接；`on` 使用默认重新握手间隔 `max(30s, 2 × interval)`。时间值设置自定义间隔，必须
  大于 interval；裸数字按秒计，支持 `30s`、`5m` 等 Nginx 秒级时间单位。
- `keepalive`：保留 TCP 检查连接，并让内核探测对端。格式为 `rehandshake:idle:intvl:cnt`，
  相比 Nginx 的 `so_keepalive=idle:intvl:cnt`，首位增加了重新握手间隔。后面的字段可以
  省略，空位使用模块默认值：

  | 字段 | 作用 | 模块默认值 |
  |---|---|---|
  | `rehandshake` | 到期后定期重新握手的连接存活时间 | `max(30s, 2 × interval)` |
  | `idle` | 内核发送 keepalive 探测前的空闲时间（`TCP_KEEPIDLE`） | interval 向上取整到秒，并按下述规则调整 |
  | `intvl` | 探测没有回应时的重发间隔（`TCP_KEEPINTVL`） | `1s` |
  | `cnt` | 内核判定断开前连续未回应的探测次数（`TCP_KEEPCNT`） | `3` |

  `keepalive=on` 使用全部模块默认值。首位也可以写 `on` 或留空，或者写 `off` 表示仅在
  连接断开后重连。裸时间值按秒计，cnt 为整数次数。idle 和 intvl 必须为 `1`～`32767`
  的整秒，cnt 为 `1`～`127`，且 intvl 不得大于 idle。

  有限的重新握手间隔必须大于 interval，显式 idle 必须小于实际重新握手间隔。省略 idle
  时，模块取“interval 向上取整到秒”和“小于实际重新握手间隔的最大整秒”的较小值，
  并以 32767 秒为上限；如果容不下正数 idle，配置报错。

  三项内核参数均显式设置，空位不会使用操作系统的默认值。平台必须支持 `TCP_KEEPIDLE`、
  `TCP_KEEPINTVL`、`TCP_KEEPCNT`，否则启用 keepalive 时 `nginx -t` 报错。Linux 支持
  这些选项，已测试的 macOS 构建会拒绝该配置。

reuse 和 keepalive 只适用于 TCP，可以同时启用；实际重新握手间隔取 reuse 与 keepalive
首位中的较小值，off 视为无穷大。连接存活时间达到该值后，在下一轮检查中重建连接。
该机制与业务请求使用的普通 upstream keepalive 相互独立。

| 模式 | 每轮检查 | 内核 keepalive 探测 | 主动重新握手 |
|---|---|---|---|
| 两者均 off（默认） | 新建连接并关闭 | 无 | 每轮 |
| 仅 reuse | peek 本地连接状态 | 无 | 达到配置的存活时间后 |
| 仅 keepalive | peek，包含内核报告的连接错误 | 按 idle/intvl/cnt | 达到首位的存活时间后 |
| 两者均启用 | 同上 | 同上 | 达到较小的存活时间后 |

```nginx
# 每 1s 检查，健康期约每 5s 一个 keepalive 往返，连接存活 30s 后重新握手。
check interval=1000 timeout=900 type=tcp reuse=30s keepalive=30s:5s:1s:3;

# keepalive 首位 off 不贡献有限存活时间，由 reuse 控制重新握手。
check interval=1000 type=tcp reuse=30s keepalive=off:5s:1s:3;

# idle 省略时自动调小，确保早于重新握手发生。
check interval=1500 type=tcp reuse=2s keepalive=on;
```

keepalive 探测不带应用数据，由内核独立于检查轮次调度。每轮读取内核连接状态，不会强制
发出探测，也不要求这一轮获得新的 ACK。在约 `idle + intvl × cnt` 都没有回应后
（另加调度延迟），内核中止连接。cnt=3 时，前两个探测没有回应仍保留连接，第三个没有
回应达到断开阈值。

检查连接断开本身不计失败。某一轮 peek 失败时，该轮立即重新握手，以新连接的结果计数；
空闲期收到 FIN、RST 或 keepalive 超时则关闭旧连接，下一轮重新握手。这样可以避免仅因
一条空闲连接过期，就把仍能正常接受新连接的上游判为不健康。

keepalive 能发现本地 peek 在重连前无法发现的主机或网络静默失联；重新握手还能检查
是否能建立新连接。监听关闭或者防火墙只拦新连接时，已有连接仍可能正常；没有有限 reuse
存活时间的 `keepalive=off:...` 无法验证新连接可用性。两种方式均只验证 TCP 层。

空闲期收到的服务端数据会被读掉并丢弃，连接继续保留；单条连接累计超过 4KB 时关闭。
发送欢迎信息的服务也可能在应用握手超时后主动关闭连接，下一轮检查会重新握手。

TCP 检查会连接到后端并尝试 peek 一个字节。UDP 每轮使用独立 connected socket，
向实际检查地址发送固定负载 `NGX_UDP_CHECKER`；实际发送失败、接收失败或成功读取的
`SO_ERROR` 值用于识别错误。只有已尝试发送、仍有效且尚未截止的轮次观察到
`ECONNREFUSED` 才计失败；空数据报、普通回复、无确认失败的静默截止和本机资源异常
等终结结果计成功。这里的成功表示“本轮未确认失败”，不能证明应用服务健康。
`EAGAIN`/`EINTR` 在原截止内等待或有界重试，发送成功后每轮只发送一个数据报。

失败使 fall 递增并清零 rise，成功使 rise 递增并清零 fall，达到相应阈值才转换状态。
普通 socket 错误反馈受内核和网络条件影响；端口复用后迟到错误的网络关联能力有限。

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
- 首轮在 `[0,max(interval,1000ms))` 错峰，后续 timer 定位到上一轮开始时刻加 interval。
  实际时刻仍受事件循环和 `timer_resolution` 影响。
- 每 worker、每个有检查节点的模块设置一条备用巡检 timer；每批最多 256 条或
  1ms 预算，按阶段进度期限加 `max(1000ms,2×timer_resolution)` 宽限判断停顿。
  近期活跃的备用按稳定评分优选，有限窗口后开放兜底竞争。接管者跨轮持续负责，
  原执行者恢复后清理失效资源；成功 reload 在新配置中重新分片。
- 巡检用于待处理结果的重试，以及 worker 停顿或崩溃后的恢复。正常每 100ms 一批，
  最多扫描 256 条，名义处理预算为 1ms。达到批量上限时完整扫描一轮约需
  `ceil(N/256) × 100ms`，预算用尽或事件循环繁忙时可能更久；列表较大会延迟接管发现。
- 每个 peer 只有一个有效结果发布者；暂停进程可能暂时保留旧 socket，恢复后按
  配置、实例、任期和轮次身份隔离。单 worker 或全部 worker 停顿时无法保证接管。
  worker 崩溃或停顿后，负责的 peer 数可能暂时不均衡，成功 reload 后重新初始分片。
- 保持连接的两个参数均关闭时，TCP 每轮新建并关闭连接；保留的连接按配置存活时间和内核
  状态维护。
  正常稳定期周期 timer 总量约为 peer 数，另计在途 timeout 和巡检；小探测上下文
  按首次实际负责时分配并在配置内复用，最坏内存边界仍为 peer 数 × worker 数。
- 每条保留的检查连接在执行 worker 中占用一个连接槽和 FD，规划 `worker_connections`
  和打开文件上限时需计入：正常约为 peer 数 / worker 数，接管后单 worker 可能更多。
  连接紧张时 Nginx 可以回收空闲的 reusable 检查连接，后续轮次重连并增加流量；在途检查
  也占连接槽，因此需要为业务流量保留余量。
- 保留连接的 peek 确认本地 TCP 栈尚未观察到关闭或错误；keepalive 通过内核往返定期刷新
  可达性，定期重新握手验证新连接能否建立。
- 健康检查 TCP 复用与普通 upstream keepalive 相互独立。
- TCP idle 阶段保留读监听；服务端数据在单连接累计 4KB 上限内读取并丢弃，FIN、RST、
  socket 错误、数据超限或回收通知关闭连接，下一轮重连，idle 清理本身不计失败。
  select/poll 的多余写关注在进入 idle 时撤销。
- L4 检查关闭连接前不会完成应用握手，部分服务将其计为连接错误，例如 MySQL 会在同一
  来源连续中断达到 max_connect_errors 后封禁主机。保持连接减少连接频率；协议级验证
  需要应用层检查。
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

## 从按次数复用升级

使用本模块和匹配的 upstream 补丁重新构建 Nginx，更新配置后先运行 `nginx -t`，通过后
再 reload。

| 配置 | 旧默认值或含义 | 当前默认值或含义 |
|---|---|---|
| `interval` | 30000ms，从结果提交起算 | 5000ms，从轮次开始起算，最小 100ms |
| `timeout` | 1000ms | `min(5000, interval - interval / 10)`，必须小于 interval |
| `fall` / `rise` | 5 / 2 | 3 / 2 |
| TCP `default_down` | `true` | `false`，与 UDP 一致 |
| `reuse=on` | 检查若干次后重连 | 达到 `max(30s, 2 × interval)` 后重连 |

`check_keepalive_requests` 已移除，原来 N 次的限制可近似换算为
`N × interval / 1000` 秒的连接存活时间（interval 的单位是毫秒）。例如：

```nginx
# 原配置：interval=3000，check_keepalive_requests 10。
check interval=3000 timeout=1000 type=tcp reuse=30s;
```

显式存活时间必须大于 interval；不足整秒的换算结果需要调整为支持的秒级数值。
已有的 interval 小于 100、timeout 不小于 interval、端口越界或 unix server 配检查端口
都会在配置检查时被拒绝。真正的注册失败也会拒绝加载配置，避免静默绕过健康过滤。

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
