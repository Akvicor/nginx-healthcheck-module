#include "ngx_healthcheck_probe.h"

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

void
ngx_healthcheck_tcp_idle(ngx_event_t *event)
{
    ngx_connection_t        *connection = event->data;
    ngx_healthcheck_probe_t *probe = connection->data;
    char                    byte;
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
    n = recv(connection->fd, &byte, 1, MSG_PEEK);
    error = n == -1 ? ngx_socket_errno : 0;
    if (n == -1 && error == NGX_EAGAIN) {
        event->ready = 0;
        if (ngx_handle_read_event(event, 0) == NGX_OK) {
            return;
        }
    }
    ngx_healthcheck_probe_close(probe);
}
