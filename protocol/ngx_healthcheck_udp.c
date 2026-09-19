#include "ngx_healthcheck_probe.h"

/* 只在已尝试发送、尚未截止的有效轮次采用最小失败白名单。 */
static ngx_int_t
ngx_healthcheck_udp_error(ngx_healthcheck_probe_t *probe,
    const char *source, ngx_err_t error)
{
    uint64_t now;

    if (ngx_healthcheck_now(&now) != NGX_OK) {
        ngx_healthcheck_finish(probe, 1, "monotonic clock unavailable", "clock", 0);
    } else if (now / 1000000 >= probe->deadline) {
        ngx_healthcheck_finish(probe, 1, "deadline without confirmed failure", "timer", 0);
    } else if (probe->attempted && error == NGX_ECONNREFUSED) {
        ngx_healthcheck_finish(probe, 0, "connection refused", source, error);
    } else if (error == NGX_ENOMEM || error == ENOBUFS
               || error == NGX_EMFILE || error == NGX_ENFILE)
    {
        ngx_log_error(NGX_LOG_ERR, probe->pc.log, error,
                      "healthcheck UDP local resource error at %s", source);
        ngx_healthcheck_finish(probe, 1, "local resource error", source, error);
    } else {
        /* 可继续观察时保留原截止，已清除的非白名单错误不提前生成成功。 */
        probe->error = error;
        probe->source = source;
        probe->reason = "unconfirmed socket error";
        ngx_log_debug2(probe->peer->runtime->log_level, probe->pc.log, error,
                       "healthcheck UDP unconfirmed error at %s for %V",
                       source, probe->pc.name);
        return NGX_AGAIN;
    }
    return NGX_OK;
}

/* 发送阶段结束后统一撤销多余写关注，继续等待原轮次的回复或错误。 */
static void
ngx_healthcheck_udp_wait_reply(ngx_healthcheck_probe_t *probe)
{
    ngx_connection_t *connection = probe->pc.connection;

    probe->sending = 0;
    probe->stage = probe->sent ? "wait reply" : "wait after send error";
    if ((ngx_event_flags & NGX_USE_LEVEL_EVENT) && connection->write->active
        && ngx_del_event(connection->write, NGX_WRITE_EVENT, 0) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, connection->log, 0,
                      "healthcheck UDP write event removal failed");
        ngx_healthcheck_finish(probe, 1, "event removal failed", "event", 0);
        return;
    }
    if (ngx_handle_read_event(connection->read, 0) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, connection->log, 0,
                      "healthcheck UDP read event registration failed");
        ngx_healthcheck_finish(probe, 1, "read event registration failed", "event", 0);
    }
}

void
ngx_healthcheck_udp_write(ngx_event_t *event)
{
    ngx_healthcheck_probe_t *probe;
    ngx_connection_t       *connection;
    ngx_str_t              *payload;
    ssize_t                 n;
    ngx_err_t               error;
    ngx_uint_t              retries;

    probe = ngx_healthcheck_io_ready(event);
    if (probe == NULL || !probe->sending) {
        return;
    }
    connection = probe->pc.connection;
    payload = &probe->peer->conf->send;
    for (retries = 0; retries < 8; retries++) {
        probe->attempted = 1;
        n = send(connection->fd, payload->data, payload->len, 0);
        error = n == -1 ? ngx_socket_errno : 0;
        if (n == (ssize_t) payload->len) {
            probe->sent = 1;
            ngx_healthcheck_udp_wait_reply(probe);
            return;
        }
        if (n >= 0) {
            ngx_log_error(NGX_LOG_ERR, connection->log, 0,
                          "healthcheck UDP incomplete datagram send: %z/%uz", n, payload->len);
            ngx_healthcheck_finish(probe, 1, "incomplete datagram send", "send", 0);
            return;
        }
        if (error == NGX_EINTR) {
            if (ngx_healthcheck_io_ready(event) == NULL) {
                return;
            }
            continue;
        }
        if (error != NGX_EAGAIN) {
            ngx_log_error(NGX_LOG_ERR, connection->log, error,
                          "healthcheck UDP send failed for %V", probe->pc.name);
            if (ngx_healthcheck_udp_error(probe, "send", error) == NGX_AGAIN) {
                ngx_healthcheck_udp_wait_reply(probe);
            }
            return;
        }
        break;
    }
    if (retries == 8) {
        ngx_post_event(event, &ngx_posted_next_events);
        return;
    }
    event->ready = 0;
    if (ngx_handle_write_event(event, 0) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, connection->log, 0,
                      "healthcheck UDP write event registration failed");
        ngx_healthcheck_finish(probe, 1, "write event registration failed", "event", 0);
    }
}

void
ngx_healthcheck_udp_read(ngx_event_t *event)
{
    ngx_healthcheck_probe_t *probe;
    ngx_connection_t       *connection;
    int                     socket_error;
    socklen_t               length;
    char                    byte;
    ssize_t                 n;
    ngx_err_t               error;
    ngx_uint_t              retries;

    probe = ngx_healthcheck_io_ready(event);
    if (probe == NULL) {
        return;
    }
    connection = probe->pc.connection;
    /* SO_ERROR 会清除错误，必须在普通 recv 前保存并分类。 */
    for (retries = 0; retries < 8; retries++) {
        length = sizeof(socket_error);
        if (getsockopt(connection->fd, SOL_SOCKET, SO_ERROR, &socket_error,
                       &length) == 0)
        {
            break;
        }
        error = ngx_socket_errno;
        if (error != NGX_EINTR) {
            ngx_log_error(NGX_LOG_ERR, connection->log, error,
                          "healthcheck UDP SO_ERROR query failed");
            ngx_healthcheck_finish(probe, 1, "socket error query failed", "getsockopt", error);
            return;
        }
        if (ngx_healthcheck_io_ready(event) == NULL) {
            return;
        }
    }
    if (retries == 8) {
        ngx_post_event(event, &ngx_posted_next_events);
        return;
    }
    if (socket_error != 0) {
        if (ngx_healthcheck_udp_error(probe, "SO_ERROR", socket_error) != NGX_AGAIN) {
            return;
        }
    }
    for (retries = 0; retries < 8; retries++) {
        n = recv(connection->fd, &byte, 1, 0);
        error = n == -1 ? ngx_socket_errno : 0;
        if (n >= 0) {
            ngx_healthcheck_finish(probe, 1,
                                   n == 0 ? "empty UDP reply" : "UDP reply", "recv", 0);
            return;
        }
        if (error == NGX_EINTR) {
            if (ngx_healthcheck_io_ready(event) == NULL) {
                return;
            }
            continue;
        }
        if (error != NGX_EAGAIN) {
            if (ngx_healthcheck_udp_error(probe, "recv", error) != NGX_AGAIN) {
                return;
            }
            break;
        }
        break;
    }
    if (retries == 8) {
        ngx_post_event(event, &ngx_posted_next_events);
        return;
    }
    event->ready = 0;
    if (ngx_handle_read_event(event, 0) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, connection->log, 0,
                      "healthcheck UDP read event registration failed");
        ngx_healthcheck_finish(probe, 1, "read event registration failed", "event", 0);
    }
}
