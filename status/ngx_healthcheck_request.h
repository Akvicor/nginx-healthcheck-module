#ifndef NGX_HEALTHCHECK_REQUEST_H
#define NGX_HEALTHCHECK_REQUEST_H

#include "ngx_healthcheck_snapshot.h"

/* 两种状态入口在完整采集后使用各自格式化函数和参数。 */
typedef ngx_int_t (*ngx_healthcheck_status_output_pt)(
    ngx_healthcheck_status_writer_t *writer, ngx_healthcheck_snapshot_t *snapshots,
    ngx_uint_t argument);

/* 同步完成时直接返回输出结果；需延续时保留请求并以 NGX_DONE 交回核心。 */
ngx_int_t ngx_healthcheck_status_request(ngx_http_request_t *r,
    ngx_upstream_check_peers_t **sources, ngx_uint_t count, ngx_uint_t flag,
    ngx_healthcheck_status_output_pt output, ngx_uint_t argument);

#endif
