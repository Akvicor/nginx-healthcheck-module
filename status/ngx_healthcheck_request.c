#include "ngx_healthcheck_request.h"

/* 批量限制让大列表也能让出事件循环，竞争重试不设置总采集截止。 */
#define NGX_HEALTHCHECK_STATUS_BATCH       256
#define NGX_HEALTHCHECK_STATUS_RETRY_MS    5
#define NGX_HEALTHCHECK_STATUS_YIELD_MS    1
#define NGX_HEALTHCHECK_STATUS_WARN_MS     UINT64_C(1000)
#define NGX_HEALTHCHECK_STATUS_LOG_MS      UINT64_C(5000)

/* 数据和回调归请求池所有；已挂起请求由核心清理链取消独立事件。 */
typedef struct {
    ngx_http_request_t               *request;
    ngx_event_t                      retry;
    ngx_http_cleanup_t              *cleanup;
    ngx_upstream_check_peers_t       *sources[2];
    ngx_healthcheck_snapshot_t        snapshots[2];
    ngx_healthcheck_status_output_pt  output;
    ngx_uint_t                       count, module, flag, argument;
    ngx_uint_t                       waiting_module, waiting_peer;
    uint64_t                         started, next_log;
} ngx_healthcheck_request_t;

static void ngx_healthcheck_request_resume(ngx_event_t *event);

static void
ngx_healthcheck_request_cleanup(void *data)
{
    ngx_healthcheck_request_t *ctx = data;

    if (ctx->retry.timer_set) {
        ngx_del_timer(&ctx->retry);
    }
    if (ctx->retry.posted) {
        ngx_delete_posted_event(&ctx->retry);
    }
    if (ctx->cleanup != NULL) {
        ctx->cleanup->handler = NULL;
    }
}

static void
ngx_healthcheck_request_read(ngx_http_request_t *r)
{
    if (r->connection->error) {
        ngx_http_finalize_request(r, NGX_ERROR);
    } else if (r->discard_body) {
        ngx_http_discarded_request_body_handler(r);
    } else {
        ngx_http_test_reading(r);
    }
}

static ngx_int_t
ngx_healthcheck_request_collect(ngx_healthcheck_request_t *ctx)
{
    ngx_uint_t remaining = NGX_HEALTHCHECK_STATUS_BATCH;
    ngx_int_t  rc;

    while (ctx->module < ctx->count) {
        rc = ngx_healthcheck_snapshot_step(ctx->sources[ctx->module], ctx->flag,
                                          &ctx->snapshots[ctx->module], &remaining);
        if (rc != NGX_OK) {
            return rc;
        }
        ctx->module++;
    }
    return NGX_OK;
}

static ngx_int_t
ngx_healthcheck_request_output(ngx_healthcheck_request_t *ctx)
{
    ngx_healthcheck_status_writer_t writer;

    ngx_healthcheck_status_writer_init(&writer, ctx->request->pool);
    if (ctx->output(&writer, ctx->snapshots, ctx->argument) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    if (ctx->count == 2) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                       "[ngx-healthcheck][status-interface] response length: %uz, "
                       "blocks: %ui", writer.length, writer.blocks);
    }
    return ngx_healthcheck_status_writer_send(ctx->request, &writer);
}

static ngx_int_t
ngx_healthcheck_request_wait(ngx_healthcheck_request_t *ctx, ngx_int_t rc)
{
    ngx_http_request_t          *r = ctx->request;
    ngx_upstream_check_peers_t  *source;
    ngx_upstream_check_peer_t   *peers;
    ngx_pid_t                   holder;
    uint64_t                    now;

    r->read_event_handler = ngx_healthcheck_request_read;
    /* HTTP/2、HTTP/3 的连接事件由协议层管理；主请求的普通 socket 保持断开检测。 */
    if (r == r->main
#if (NGX_HTTP_V2)
        && r->stream == NULL
#endif
#if (NGX_HTTP_V3)
        && r->connection->quic == NULL
#endif
        && ngx_handle_read_event(r->connection->read, 0) != NGX_OK)
    {
        return NGX_ERROR;
    }
    if (rc == NGX_AGAIN && ngx_healthcheck_now(&now) == NGX_OK) {
        now /= 1000000;
        if (ctx->started == 0 || ctx->waiting_module != ctx->module
            || ctx->waiting_peer != ctx->snapshots[ctx->module].next)
        {
            ctx->started = now;
            ctx->waiting_module = ctx->module;
            ctx->waiting_peer = ctx->snapshots[ctx->module].next;
            /* 等待时长属于当前 peer，日志节流覆盖整个请求。 */
            ctx->next_log = ngx_max(ctx->next_log,
                ngx_healthcheck_add_time(now, NGX_HEALTHCHECK_STATUS_WARN_MS));
        }
        if (now >= ctx->next_log) {
            source = ctx->sources[ctx->module];
            peers = source->peers.elts;
            holder = NGX_INVALID_PID;
#if (NGX_HAVE_ATOMIC_OPS)
            holder = (ngx_pid_t) *peers[ctx->snapshots[ctx->module].next].shm->mutex.lock;
#endif
            ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                          "healthcheck status waiting module=%ui peer=%ui "
                          "generation=%ui holder=%P elapsed_ms=%uL",
                          source->module, ctx->snapshots[ctx->module].next,
                          source->generation, holder, now - ctx->started);
            ctx->next_log = ngx_healthcheck_add_time(now, NGX_HEALTHCHECK_STATUS_LOG_MS);
        }
    }
    /* 独立、非 cancelable 的 timer 表示活跃请求，正常排空继续等待它完成。 */
    ngx_add_timer(&ctx->retry, rc == NGX_AGAIN ? NGX_HEALTHCHECK_STATUS_RETRY_MS
                                            : NGX_HEALTHCHECK_STATUS_YIELD_MS);
    return NGX_OK;
}

static void
ngx_healthcheck_request_resume(ngx_event_t *event)
{
    ngx_healthcheck_request_t *ctx = event->data;
    ngx_http_request_t       *r = ctx->request;
    ngx_connection_t         *c = r->connection;
    ngx_int_t                 rc;

    if (c->error || c->close || r->main->terminated) {
        rc = NGX_ERROR;
    } else {
        rc = ngx_healthcheck_request_collect(ctx);
        if (rc == NGX_AGAIN || rc == NGX_BUSY) {
            if (ngx_healthcheck_request_wait(ctx, rc) == NGX_OK) {
                return;
            }
            rc = NGX_ERROR;
        } else if (rc == NGX_OK) {
            rc = ngx_healthcheck_request_output(ctx);
        } else {
            rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }
    ngx_healthcheck_request_cleanup(ctx);
    ngx_http_finalize_request(r, rc);
    /* 完成可能释放请求池，此后只通过连接派发核心排队的结束/父请求事件。 */
    ngx_http_run_posted_requests(c);
}

ngx_int_t
ngx_healthcheck_status_request(ngx_http_request_t *r,
    ngx_upstream_check_peers_t **sources, ngx_uint_t count, ngx_uint_t flag,
    ngx_healthcheck_status_output_pt output, ngx_uint_t argument)
{
    ngx_healthcheck_request_t *ctx;
    ngx_uint_t                i;
    ngx_int_t                 rc;

    if (count == 0 || count > 2 || r->connection->error) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ctx->request = r;
    ctx->count = count;
    ctx->flag = flag;
    ctx->output = output;
    ctx->argument = argument;
    for (i = 0; i < count; i++) {
        ctx->sources[i] = sources[i];
        if (ngx_healthcheck_snapshot_init(r->pool, sources[i], &ctx->snapshots[i]) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }
    rc = ngx_healthcheck_request_collect(ctx);
    if (rc == NGX_OK) {
        return ngx_healthcheck_request_output(ctx);
    }
    if (rc != NGX_AGAIN && rc != NGX_BUSY) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ctx->cleanup = ngx_http_cleanup_add(r, 0);
    if (ctx->cleanup == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ctx->cleanup->handler = ngx_healthcheck_request_cleanup;
    ctx->cleanup->data = ctx;
    ctx->retry.handler = ngx_healthcheck_request_resume;
    ctx->retry.data = ctx;
    ctx->retry.log = r->connection->log;
    if (ngx_healthcheck_request_wait(ctx, rc) != NGX_OK) {
        ngx_healthcheck_request_cleanup(ctx);
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    /* NGX_DONE 在 content phase 消耗当前引用，额外引用由最终完成路径消耗。 */
    r->main->count++;
    return NGX_DONE;
}
