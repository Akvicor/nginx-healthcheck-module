#include "ngx_healthcheck_probe.h"

/* 空闲期累计丢弃上限：连接即发送的 banner 可被吸收，持续发送数据的对端则关闭连接。 */
#define NGX_HEALTHCHECK_IDLE_DISCARD_MAX  4096

ngx_int_t
ngx_healthcheck_tcp_keepalive(ngx_connection_t *connection,
    ngx_upstream_check_srv_conf_t *conf)
{
#if (NGX_HAVE_KEEPALIVE_TUNABLE)
    int  on, idle, interval, count;

    on = 1;
    idle = (int) conf->keepalive_idle;
    interval = (int) conf->keepalive_interval;
    count = (int) conf->keepalive_count;
    /* 三项全部显式设置；未设置 TCP_KEEPINTVL 时内核沿用系统值，健康周期会被拉长。 */
    if (setsockopt(connection->fd, SOL_SOCKET, SO_KEEPALIVE,
                   (const void *) &on, sizeof(on)) == -1
        || setsockopt(connection->fd, IPPROTO_TCP, TCP_KEEPIDLE,
                      (const void *) &idle, sizeof(idle)) == -1
        || setsockopt(connection->fd, IPPROTO_TCP, TCP_KEEPINTVL,
                      (const void *) &interval, sizeof(interval)) == -1
        || setsockopt(connection->fd, IPPROTO_TCP, TCP_KEEPCNT,
                      (const void *) &count, sizeof(count)) == -1)
    {
        ngx_log_error(NGX_LOG_ERR, connection->log, ngx_socket_errno,
                      "healthcheck keepalive setsockopt() failed");
        return NGX_ERROR;
    }
    return NGX_OK;
#else
    return NGX_ERROR;
#endif
}

ngx_int_t
ngx_healthcheck_tcp_peek(ngx_connection_t *connection,
    ngx_healthcheck_probe_t *probe)
{
    char       byte;
    ssize_t    n;
    ngx_err_t  error;

    n = recv(connection->fd, &byte, 1, MSG_PEEK);
    error = n == -1 ? ngx_socket_errno : 0;
    if (n == 1 || (n == -1 && error == NGX_EAGAIN)) {
        return NGX_OK;
    }
    if (probe->peer->runtime->peers->module == NGX_HEALTHCHECK_STREAM) {
        ngx_log_error(NGX_LOG_WARN, connection->log, error,
                      "when peek one byte, recv(): %z, peer: %V",
                      n, &probe->peer->check_peer_addr->name);
    } else {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, connection->log, error,
                       "http check peek failed, recv(): %z, peer: %V",
                       n, &probe->peer->check_peer_addr->name);
    }
    probe->error = error;
    return NGX_ERROR;
}

void
ngx_healthcheck_tcp_event(ngx_event_t *event)
{
    ngx_healthcheck_probe_t *probe;
    ngx_int_t               rc;

    probe = ngx_healthcheck_io_ready(event);
    if (probe == NULL) {
        return;
    }
    rc = ngx_healthcheck_tcp_peek(probe->pc.connection, probe);
    ngx_healthcheck_finish(probe, rc == NGX_OK,
                           rc == NGX_OK ? "tcp peek success" : "tcp closed or error",
                           "recv", rc == NGX_OK ? 0 : probe->error);
}

/*
 * 空闲期读事件：对端数据读取后丢弃并保留连接（水平触发后端因此不会持续报告可读），
 * FIN、RST、keepalive 超时等错误或累计数据超限时关闭连接；关闭不计失败，下一轮
 * 以重新握手结果判定健康。
 */
void
ngx_healthcheck_tcp_idle(ngx_event_t *event)
{
    ngx_connection_t        *connection = event->data;
    ngx_healthcheck_probe_t *probe = connection->data;
    u_char                  buffer[NGX_HEALTHCHECK_IDLE_DISCARD_MAX + 1];
    size_t                  size;
    ssize_t                 n;
    ngx_err_t               error;

    if (probe == NULL || probe->pc.connection != connection) {
        return;
    }
    if (ngx_quit || ngx_exiting || ngx_terminate || probe->peer->runtime->stopping) {
        ngx_healthcheck_runtime_stop(probe->peer->runtime->peers);
        return;
    }
    if (connection->close || event->timedout
        || ngx_healthcheck_probe_valid(probe) == NGX_DECLINED)
    {
        ngx_healthcheck_probe_close(probe);
        return;
    }
    for ( ;; ) {
        /* 多读 1 字节用于判断是否超过累计上限。 */
        size = NGX_HEALTHCHECK_IDLE_DISCARD_MAX - probe->discarded + 1;
        n = recv(connection->fd, buffer, size, 0);
        if (n > 0) {
            probe->discarded += n;
            if (probe->discarded > NGX_HEALTHCHECK_IDLE_DISCARD_MAX) {
                ngx_log_debug1(probe->peer->runtime->log_level, connection->log, 0,
                               "healthcheck idle connection discarded more than %uz "
                               "bytes, closing", (size_t) NGX_HEALTHCHECK_IDLE_DISCARD_MAX);
                break;
            }
            continue;
        }
        error = n == -1 ? ngx_socket_errno : 0;
        if (n == -1 && error == NGX_EINTR) {
            continue;
        }
        if (n == -1 && error == NGX_EAGAIN) {
            event->ready = 0;
            if (ngx_handle_read_event(event, 0) == NGX_OK) {
                return;
            }
        }
        break;
    }
    ngx_healthcheck_probe_close(probe);
}
