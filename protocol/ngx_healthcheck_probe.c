#include "ngx_healthcheck_probe.h"

/* 非 level-triggered 后端保留写 handler 槽，实际 I/O 由读监听与周期检查驱动。 */
static void
ngx_healthcheck_idle_write(ngx_event_t *event)
{
}

void
ngx_healthcheck_probe_close(ngx_healthcheck_probe_t *probe)
{
    ngx_connection_t *connection = probe->pc.connection;
    struct linger    linger;

    if (connection == NULL) {
        return;
    }
    if (connection->read->timer_set) {
        ngx_del_timer(connection->read);
    }
    if (connection->write->timer_set) {
        ngx_del_timer(connection->write);
    }
    if (connection->read->posted) {
        ngx_delete_posted_event(connection->read);
    }
    if (connection->write->posted) {
        ngx_delete_posted_event(connection->write);
    }
    ngx_reusable_connection(connection, 0);
    linger.l_onoff = 1;
    linger.l_linger = 0;
    (void) setsockopt(connection->fd, SOL_SOCKET, SO_LINGER, &linger,
                      sizeof(linger));
    probe->pc.connection = NULL;
    connection->data = NULL;
    ngx_close_connection(connection);
}

void
ngx_healthcheck_probe_cleanup(ngx_healthcheck_probe_t *probe, ngx_uint_t reusable)
{
    ngx_connection_t             *connection = probe->pc.connection;
    ngx_upstream_check_srv_conf_t *conf = probe->peer->conf;

    if (probe->timeout.timer_set) {
        ngx_del_timer(&probe->timeout);
    }
    if (probe->timeout.posted) {
        ngx_delete_posted_event(&probe->timeout);
    }
    if (connection == NULL) {
        return;
    }
    if (!reusable || !conf->tcp_reuse
        || conf->check_type_conf->type != NGX_CHECK_TYPE_TCP
        || connection->error || connection->timedout
        || (conf->check_keepalive_requests != 0
            && connection->requests >= conf->check_keepalive_requests))
    {
        ngx_healthcheck_probe_close(probe);
        return;
    }
    if (connection->read->timer_set) {
        ngx_del_timer(connection->read);
    }
    if (connection->write->timer_set) {
        ngx_del_timer(connection->write);
    }
    if ((ngx_event_flags & NGX_USE_LEVEL_EVENT) && connection->write->active) {
        if (ngx_del_event(connection->write, NGX_WRITE_EVENT, 0) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, connection->log, 0,
                          "healthcheck idle write event removal failed");
            ngx_healthcheck_probe_close(probe);
            return;
        }
    }
    if (connection->write->posted) {
        ngx_delete_posted_event(connection->write);
    }
    connection->write->handler = ngx_healthcheck_idle_write;
    connection->read->handler = ngx_healthcheck_tcp_idle;
    connection->idle = 1;
    ngx_reusable_connection(connection, 1);
    if (ngx_handle_read_event(connection->read, 0) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, connection->log, 0,
                      "healthcheck idle read event registration failed");
        ngx_healthcheck_probe_close(probe);
    } else if (connection->read->ready) {
        ngx_healthcheck_tcp_idle(connection->read);
    }
}

ngx_healthcheck_probe_t *
ngx_healthcheck_io_ready(ngx_event_t *event)
{
    ngx_connection_t         *connection = event->data;
    ngx_healthcheck_probe_t   *probe;
    ngx_int_t                 rc;
    uint64_t                  now;

    probe = connection->data;
    if (probe == NULL || probe->pc.connection != connection
        || !probe->active || probe->pending)
    {
        return NULL;
    }
    if (ngx_quit || ngx_exiting || ngx_terminate || probe->peer->runtime->stopping) {
        ngx_healthcheck_runtime_stop(probe->peer->runtime->peers);
        return NULL;
    }
    rc = ngx_healthcheck_probe_valid(probe);
    if (rc == NGX_DECLINED) {
        ngx_healthcheck_probe_cleanup(probe, 0);
        probe->active = 0;
        return NULL;
    }
    if (rc != NGX_OK || ngx_healthcheck_now(&now) != NGX_OK) {
        return NULL;
    }
    if (now / 1000000 >= probe->deadline) {
        ngx_healthcheck_timeout(&probe->timeout);
        return NULL;
    }
    return probe;
}

void
ngx_healthcheck_timeout(ngx_event_t *event)
{
    ngx_healthcheck_probe_t    *probe = event->data;
    ngx_healthcheck_runtime_t  *runtime = probe->peer->runtime;
    uint64_t                   now;

    if (!probe->active || probe->pending) {
        return;
    }
    if (ngx_quit || ngx_exiting || ngx_terminate || runtime->stopping) {
        ngx_healthcheck_runtime_stop(runtime->peers);
        return;
    }
    if (ngx_healthcheck_probe_valid(probe) == NGX_DECLINED) {
        probe->active = 0;
        ngx_healthcheck_probe_cleanup(probe, 0);
        return;
    }
    if (ngx_healthcheck_now(&now) != NGX_OK) {
        ngx_healthcheck_timer(event, 1);
        return;
    }
    if (now / 1000000 < probe->deadline) {
        ngx_healthcheck_timer(event, probe->deadline - now / 1000000);
        return;
    }
    if (probe->peer->conf->check_type_conf->type == NGX_CHECK_TYPE_UDP) {
        ngx_healthcheck_finish(probe, 1, "deadline without confirmed failure",
                               "timer", 0);
    } else {
        ngx_log_error((runtime->peers->module == NGX_HEALTHCHECK_HTTP
                       ? NGX_LOG_ERR : NGX_LOG_WARN), runtime->cycle->log, 0,
                      "[%s] tcp check time out with peer: %V",
                      runtime->name, &probe->peer->check_peer_addr->name);
        ngx_healthcheck_finish(probe, 0, "tcp timeout", "timer", 0);
    }
}

void
ngx_healthcheck_probe_start(ngx_healthcheck_probe_t *probe)
{
    ngx_upstream_check_peer_t  *peer = probe->peer;
    ngx_connection_t          *connection;
    ngx_int_t                  rc;
    ngx_uint_t                 udp;
    uint64_t                   now;

    udp = peer->conf->check_type_conf->type == NGX_CHECK_TYPE_UDP;
    rc = ngx_healthcheck_probe_valid(probe);
    if (rc == NGX_DECLINED) {
        probe->active = 0;
        ngx_healthcheck_probe_cleanup(probe, 0);
        return;
    }
    if (rc != NGX_OK) {
        /* 还未执行网络动作，巡检可在资格恢复可读后继续准备。 */
        return;
    }
    if (ngx_healthcheck_now(&now) != NGX_OK) {
        return;
    }
    if (now / 1000000 >= probe->deadline) {
        ngx_healthcheck_timeout(&probe->timeout);
        return;
    }
    connection = probe->pc.connection;
    if (connection != NULL) {
        if (ngx_healthcheck_tcp_peek(connection, probe) == NGX_OK) {
            connection->idle = 0;
            connection->close = 0;
            ngx_reusable_connection(connection, 0);
            rc = NGX_OK;
            goto connected;
        }
        ngx_healthcheck_probe_close(probe);
    }
    ngx_memzero(&probe->pc, sizeof(probe->pc));
    probe->pc.sockaddr = peer->check_peer_addr->sockaddr;
    probe->pc.socklen = peer->check_peer_addr->socklen;
    probe->pc.name = &peer->check_peer_addr->name;
    probe->pc.type = udp ? SOCK_DGRAM : SOCK_STREAM;
    probe->pc.get = ngx_event_get_peer;
    probe->pc.log = peer->runtime->cycle->log;
    probe->pc.log_error = NGX_ERROR_ERR;
    rc = ngx_event_connect_peer(&probe->pc);
    if (rc == NGX_ERROR || rc == NGX_DECLINED || probe->pc.connection == NULL) {
        ngx_log_error(NGX_LOG_WARN, peer->runtime->cycle->log, 0,
                      "[%s] check connect failed, peer: %V, rc: %i",
                      peer->runtime->name, &peer->check_peer_addr->name, rc);
        ngx_healthcheck_finish(probe, udp, "socket preparation failed",
                               "ngx_event_connect_peer", 0);
        return;
    }
    connection = probe->pc.connection;
    connection->data = probe;
    connection->log = probe->pc.log;
    connection->sendfile = 0;
    connection->read->log = connection->log;
    connection->write->log = connection->log;
    connection->start_time = ngx_current_msec;

connected:
    connection->read->cancelable = 1;
    connection->write->cancelable = 1;
    connection->write->handler = udp ? ngx_healthcheck_udp_write
                                     : ngx_healthcheck_tcp_event;
    connection->read->handler = udp ? ngx_healthcheck_udp_read
                                    : ngx_healthcheck_tcp_event;
    probe->timeout.handler = ngx_healthcheck_timeout;
    probe->timeout.data = probe;
    probe->timeout.log = connection->log;
    probe->timeout.timedout = 0;
    probe->prepared = 1;
    if (ngx_healthcheck_now(&now) != NGX_OK) {
        ngx_healthcheck_finish(probe, udp, "monotonic clock unavailable", "clock", 0);
        return;
    }
    /* TCP 保留准备完成后起算 timeout；UDP 使用领取时固定的整轮截止。 */
    if (!udp) {
        probe->deadline = ngx_healthcheck_add_time(now / 1000000,
                                                    peer->conf->check_timeout);
    }
    probe->stage = udp ? "send" : "tcp check";
    (void) ngx_healthcheck_probe_waiting(probe);
    ngx_healthcheck_timer(&probe->timeout,
        now / 1000000 < probe->deadline ? probe->deadline - now / 1000000 : 0);
    if (rc == NGX_OK) {
        connection->write->handler(connection->write);
    }
}

void
ngx_healthcheck_probe_resume(ngx_healthcheck_probe_t *probe)
{
    ngx_connection_t *connection;
    uint64_t          now;

    if (!probe->active) {
        return;
    }
    if (!probe->prepared) {
        ngx_healthcheck_probe_start(probe);
        return;
    }
    (void) ngx_healthcheck_probe_waiting(probe);
    if (ngx_healthcheck_now(&now) == NGX_OK && now / 1000000 >= probe->deadline) {
        ngx_healthcheck_timeout(&probe->timeout);
        return;
    }
    connection = probe->pc.connection;
    if (connection != NULL && connection->read->ready) {
        connection->read->handler(connection->read);
    }
    connection = probe->pc.connection;
    if (probe->active && connection != NULL && connection->write->ready) {
        connection->write->handler(connection->write);
    }
}
