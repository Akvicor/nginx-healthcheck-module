#ifndef NGX_HEALTHCHECK_SNAPSHOT_H
#define NGX_HEALTHCHECK_SNAPSHOT_H

#include "common.h.in"
#include "ngx_healthcheck_status_writer.h"

/* 展示记录保存完整数值，配置文本通过有效请求 cycle 的配置引用。 */
typedef struct {
    ngx_uint_t                      index;
    ngx_str_t                      *upstream_name;
    ngx_addr_t                     *peer_addr;
    ngx_upstream_check_srv_conf_t   *conf;
    ngx_healthcheck_health_t         health;
} ngx_healthcheck_snapshot_peer_t;

/* peers 只包含筛选后的记录；total/up 保留 HTML 原有全组汇总语义。 */
typedef struct {
    ngx_array_t peers;
    ngx_uint_t  generation;
    ngx_uint_t  total;
    ngx_uint_t  up;
    ngx_uint_t  next; /* 已处理前缀的长度，包含被筛选掉的记录。 */
} ngx_healthcheck_snapshot_t;

/* 每请求初始化一次，后续采集复用数组和进度。 */
ngx_int_t ngx_healthcheck_snapshot_init(ngx_pool_t *pool,
    ngx_upstream_check_peers_t *peers,
    ngx_healthcheck_snapshot_t *snapshot);

/* NGX_AGAIN 表示读竞争，NGX_BUSY 表示批量预算耗尽；两者均保留完整前缀。 */
ngx_int_t ngx_healthcheck_snapshot_step(ngx_upstream_check_peers_t *peers,
    ngx_uint_t flag, ngx_healthcheck_snapshot_t *snapshot, ngx_uint_t *remaining);

/* 旧 HTTP-only 状态接口保留 HTML/CSV/JSON 的既有结构。 */
ngx_int_t ngx_healthcheck_legacy_output(ngx_healthcheck_status_writer_t *writer,
    ngx_healthcheck_snapshot_t *snapshot, ngx_uint_t format);

#endif
