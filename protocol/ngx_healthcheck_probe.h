#ifndef NGX_HEALTHCHECK_PROBE_H
#define NGX_HEALTHCHECK_PROBE_H

#include "runtime/ngx_healthcheck_runtime.h"

/* 连接事件先校验所属轮次，再由 TCP/UDP 的协议规则处理。 */
ngx_healthcheck_probe_t *ngx_healthcheck_io_ready(ngx_event_t *event);
void ngx_healthcheck_tcp_event(ngx_event_t *event);
void ngx_healthcheck_tcp_idle(ngx_event_t *event);
ngx_int_t ngx_healthcheck_tcp_peek(ngx_connection_t *connection,
    ngx_healthcheck_probe_t *probe);
void ngx_healthcheck_udp_write(ngx_event_t *event);
void ngx_healthcheck_udp_read(ngx_event_t *event);

#endif
