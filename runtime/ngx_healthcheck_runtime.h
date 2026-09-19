#ifndef NGX_HEALTHCHECK_RUNTIME_H
#define NGX_HEALTHCHECK_RUNTIME_H

#include "common.h.in"
#include <ngx_event.h>
#include <ngx_event_connect.h>

/* 单轮身份随事件和连接持有，只有旧通知清理完毕后才复用本地槽。 */
struct ngx_healthcheck_probe_s {
    ngx_upstream_check_peer_t   *peer;
    ngx_peer_connection_t       pc;
    ngx_event_t                 timeout;
    uint64_t                    instance;
    uint64_t                    term;
    uint64_t                    round;
    uint64_t                    started;
    uint64_t                    deadline;
    ngx_uint_t                  generation;
    ngx_msec_t                  delay;
    ngx_err_t                   error;
    const char                 *reason;
    const char                 *stage;
    const char                 *source;
    unsigned                    active:1;
    unsigned                    pending:1;
    unsigned                    result:1;
    unsigned                    sample:1;
    unsigned                    attempted:1;
    unsigned                    sent:1;
    unsigned                    sending:1;
    unsigned                    prepared:1;
    unsigned                    waiting_published:1;
};

/* 每个有效进程、每个有检查 peer 的模块只有一个备用巡检入口。 */
struct ngx_healthcheck_runtime_s {
    ngx_upstream_check_peers_t  *peers;
    ngx_cycle_t                *cycle;
    ngx_event_t                 watch;
    ngx_uint_t                  worker;
    ngx_uint_t                  cursor;
    ngx_uint_t                  log_level;
    const char                 *name;
    uint64_t                    instance;
    uint64_t                    grace;
    uint64_t                    interval;
    uint64_t                    preference;
    uint64_t                    budget;
    unsigned                    registered:1;
    unsigned                    timers_started:1;
    unsigned                    stopping:1;
    unsigned                    cleaned:1;
};

/* 有效 cycle 的进程入口与退出入口，共用调度及资源归属协议。 */
ngx_int_t ngx_healthcheck_runtime_init(ngx_upstream_check_peers_t *peers);
void ngx_healthcheck_runtime_stop(ngx_upstream_check_peers_t *peers);

/* 校验回调身份；NGX_AGAIN 表示暂不可读，NGX_DECLINED 表示已失效。 */
ngx_int_t ngx_healthcheck_probe_valid(ngx_healthcheck_probe_t *probe);

/* 首次安装 timeout 后登记实际截止；重试不改变本地固定截止。 */
ngx_int_t ngx_healthcheck_probe_waiting(ngx_healthcheck_probe_t *probe);

/* 固化单轮结果并尝试一次提交；锁竞争时关闭 I/O，交给巡检重试。 */
void ngx_healthcheck_finish(ngx_healthcheck_probe_t *probe, ngx_uint_t result,
    const char *reason, const char *source, ngx_err_t error);

/* 协议 I/O 的开始、重试和清理由协议模块实现。 */
void ngx_healthcheck_probe_start(ngx_healthcheck_probe_t *probe);
void ngx_healthcheck_probe_resume(ngx_healthcheck_probe_t *probe);
void ngx_healthcheck_probe_close(ngx_healthcheck_probe_t *probe);
void ngx_healthcheck_probe_cleanup(ngx_healthcheck_probe_t *probe,
    ngx_uint_t reusable);
/* 准备及 I/O 阶段共用截止处理，领取轮次时即可安装。 */
void ngx_healthcheck_timeout(ngx_event_t *event);

/* 原生 timer 只安排可表示的时间片，回调仍按宽位截止复核。 */
void ngx_healthcheck_timer(ngx_event_t *event, uint64_t delay);

#endif
