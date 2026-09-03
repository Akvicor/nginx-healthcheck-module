/*
 * Copyright (C) 2017- Changxun Zhou(changxunzhou@qq.com)
 * desc: http upstream server health check.
 * note: base on https://github.com/yaoweibin/nginx_upstream_check_module
 */

/* Thanks:
 * Copyright (C) 2010-2014 Weibin Yao (yaoweibin@gmail.com)
 * Copyright (C) 2010-2014 Alibaba Group Holding Limited
 */

#include "common.h.in"
#include "ngx_http_upstream_check_module.h"
#include "ngx_healthcheck_status_writer.h"

#define LOG_LEVEL NGX_LOG_DEBUG_HTTP
#define MODULE_NAME "[ngx_healthcheck:http]"

typedef struct {
    ngx_buf_t                                send;
    ngx_buf_t                                recv;

    ngx_uint_t                               state;
} ngx_http_upstream_check_ctx_t;




#define NGX_HTTP_CHECK_CONNECT_DONE          0x0001
#define NGX_HTTP_CHECK_SEND_DONE             0x0002
#define NGX_HTTP_CHECK_RECV_DONE             0x0004
#define NGX_HTTP_CHECK_ALL_DONE              0x0008


#define NGX_HTTP_CHECK_TCP                   0x0001
#define NGX_HTTP_CHECK_UDP                   0x0004


typedef ngx_int_t (*ngx_http_upstream_check_status_format_pt) (
    ngx_healthcheck_status_writer_t *writer,
    ngx_upstream_check_peers_t *peers, ngx_uint_t flag);

typedef struct {
    ngx_str_t                                format;
    ngx_str_t                                content_type;

    ngx_http_upstream_check_status_format_pt output;
} ngx_check_status_conf_t;


#define NGX_CHECK_STATUS_DOWN                0x0001
#define NGX_CHECK_STATUS_UP                  0x0002

typedef struct {
    ngx_check_status_conf_t                 *format;
    ngx_flag_t                               flag;
} ngx_http_upstream_check_status_ctx_t;


typedef ngx_int_t (*ngx_http_upstream_check_status_command_pt)
    (ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value);

typedef struct {
    ngx_str_t                                 name;
    ngx_http_upstream_check_status_command_pt handler;
} ngx_check_status_command_t;


typedef struct {
    ngx_uint_t                               check_shm_size;
    ngx_upstream_check_peers_t         *peers;
} ngx_http_upstream_check_main_conf_t;


typedef struct {
    ngx_check_status_conf_t                 *format;
} ngx_http_upstream_check_loc_conf_t;


static ngx_int_t ngx_http_upstream_check_add_timers(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_upstream_check_peer_owner(
    ngx_upstream_check_peer_t *peer, ngx_uint_t worker_processes);

static ngx_int_t ngx_http_upstream_check_peek_one_byte(ngx_connection_t *c);

static void ngx_http_upstream_check_begin_handler(ngx_event_t *event);
static void ngx_http_upstream_check_connect_handler(ngx_event_t *event);

static void ngx_http_upstream_check_peek_handler(ngx_event_t *event);

static void ngx_http_upstream_check_send_handler(ngx_event_t *event);

static void ngx_http_upstream_check_dummy_handler(ngx_event_t *event);
static void ngx_http_upstream_check_reuse_close_handler(ngx_event_t *event);
static void ngx_http_upstream_check_close_connection(
    ngx_upstream_check_peer_t *peer, ngx_connection_t *c, const char *reason);

static ngx_int_t ngx_http_upstream_check_udp_init(
    ngx_upstream_check_peer_t *peer);
static void ngx_http_upstream_check_udp_reinit(
    ngx_upstream_check_peer_t *peer);

static void ngx_http_upstream_check_status_update(
    ngx_upstream_check_peer_t *peer,
    ngx_int_t result);

static void ngx_http_upstream_check_clean_event(
    ngx_upstream_check_peer_t *peer);

static void ngx_http_upstream_check_timeout_handler(ngx_event_t *event);
static void ngx_http_upstream_check_finish_handler(ngx_event_t *event);

static ngx_int_t ngx_http_upstream_check_need_exit();
static void ngx_http_upstream_check_clear_all_events();

static ngx_int_t ngx_http_upstream_check_status_handler(
    ngx_http_request_t *r);

static void ngx_http_upstream_check_status_parse_args(ngx_http_request_t *r,
    ngx_http_upstream_check_status_ctx_t *ctx);

static ngx_int_t ngx_http_upstream_check_status_command_format(
    ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value);
static ngx_int_t ngx_http_upstream_check_status_command_status(
    ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value);

static ngx_int_t ngx_http_upstream_check_status_html_format(
    ngx_healthcheck_status_writer_t *writer,
    ngx_upstream_check_peers_t *peers, ngx_uint_t flag);
static ngx_int_t ngx_http_upstream_check_status_csv_format(
    ngx_healthcheck_status_writer_t *writer,
    ngx_upstream_check_peers_t *peers, ngx_uint_t flag);
static ngx_int_t ngx_http_upstream_check_status_json_format(
    ngx_healthcheck_status_writer_t *writer,
    ngx_upstream_check_peers_t *peers, ngx_uint_t flag);

static ngx_int_t ngx_http_upstream_check_addr_change_port(ngx_pool_t *pool,
    ngx_addr_t *dst, ngx_addr_t *src, ngx_uint_t port);

static ngx_check_conf_t *ngx_http_get_check_type_conf(ngx_str_t *str);

static char *ngx_http_upstream_check(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_upstream_check_keepalive_requests(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

static char *ngx_http_upstream_check_shm_size(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

static ngx_check_status_conf_t *ngx_http_get_check_status_format_conf(
    ngx_str_t *str);
static char *ngx_http_upstream_check_status(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

static void *ngx_http_upstream_check_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_check_init_main_conf(ngx_conf_t *cf,
    void *conf);

static void *ngx_http_upstream_check_create_srv_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_check_init_srv_conf(ngx_conf_t *cf, void *conf);

static void *ngx_http_upstream_check_create_loc_conf(ngx_conf_t *cf);
static char * ngx_http_upstream_check_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

#define SHM_NAME_LEN 256

static char *ngx_http_upstream_check_init_shm(ngx_conf_t *cf, void *conf);

static ngx_int_t ngx_http_upstream_check_get_shm_name(ngx_str_t *shm_name,
    ngx_pool_t *pool, ngx_uint_t generation);
static ngx_int_t ngx_http_upstream_check_init_shm_peer(
    ngx_upstream_check_peer_shm_t *peer_shm,
    ngx_upstream_check_peer_shm_t *opeer_shm,
    ngx_uint_t init_down, ngx_pool_t *pool, ngx_str_t *peer_name);

static ngx_int_t ngx_http_upstream_check_init_shm_zone(
    ngx_shm_zone_t *shm_zone, void *data);


static ngx_int_t ngx_http_upstream_check_init_process(ngx_cycle_t *cycle);


static ngx_command_t  ngx_http_upstream_check_commands[] = {

    { ngx_string("check"),
      NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
      ngx_http_upstream_check,
      0,
      0,
      NULL },

    { ngx_string("check_keepalive_requests"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_check_keepalive_requests,
      0,
      0,
      NULL },

    { ngx_string("check_shm_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_check_shm_size,
      0,
      0,
      NULL },

    { ngx_string("check_status"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1|NGX_CONF_NOARGS,
      ngx_http_upstream_check_status,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_check_module_ctx = {
    NULL,                                    /* preconfiguration */
    NULL,                                    /* postconfiguration */

    ngx_http_upstream_check_create_main_conf,/* create main configuration */
    ngx_http_upstream_check_init_main_conf,  /* init main configuration */

    ngx_http_upstream_check_create_srv_conf, /* create server configuration */
    NULL,                                    /* merge server configuration */

    ngx_http_upstream_check_create_loc_conf, /* create location configuration */
    ngx_http_upstream_check_merge_loc_conf   /* merge location configuration */
};


ngx_module_t  ngx_http_upstream_check_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_check_module_ctx,   /* module context */
    ngx_http_upstream_check_commands,      /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_http_upstream_check_init_process,  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_check_conf_t  ngx_check_types[] = {

    { NGX_HTTP_CHECK_TCP,
      ngx_string("tcp"),
      ngx_null_string,
      0,
      ngx_http_upstream_check_peek_handler,
      ngx_http_upstream_check_peek_handler,
      NULL,
      NULL,
      NULL,
      1 },

    { NGX_HTTP_CHECK_UDP,
      ngx_string("udp"),
      ngx_string("NGX_UDP_CHECKER"),
      0,
      ngx_http_upstream_check_send_handler,
      ngx_http_upstream_check_peek_handler,
      ngx_http_upstream_check_udp_init,
      NULL,
      ngx_http_upstream_check_udp_reinit,
      0 },

    { 0,
      ngx_null_string,
      ngx_null_string,
      0,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      0 }
};


static ngx_check_status_conf_t  ngx_check_status_formats[] = {

    { ngx_string("html"),
      ngx_string("text/html"),
      ngx_http_upstream_check_status_html_format },

    { ngx_string("csv"),
      ngx_string("text/plain"),
      ngx_http_upstream_check_status_csv_format },

    { ngx_string("json"),
      ngx_string("application/json"), /* RFC 4627 */
      ngx_http_upstream_check_status_json_format },

    { ngx_null_string, ngx_null_string, NULL }
};


static ngx_check_status_command_t ngx_check_status_commands[] =  {

    { ngx_string("format"),
      ngx_http_upstream_check_status_command_format },

    { ngx_string("status"),
      ngx_http_upstream_check_status_command_status },

    { ngx_null_string, NULL }
};


static ngx_uint_t ngx_http_upstream_check_shm_generation = 0;
ngx_upstream_check_peers_t *http_peers_ctx = NULL;


ngx_uint_t
ngx_http_upstream_check_add_peer(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us, ngx_addr_t *peer_addr)
{
    ngx_upstream_check_peer_t       *peer;
    ngx_upstream_check_peers_t      *peers;
    ngx_upstream_check_srv_conf_t   *ucscf;
    ngx_http_upstream_check_main_conf_t  *ucmcf;

    if (us->srv_conf == NULL) {
        return NGX_ERROR;
    }

    ucscf = ngx_http_conf_upstream_srv_conf(us, ngx_http_upstream_check_module);

    if(ucscf->check_interval == 0) {
        return NGX_ERROR;
    }

    ucmcf = ngx_http_conf_get_module_main_conf(cf,
                                               ngx_http_upstream_check_module);
    peers = ucmcf->peers;

    peer = ngx_array_push(&peers->peers);
    if (peer == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(peer, sizeof(ngx_upstream_check_peer_t));

    peer->index = peers->peers.nelts - 1;
    peer->conf = ucscf;
    peer->upstream_name = &us->host;
    peer->peer_addr = peer_addr;

    if (ucscf->port) {
        peer->check_peer_addr = ngx_pcalloc(cf->pool, sizeof(ngx_addr_t));
        if (peer->check_peer_addr == NULL) {
            return NGX_ERROR;
        }

        if (ngx_http_upstream_check_addr_change_port(cf->pool,
                peer->check_peer_addr, peer_addr, ucscf->port)
            != NGX_OK) {

            return NGX_ERROR;
        }

    } else {
        peer->check_peer_addr = peer->peer_addr;
    }

    return peer->index;
}


static ngx_int_t
ngx_http_upstream_check_addr_change_port(ngx_pool_t *pool, ngx_addr_t *dst,
    ngx_addr_t *src, ngx_uint_t port)
{
    size_t                len;
    u_char               *p;
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif

    dst->socklen = src->socklen;
    dst->sockaddr = ngx_palloc(pool, dst->socklen);
    if (dst->sockaddr == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(dst->sockaddr, src->sockaddr, dst->socklen);

    switch (dst->sockaddr->sa_family) {

    case AF_INET:

        len = NGX_INET_ADDRSTRLEN + sizeof(":65535") - 1;
        sin = (struct sockaddr_in *) dst->sockaddr;
        sin->sin_port = htons(port);

        break;

#if (NGX_HAVE_INET6)
    case AF_INET6:

        len = NGX_INET6_ADDRSTRLEN + sizeof(":65535") - 1;
        sin6 = (struct sockaddr_in6 *) dst->sockaddr;
        sin6->sin6_port = htons(port);

        break;
#endif

    default:
        return NGX_ERROR;
    }

    p = ngx_pnalloc(pool, len);
    if (p == NULL) {
        return NGX_ERROR;
    }

#if (nginx_version >= 1005012)
    len = ngx_sock_ntop(dst->sockaddr, dst->socklen, p, len, 1);
#else
    len = ngx_sock_ntop(dst->sockaddr, p, len, 1);
#endif

    dst->name.len = len;
    dst->name.data = p;

    return NGX_OK;
}


ngx_uint_t
ngx_http_upstream_check_peer_down(ngx_uint_t index)
{
    ngx_upstream_check_peer_t  *peer;

    if (http_peers_ctx == NULL || index >= http_peers_ctx->peers.nelts) {
        return 0;
    }

    peer = http_peers_ctx->peers.elts;

    if (peer[index].shm == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, MODULE_NAME
                      "http check peer shm is NULL, index: %ui", index);
        return 0;
    }

    return (peer[index].shm->down);
}


/* TODO: this interface can count each peer's busyness */
void
ngx_http_upstream_check_get_peer(ngx_uint_t index)
{
    ngx_upstream_check_peer_t  *peer;

    if (http_peers_ctx == NULL || index >= http_peers_ctx->peers.nelts) {
        return;
    }

    peer = http_peers_ctx->peers.elts;

    if (peer[index].shm == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, MODULE_NAME
                      "http check get peer shm is NULL, index: %ui", index);
        return;
    }

    ngx_shmtx_lock(&peer[index].shm->mutex);

    peer[index].shm->busyness++;
    peer[index].shm->access_count++;

    ngx_shmtx_unlock(&peer[index].shm->mutex);
}


void
ngx_http_upstream_check_free_peer(ngx_uint_t index)
{
    ngx_upstream_check_peer_t  *peer;

    if (http_peers_ctx == NULL || index >= http_peers_ctx->peers.nelts) {
        return;
    }

    peer = http_peers_ctx->peers.elts;

    if (peer[index].shm == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, MODULE_NAME
                      "http check free peer shm is NULL, index: %ui", index);
        return;
    }

    ngx_shmtx_lock(&peer[index].shm->mutex);

    if (peer[index].shm->busyness > 0) {
        peer[index].shm->busyness--;
    }

    ngx_shmtx_unlock(&peer[index].shm->mutex);
}


static ngx_int_t
ngx_http_upstream_check_add_timers(ngx_cycle_t *cycle)
{
    ngx_uint_t                           i, context_count, owned, skipped;
    ngx_uint_t                           worker_processes;
    ngx_msec_t                           t, delay;
    ngx_check_conf_t                    *cf;
    ngx_core_conf_t                     *ccf;
    ngx_upstream_check_peer_t      *peer;
    ngx_upstream_check_peers_t     *peers;
    ngx_upstream_check_srv_conf_t  *ucscf;
    ngx_upstream_check_peer_shm_t  *peer_shm;
    ngx_upstream_check_peers_shm_t *peers_shm;

    peers = http_peers_ctx;
    if (peers == NULL) {
        return NGX_OK;
    }

    peers_shm = peers->peers_shm;
    if (peers_shm == NULL) {
        return NGX_OK;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, cycle->log, 0, MODULE_NAME
                   "http check upstream init_process, shm_name: %V, "
                   "peer number: %ud",
                   &peers->check_shm_name,
                   peers->peers.nelts);

    srandom(ngx_pid);
    owned = 0;
    context_count = 0;
    skipped = 0;

    ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
    worker_processes = (ccf && ccf->worker_processes > 0)
                       ? (ngx_uint_t) ccf->worker_processes : 1;

    peer = peers->peers.elts;
    peer_shm = peers_shm->peers;

    for (i = 0; i < peers->peers.nelts; i++) {
        peer[i].shm = &peer_shm[i];

        peer[i].check_ev.handler = ngx_http_upstream_check_begin_handler;
        peer[i].check_ev.log = cycle->log;
        peer[i].check_ev.data = &peer[i];
        peer[i].check_ev.timer_set = 0;

        peer[i].check_timeout_ev.handler =
            ngx_http_upstream_check_timeout_handler;
        peer[i].check_timeout_ev.log = cycle->log;
        peer[i].check_timeout_ev.data = &peer[i];
        peer[i].check_timeout_ev.timer_set = 0;

        ucscf = peer[i].conf;
        cf = ucscf->check_type_conf;
        peer[i].check_owner = ngx_http_upstream_check_peer_owner(&peer[i],
                                                                 worker_processes);

        if (peer[i].check_owner && cf->type == NGX_HTTP_CHECK_UDP) {
            peer[i].check_data = ngx_pcalloc(cycle->pool,
                                      sizeof(ngx_http_upstream_check_ctx_t));
            if (peer[i].check_data == NULL) {
                return NGX_ERROR;
            }
            if (cf->init == NULL || cf->init(&peer[i]) != NGX_OK) {
                return NGX_ERROR;
            }
            context_count++;
        }

        peer[i].send_handler = cf->send_handler;
        peer[i].recv_handler = cf->recv_handler;

        peer[i].init = cf->init;
        peer[i].parse = cf->parse;
        peer[i].reinit = cf->reinit;

        /*
         * We add a random start time here, since we don't want to trigger
         * the check events too close to each other at the beginning.
         */
        delay = ucscf->check_interval > 1000 ? ucscf->check_interval : 1000;
        t = ngx_random() % delay;

        if (peer[i].check_owner) {
            owned++;
            ngx_shmtx_lock(&peer[i].shm->mutex);
            peer[i].shm->owner = NGX_INVALID_PID;
            ngx_shmtx_unlock(&peer[i].shm->mutex);
            ngx_add_timer(&peer[i].check_ev, t);
        } else {
            skipped++;
            ngx_log_debug4(NGX_LOG_DEBUG_HTTP, cycle->log, 0, MODULE_NAME
                           "http check skip non-owner peer timer, index: %ui, "
                           "worker: %ui, worker_processes: %ui, owner: %ui",
                           peer[i].index, ngx_worker, worker_processes,
                           peer[i].index % worker_processes);
        }
    }

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, cycle->log, 0, MODULE_NAME
                   "http check worker peer assignment, worker: %ui, "
                   "worker_processes: %ui, owned: %ui, skipped: %ui",
                   ngx_worker, worker_processes, owned, skipped);

    ngx_log_debug6(NGX_LOG_DEBUG_HTTP, cycle->log, 0, MODULE_NAME
                   "http check worker memory profile, worker: %ui, "
                    "peers: %ui, peer_bytes: %uz, contexts: %ui, "
                    "context_bytes: %uz, active_timers: %ui",
                   ngx_worker, peers->peers.nelts,
                   peers->peers.nelts * sizeof(ngx_upstream_check_peer_t),
                    context_count,
                    (size_t) context_count
                    * sizeof(ngx_http_upstream_check_ctx_t), owned);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_peer_owner(ngx_upstream_check_peer_t *peer,
    ngx_uint_t worker_processes)
{
    if (worker_processes <= 1 || ngx_process == NGX_PROCESS_SINGLE)
    {
        return 1;
    }

    return (peer->index % worker_processes == ngx_worker);
}


static void
ngx_http_upstream_check_begin_handler(ngx_event_t *event)
{
    ngx_msec_t                           interval;
    ngx_upstream_check_peer_t      *peer;
    ngx_upstream_check_peers_t     *peers;
    ngx_upstream_check_srv_conf_t  *ucscf;
    ngx_upstream_check_peers_shm_t *peers_shm;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    peers = http_peers_ctx;
    if (peers == NULL) {
        return;
    }

    peers_shm = peers->peers_shm;
    if (peers_shm == NULL) {
        return;
    }

    peer = event->data;
    ucscf = peer->conf;

    if (!peer->check_owner) {
        return;
    }

    ngx_add_timer(event, ucscf->check_interval / 2);

    /* This process is processing this peer now. */
    if ((peer->shm->owner == ngx_pid  ||
        peer->check_timeout_ev.timer_set)) {
        return;
    }

    interval = ngx_current_msec - peer->shm->access_time;
    ngx_log_debug5(NGX_LOG_DEBUG_HTTP, event->log, 0, MODULE_NAME
                   "http check begin handler index: %ui, owner: %P, "
                   "ngx_pid: %P, interval: %M, check_interval: %M",
                   peer->index, peer->shm->owner,
                   ngx_pid, interval,
                   ucscf->check_interval);

    ngx_shmtx_lock(&peer->shm->mutex);

    if (peers_shm->generation != ngx_http_upstream_check_shm_generation) {
        ngx_shmtx_unlock(&peer->shm->mutex);
        return;
    }

    if ((interval >= ucscf->check_interval)
         && (peer->shm->owner == NGX_INVALID_PID))
    {
        peer->shm->owner = ngx_pid;

    } else if (interval >= (ucscf->check_interval << 4)) {

        /*
         * If the check peer has been untouched for 2^4 times of
         * the check interval, activate the current timer.
         * Sometimes, the checking process may disappear
         * in some circumstances, and the clean event will never
         * be triggered.
         */
        peer->shm->owner = ngx_pid;
        peer->shm->access_time = ngx_current_msec;
    }

    ngx_shmtx_unlock(&peer->shm->mutex);

    if (peer->shm->owner == ngx_pid) {
        ngx_http_upstream_check_connect_handler(event);
    }
}


static void
ngx_http_upstream_check_connect_handler(ngx_event_t *event)
{
    ngx_int_t                            rc;
    ngx_connection_t                    *c;
    ngx_upstream_check_peer_t      *peer;
    ngx_upstream_check_srv_conf_t  *ucscf;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    peer = event->data;
    ucscf = peer->conf;

    // 记录检查开始时间
    peer->check_start_time = ngx_current_msec;

    if (peer->pc.connection != NULL) {
        c = peer->pc.connection;
        if ((rc = ngx_http_upstream_check_peek_one_byte(c)) == NGX_OK) {
            c->idle = 0;
            c->close = 0;
            ngx_reusable_connection(c, 0);
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, event->log, 0, MODULE_NAME
                           "http check reuse connection, index: %ui, fd: %d",
                           peer->index, c->fd);
            goto upstream_check_connect_done;
        } else {
            ngx_http_upstream_check_close_connection(peer, c, "reuse stale");
        }
    }
    ngx_memzero(&peer->pc, sizeof(ngx_peer_connection_t));

    peer->pc.sockaddr = peer->check_peer_addr->sockaddr;
    peer->pc.socklen = peer->check_peer_addr->socklen;
    peer->pc.name = &peer->check_peer_addr->name;
    peer->pc.type = (ucscf->check_type_conf->type == NGX_HTTP_CHECK_UDP) ? SOCK_DGRAM : SOCK_STREAM;

    peer->pc.get = ngx_event_get_peer;
    peer->pc.log = event->log;
    peer->pc.log_error = NGX_ERROR_ERR;

    peer->pc.cached = 0;
    peer->pc.connection = NULL;

    rc = ngx_event_connect_peer(&peer->pc);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        ngx_log_error(NGX_LOG_WARN, event->log, 0, MODULE_NAME
                      "http check connect failed, index: %ui, peer: %V, rc: %i",
                      peer->index, &peer->check_peer_addr->name, rc);
        ngx_http_upstream_check_status_update(peer, 0);
        ngx_http_upstream_check_clean_event(peer);
        return;
    }

    /* NGX_OK or NGX_AGAIN */
    c = peer->pc.connection;
    c->data = peer;
    c->log = peer->pc.log;
    c->sendfile = 0;
    c->read->log = c->log;
    c->write->log = c->log;
    c->start_time = ngx_current_msec;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, event->log, 0, MODULE_NAME
                   "http check created connection, index: %ui, fd: %d",
                   peer->index, c->fd);

upstream_check_connect_done:
    peer->state = NGX_HTTP_CHECK_CONNECT_DONE;

    c->write->handler = peer->send_handler;
    c->read->handler = peer->recv_handler;

    ngx_add_timer(&peer->check_timeout_ev, ucscf->check_timeout);

    /* The kqueue's loop interface needs it. */
    if (rc == NGX_OK) {
        c->write->handler(c->write);
    }
}

static ngx_int_t
ngx_http_upstream_check_peek_one_byte(ngx_connection_t *c)
{
    char                            buf[1];
    ngx_int_t                       n;
    ngx_err_t                       err;

    n = recv(c->fd, buf, 1, MSG_PEEK);
    err = ngx_socket_errno;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, err,
                   "http check upstream recv(): %i, fd: %d",
                   n, c->fd);

    if (n == 1 || (n == -1 && err == NGX_EAGAIN)) {
        return NGX_OK;
    } else {
        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, err, MODULE_NAME
                       "http check peek failed, recv(): %i, fd: %d, err: %d",
                       n, c->fd, err);
        return NGX_ERROR;
    }
}

// HTTP模块的延迟更新函数
static void
ngx_http_upstream_check_update_delay(ngx_upstream_check_peer_t *peer)
{
    ngx_msec_t                          delay;
    ngx_healthcheck_delay_stats_t       stats;
    ngx_upstream_check_peer_shm_t      *peer_shm;

    peer_shm = peer->shm;

    // 计算本次检查的延迟
    delay = ngx_current_msec - peer->check_start_time;

    ngx_shmtx_lock(&peer_shm->mutex);

    stats.last = peer_shm->last_check_delay;
    stats.avg = peer_shm->avg_check_delay;
    stats.min = peer_shm->min_check_delay;
    stats.max = peer_shm->max_check_delay;
    stats.delay_total = peer_shm->delay_total;
    stats.delay_sample_count = peer_shm->delay_sample_count;

    if (ngx_healthcheck_update_delay(&stats, delay) != NGX_OK) {
        ngx_shmtx_unlock(&peer_shm->mutex);
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, MODULE_NAME
                      "delay statistics update failed for peer: %V",
                      &peer->check_peer_addr->name);
        return;
    }

    peer_shm->last_check_delay = stats.last;
    peer_shm->avg_check_delay = stats.avg;
    peer_shm->min_check_delay = stats.min;
    peer_shm->max_check_delay = stats.max;
    peer_shm->delay_total = stats.delay_total;
    peer_shm->delay_sample_count = stats.delay_sample_count;

    ngx_shmtx_unlock(&peer_shm->mutex);

    ngx_log_debug6(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0, MODULE_NAME
                   "[delay] peer:%V, delay:%M ms, avg:%M ms, min:%M ms, max:%M ms, samples:%ui",
                   &peer->check_peer_addr->name,
                   delay,
                   peer_shm->avg_check_delay,
                   peer_shm->min_check_delay,
                   peer_shm->max_check_delay,
                   peer_shm->delay_sample_count);
}

static void
ngx_http_upstream_check_peek_handler(ngx_event_t *event)
{
    ngx_connection_t               *c;
    ngx_upstream_check_peer_t *peer;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    c = event->data;
    peer = c->data;

    if (ngx_http_upstream_check_peek_one_byte(c) == NGX_OK) {
        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0, MODULE_NAME
                       "http check peek ok, index: %ui, fd: %d, peer: %V",
                       peer->index, c->fd, &peer->check_peer_addr->name);
        ngx_http_upstream_check_status_update(peer, 1);
        c->requests++;
    } else {
        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0, MODULE_NAME
                       "http check peek error, index: %ui, fd: %d, peer: %V",
                       peer->index, c->fd, &peer->check_peer_addr->name);
        c->error = 1;
        ngx_http_upstream_check_status_update(peer, 0);
    }

    ngx_http_upstream_check_clean_event(peer);

    ngx_http_upstream_check_finish_handler(event);
}


static void
ngx_http_upstream_check_dummy_handler(ngx_event_t *event)
{
    return;
}


static void
ngx_http_upstream_check_reuse_close_handler(ngx_event_t *event)
{
    char                            buf[1];
    ngx_err_t                       err;
    ngx_int_t                       n;
    ngx_connection_t               *c;
    ngx_upstream_check_peer_t      *peer;

    c = event->data;
    peer = c->data;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0, MODULE_NAME
                   "http check reuse close handler, index: %ui, fd: %d",
                   peer ? peer->index : (ngx_uint_t) -1, c->fd);

    if (peer == NULL) {
        return;
    }

    if (c->close || c->read->timedout) {
        ngx_http_upstream_check_close_connection(peer, c,
                                                c->close ? "drain" : "idle timeout");
        return;
    }

    n = recv(c->fd, buf, 1, MSG_PEEK);
    err = ngx_socket_errno;

    if (n == -1 && err == NGX_EAGAIN) {
        event->ready = 0;

        if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
            ngx_http_upstream_check_close_connection(peer, c, "read event failed");
        }

        return;
    }

    ngx_http_upstream_check_close_connection(peer, c,
        n == 0 ? "peer closed idle" : "idle data or error");
}


static void
ngx_http_upstream_check_close_connection(ngx_upstream_check_peer_t *peer,
    ngx_connection_t *c, const char *reason)
{
    struct linger  so_linger;

    if (c == NULL) {
        return;
    }

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0, MODULE_NAME
                   "http check close connection, index: %ui, fd: %d, reason: %s",
                   peer ? peer->index : (ngx_uint_t) -1, c->fd, reason);

    if (c->read && c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    if (c->write && c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    ngx_reusable_connection(c, 0);

    so_linger.l_onoff = 1;
    so_linger.l_linger = 0;
    (void) setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &so_linger,
                      sizeof(so_linger));

    if (peer && peer->pc.connection == c) {
        peer->pc.connection = NULL;
    }

    ngx_close_connection(c);
}


static void
ngx_http_upstream_check_send_handler(ngx_event_t *event)
{
    ssize_t                         size;
    ngx_connection_t               *c;
    ngx_http_upstream_check_ctx_t  *ctx;
    ngx_upstream_check_peer_t *peer;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    c = event->data;
    peer = c->data;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, MODULE_NAME "http check send.");

    if (peer->state != NGX_HTTP_CHECK_CONNECT_DONE) {
        if (ngx_handle_write_event(c->write, 0) != NGX_OK) {

            ngx_log_error(NGX_LOG_ERR, event->log, 0, MODULE_NAME
                          "check handle write event error with peer: %V ",
                          &peer->check_peer_addr->name);

            goto check_send_fail;
        }

        return;
    }

    ctx = peer->check_data;

    while (ctx->send.pos < ctx->send.last) {

        size = c->send(c, ctx->send.pos, ctx->send.last - ctx->send.pos);

#if (NGX_DEBUG)
        {
        ngx_err_t  err;

        err = (size >=0) ? 0 : ngx_socket_errno;
        ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, err,
                       "http check send size: %z, total: %z",
                       size, ctx->send.last - ctx->send.pos);
        }
#endif

        if (size > 0) {
            ctx->send.pos += size;
        } else if (size == 0 || size == NGX_AGAIN) {
            return;
        } else {
            c->error = 1;
            goto check_send_fail;
        }
    }

    if (ctx->send.pos == ctx->send.last) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, MODULE_NAME "http check send done.");
        peer->state = NGX_HTTP_CHECK_SEND_DONE;
        c->requests++;
    }

    return;

check_send_fail:
    ngx_http_upstream_check_status_update(peer, 0);
    ngx_http_upstream_check_clean_event(peer);
}


static ngx_int_t
ngx_http_upstream_check_udp_init(ngx_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t       *ctx;
    ngx_upstream_check_srv_conf_t  *ucscf;

    ctx = peer->check_data;
    ucscf = peer->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    ctx->state = 0;

    return NGX_OK;
}


static void
ngx_http_upstream_check_udp_reinit(ngx_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t  *ctx;

    ctx = peer->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;

    ctx->state = 0;
}


static void
ngx_http_upstream_check_status_update(ngx_upstream_check_peer_t *peer,
    ngx_int_t result)
{
    ngx_flag_t                         update_delay;
    ngx_upstream_check_srv_conf_t  *ucscf;

    ucscf = peer->conf;

    if (result) {
        update_delay = (ucscf->check_type_conf->type != NGX_HTTP_CHECK_TCP ||
                        (peer->pc.connection != NULL &&
                         peer->pc.connection->requests == 0));

        if (update_delay) {
            ngx_http_upstream_check_update_delay(peer);
        } else {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0, MODULE_NAME
                           "http check delay skip, peer: %V, requests: %ui",
                           &peer->check_peer_addr->name,
                           peer->pc.connection ? peer->pc.connection->requests : 0);
        }

        if(peer->shm->rise_count < (ngx_uint_t)-1) {
            peer->shm->rise_count++;
        }else{
            peer->shm->rise_count = ucscf->rise_count;
        }
        peer->shm->fall_count = 0;
        if (peer->shm->down && peer->shm->rise_count >= ucscf->rise_count) {
            peer->shm->down = 0;
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, MODULE_NAME
                          "enable check peer: %V ",
                          &peer->check_peer_addr->name);
        }
    } else {
        peer->shm->rise_count = 0;
        if(peer->shm->fall_count < (ngx_uint_t)-1) {
            peer->shm->fall_count++;
        }else{
            peer->shm->fall_count = ucscf->fall_count;
        }
        if (!peer->shm->down && peer->shm->fall_count >= ucscf->fall_count) {
            peer->shm->down = 1;
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, MODULE_NAME
                          "disable check peer: %V ",
                          &peer->check_peer_addr->name);
        }
    }

    ngx_log_debug5(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0, MODULE_NAME
                   "http check result, peer: %V, result: %s, rise: %ui, "
                   "fall: %ui, down: %d",
                   &peer->check_peer_addr->name,
                   result ? "up" : "down",
                   peer->shm->rise_count,
                   peer->shm->fall_count,
                   peer->shm->down);

    peer->shm->access_time = ngx_current_msec;
}


static void
ngx_http_upstream_check_clean_event(ngx_upstream_check_peer_t *peer)
{
    ngx_connection_t                    *c;
    ngx_upstream_check_srv_conf_t       *ucscf;
    ngx_check_conf_t                    *cf;
    ngx_flag_t                           reusable = 0;

    c = peer->pc.connection;
    ucscf = peer->conf;
    cf = ucscf->check_type_conf;

    if (c) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0, MODULE_NAME
                       "http check clean event: index:%i, fd: %d",
                       peer->index, c->fd);
        if (c->error == 0 &&
            !c->timedout &&
            ucscf->tcp_reuse &&
            cf->type == NGX_HTTP_CHECK_TCP &&
            (ucscf->check_keepalive_requests == 0 ||
             c->requests < ucscf->check_keepalive_requests))
        {
            reusable = 1;
        }

        if (reusable) {
            if (c->read->timer_set) {
                ngx_del_timer(c->read);
            }

            if (c->write->timer_set) {
                ngx_del_timer(c->write);
            }

            c->write->handler = ngx_http_upstream_check_dummy_handler;
            c->read->handler = ngx_http_upstream_check_reuse_close_handler;
            c->data = peer;
            c->idle = 1;
            ngx_reusable_connection(c, 1);

            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_http_upstream_check_close_connection(peer, c,
                                                        "read event failed");
            } else {
                ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0, MODULE_NAME
                               "http check save reusable connection, index: %ui, "
                               "fd: %d, requests: %ui",
                               peer->index, c->fd, c->requests);

                if (c->read->ready) {
                    ngx_http_upstream_check_reuse_close_handler(c->read);
                }
            }
        } else {
            ngx_http_upstream_check_close_connection(peer, c,
                ucscf->tcp_reuse ? "not reusable" : "reuse off");
        }
    }

    if (peer->check_timeout_ev.timer_set) {
        ngx_del_timer(&peer->check_timeout_ev);
    }

    peer->state = NGX_HTTP_CHECK_ALL_DONE;

    if (peer->check_data != NULL && peer->reinit) {
        peer->reinit(peer);
    }

    peer->shm->owner = NGX_INVALID_PID;
}


static void
ngx_http_upstream_check_timeout_handler(ngx_event_t *event)
{
    ngx_upstream_check_peer_t  *peer;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    peer = event->data;

    if (peer->pc.connection == NULL) {
        peer->shm->owner = NGX_INVALID_PID;
        return;
    }

    if (peer->pc.type == SOCK_STREAM) {
        peer->pc.connection->error = 1;

        ngx_log_error(NGX_LOG_ERR, event->log, 0, MODULE_NAME
                      "tcp check time out with peer: %V ",
                      &peer->check_peer_addr->name);

        ngx_http_upstream_check_status_update(peer, 0);
    } else if (peer->pc.type == SOCK_DGRAM) {
        peer->pc.connection->error = 0;

        ngx_log_error(NGX_LOG_NOTICE, event->log, 0, MODULE_NAME
                      "udp check time out with peer: %V, we assume it's up :) ",
                      &peer->check_peer_addr->name);

        ngx_http_upstream_check_status_update(peer, 1);
    }

    ngx_http_upstream_check_clean_event(peer);
}


static void
ngx_http_upstream_check_finish_handler(ngx_event_t *event)
{
    if (ngx_http_upstream_check_need_exit()) {
        return;
    }
}


static ngx_int_t
ngx_http_upstream_check_need_exit()
{
    if (ngx_terminate || ngx_exiting || ngx_quit) {
        ngx_http_upstream_check_clear_all_events();
        return 1;
    }

    return 0;
}


static void
ngx_http_upstream_check_clear_all_events()
{
    ngx_uint_t                       i;
    ngx_connection_t                *c;
    ngx_upstream_check_peer_t  *peer;
    ngx_upstream_check_peers_t *peers;

    static ngx_flag_t                has_cleared = 0;

    if (has_cleared || http_peers_ctx == NULL) {
        return;
    }

    ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0, MODULE_NAME
                  "clear all the events on %P ", ngx_pid);

    has_cleared = 1;

    peers = http_peers_ctx;

    peer = peers->peers.elts;
    for (i = 0; i < peers->peers.nelts; i++) {

        if (peer[i].check_ev.timer_set) {
            ngx_del_timer(&peer[i].check_ev);
        }

        if (peer[i].check_timeout_ev.timer_set) {
            ngx_del_timer(&peer[i].check_timeout_ev);
        }

        c = peer[i].pc.connection;
        if (c) {
            ngx_http_upstream_check_close_connection(&peer[i], c, "exit");
        }

    }
}


static ngx_int_t
ngx_http_upstream_check_status_handler(ngx_http_request_t *r)
{
    ngx_int_t                              rc;
    ngx_upstream_check_peers_t       *peers;
    ngx_http_upstream_check_loc_conf_t    *uclcf;
    ngx_http_upstream_check_status_ctx_t  *ctx;
    ngx_healthcheck_status_writer_t       writer;

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    uclcf = ngx_http_get_module_loc_conf(r, ngx_http_upstream_check_module);

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_check_status_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_upstream_check_status_parse_args(r, ctx);

    if (ctx->format == NULL) {
        ctx->format = uclcf->format;
    }

    r->headers_out.content_type = ctx->format->content_type;

    peers = http_peers_ctx;
    if (peers == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, MODULE_NAME
                      "http upstream check module can not find any check "
                      "server, make sure you've added the check servers");

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_healthcheck_status_writer_init(&writer, r->pool);

    if (ctx->format->output(&writer, peers, ctx->flag) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, MODULE_NAME
                   "http check status response length: %uz, blocks: %ui",
                   writer.length, writer.blocks);

    return ngx_healthcheck_status_writer_send(r, &writer);
}


static void
ngx_http_upstream_check_status_parse_args(ngx_http_request_t *r,
    ngx_http_upstream_check_status_ctx_t *ctx)
{
    ngx_str_t                    value;
    ngx_uint_t                   i;
    ngx_check_status_command_t  *command;

    if (r->args.len == 0) {
        return;
    }

    for (i = 0; /* void */ ; i++) {

        command = &ngx_check_status_commands[i];

        if (command->name.len == 0) {
            break;
        }

        if (ngx_http_arg(r, command->name.data, command->name.len, &value)
            == NGX_OK) {

           if (command->handler(ctx, &value) != NGX_OK) {
               ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, MODULE_NAME
                             "http upstream check, bad argument: \"%V\"",
                             &value);
           }
        }
    }

    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0, MODULE_NAME
            "http upstream check, flag: \"%ui\"", ctx->flag);
}


static ngx_int_t
ngx_http_upstream_check_status_command_format(
    ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value)
{
    ctx->format = ngx_http_get_check_status_format_conf(value);
    if (ctx->format == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_status_command_status(
    ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value)
{
    if (value->len == (sizeof("down") - 1)
        && ngx_strncasecmp(value->data, (u_char *) "down", value->len) == 0) {

        ctx->flag |= NGX_CHECK_STATUS_DOWN;

    } else if (value->len == (sizeof("up") - 1)
               && ngx_strncasecmp(value->data, (u_char *) "up", value->len)
               == 0) {

        ctx->flag |= NGX_CHECK_STATUS_UP;

    } else {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_status_html_format(
    ngx_healthcheck_status_writer_t *writer,
    ngx_upstream_check_peers_t *peers, ngx_uint_t flag)
{
    ngx_uint_t                      i, count;
    ngx_upstream_check_peer_t *peer;

    peer = peers->peers.elts;

    count = 0;

    for (i = 0; i < peers->peers.nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        count++;
    }

    if (ngx_healthcheck_status_writer_printf(writer,
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\n"
            "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
            "<head>\n"
            "  <title>Nginx http upstream check status</title>\n"
            "</head>\n"
            "<body>\n"
            "<h1>Nginx http upstream check status</h1>\n"
            "<h2>Check upstream server number: %ui, generation: %ui</h2>\n"
            "<table style=\"background-color:white\" cellspacing=\"0\" "
            "       cellpadding=\"3\" border=\"1\">\n"
            "  <tr bgcolor=\"#C0C0C0\">\n"
            "    <th>Index</th>\n"
            "    <th>Upstream</th>\n"
            "    <th>Name</th>\n"
            "    <th>Status</th>\n"
            "    <th>Rise counts</th>\n"
            "    <th>Fall counts</th>\n"
            "    <th>Check type</th>\n"
            "    <th>Check port</th>\n"
            "  </tr>\n",
            count, ngx_http_upstream_check_shm_generation) != NGX_OK)
        return NGX_ERROR;

    for (i = 0; i < peers->peers.nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        if (ngx_healthcheck_status_writer_printf(writer,
                "  <tr%s>\n"
                "    <td>%ui</td>\n"
                "    <td>%V</td>\n"
                "    <td>%V</td>\n"
                "    <td>%s</td>\n"
                "    <td>%ui</td>\n"
                "    <td>%ui</td>\n"
                "    <td>%V</td>\n"
                "    <td>%ui</td>\n"
                "  </tr>\n",
                peer[i].shm->down ? " bgcolor=\"#FF0000\"" : "",
                i,
                peer[i].upstream_name,
                &peer[i].peer_addr->name,
                peer[i].shm->down ? "down" : "up",
                peer[i].shm->rise_count,
                peer[i].shm->fall_count,
                &peer[i].conf->check_type_conf->name,
                peer[i].conf->port) != NGX_OK) return NGX_ERROR;
    }

    if (ngx_healthcheck_status_writer_printf(writer,
            "</table>\n"
            "</body>\n"
            "</html>\n") != NGX_OK) return NGX_ERROR;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_status_csv_format(
    ngx_healthcheck_status_writer_t *writer,
    ngx_upstream_check_peers_t *peers, ngx_uint_t flag)
{
    ngx_uint_t                       i;
    ngx_upstream_check_peer_t  *peer;

    peer = peers->peers.elts;
    for (i = 0; i < peers->peers.nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        if (ngx_healthcheck_status_writer_printf(writer,
                "%ui,%V,%V,%s,%ui,%ui,%V,%ui\n",
                i,
                peer[i].upstream_name,
                &peer[i].peer_addr->name,
                peer[i].shm->down ? "down" : "up",
                peer[i].shm->rise_count,
                peer[i].shm->fall_count,
                &peer[i].conf->check_type_conf->name,
                peer[i].conf->port) != NGX_OK) return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_status_json_format(
    ngx_healthcheck_status_writer_t *writer,
    ngx_upstream_check_peers_t *peers, ngx_uint_t flag)
{
    ngx_uint_t                       count, i, last;
    ngx_upstream_check_peer_t  *peer;

    peer = peers->peers.elts;

    count = 0;

    for (i = 0; i < peers->peers.nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        count++;
    }

    if (ngx_healthcheck_status_writer_printf(writer,
            "{\"servers\": {\n"
            "  \"total\": %ui,\n"
            "  \"generation\": %ui,\n"
            "  \"server\": [\n",
            count,
            ngx_http_upstream_check_shm_generation) != NGX_OK)
        return NGX_ERROR;

    last = peers->peers.nelts - 1;
    for (i = 0; i < peers->peers.nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        if (ngx_healthcheck_status_writer_printf(writer,
                "    {\"index\": %ui, "
                "\"upstream\": \"%V\", "
                "\"name\": \"%V\", "
                "\"status\": \"%s\", "
                "\"rise\": %ui, "
                "\"fall\": %ui, "
                "\"type\": \"%V\", "
                "\"port\": %ui}"
                "%s\n",
                i,
                peer[i].upstream_name,
                &peer[i].peer_addr->name,
                peer[i].shm->down ? "down" : "up",
                peer[i].shm->rise_count,
                peer[i].shm->fall_count,
                &peer[i].conf->check_type_conf->name,
                peer[i].conf->port,
                (i == last) ? "" : ",") != NGX_OK) return NGX_ERROR;
    }

    if (ngx_healthcheck_status_writer_printf(writer, "  ]\n")
        != NGX_OK) return NGX_ERROR;

    if (ngx_healthcheck_status_writer_printf(writer, "}}\n")
        != NGX_OK) return NGX_ERROR;

    return NGX_OK;
}


static ngx_check_conf_t *
ngx_http_get_check_type_conf(ngx_str_t *str)
{
    ngx_uint_t  i;

    for (i = 0; /* void */ ; i++) {

        if (ngx_check_types[i].type == 0) {
            break;
        }

        if (str->len != ngx_check_types[i].name.len) {
            continue;
        }

        if (ngx_strncmp(str->data, ngx_check_types[i].name.data,
                        str->len) == 0)
        {
            return &ngx_check_types[i];
        }
    }

    return NULL;
}


static char *
ngx_http_upstream_check(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                           *value, s;
    ngx_uint_t                           i, port, rise, fall, default_down;
    ngx_msec_t                           interval, timeout;
    ngx_upstream_check_srv_conf_t  *ucscf;

    /* default values */
    port = 0;
    rise = 2;
    fall = 5;
    interval = 30000;
    timeout = 1000;
    default_down = 1;

    value = cf->args->elts;

    ucscf = ngx_http_conf_get_module_srv_conf(cf,
                                              ngx_http_upstream_check_module);
    if (ucscf == NULL) {
        return NGX_CONF_ERROR;
    }

    ucscf->tcp_reuse = 0;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "type=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            ucscf->check_type_conf = ngx_http_get_check_type_conf(&s);

            if (ucscf->check_type_conf == NULL) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "port=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            port = ngx_atoi(s.data, s.len);
            if (port == (ngx_uint_t) NGX_ERROR || port == 0) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "interval=", 9) == 0) {
            s.len = value[i].len - 9;
            s.data = value[i].data + 9;

            interval = ngx_atoi(s.data, s.len);
            if (interval == (ngx_msec_t) NGX_ERROR || interval == 0) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "timeout=", 8) == 0) {
            s.len = value[i].len - 8;
            s.data = value[i].data + 8;

            timeout = ngx_atoi(s.data, s.len);
            if (timeout == (ngx_msec_t) NGX_ERROR || timeout == 0) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "rise=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            rise = ngx_atoi(s.data, s.len);
            if (rise == (ngx_uint_t) NGX_ERROR || rise == 0) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "fall=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            fall = ngx_atoi(s.data, s.len);
            if (fall == (ngx_uint_t) NGX_ERROR || fall == 0) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "default_down=", 13) == 0) {
            s.len = value[i].len - 13;
            s.data = value[i].data + 13;

            if (ngx_strcasecmp(s.data, (u_char *) "true") == 0) {
                default_down = 1;
            } else if (ngx_strcasecmp(s.data, (u_char *) "false") == 0) {
                default_down = 0;
            } else {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid value \"%s\", "
                                   "it must be \"true\" or \"false\"",
                                   value[i].data);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "reuse=", 6) == 0) {
            s.len = value[i].len - 6;
            s.data = value[i].data + 6;

            if (s.len == 2
                && ngx_strncasecmp(s.data, (u_char *) "on", 2) == 0)
            {
                ucscf->tcp_reuse = 1;

            } else if (s.len == 3
                       && ngx_strncasecmp(s.data, (u_char *) "off", 3) == 0)
            {
                ucscf->tcp_reuse = 0;

            } else {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid value \"%V\", "
                                   "it must be \"on\" or \"off\"",
                                   &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        goto invalid_check_parameter;
    }

    ucscf->port = port;
    ucscf->check_interval = interval;
    ucscf->check_timeout = timeout;
    ucscf->fall_count = fall;
    ucscf->rise_count = rise;
    ucscf->default_down = default_down;

    if (ucscf->check_type_conf == NGX_CONF_UNSET_PTR) {
        ngx_str_set(&s, "tcp");
        ucscf->check_type_conf = ngx_http_get_check_type_conf(&s);
    }

    if (ucscf->check_type_conf->type == NGX_HTTP_CHECK_UDP && ucscf->tcp_reuse) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "reuse=on is only valid with type=tcp");
        return NGX_CONF_ERROR;
    }

    ngx_log_debug7(NGX_LOG_DEBUG_HTTP, cf->log, 0, MODULE_NAME
                   "http check parsed, type: %V, interval: %M, timeout: %M, "
                   "rise: %ui, fall: %ui, default_down: %ui, reuse: %i",
                   &ucscf->check_type_conf->name, interval, timeout, rise, fall,
                   default_down, ucscf->tcp_reuse);

    return NGX_CONF_OK;

invalid_check_parameter:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}


static char *
ngx_http_upstream_check_keepalive_requests(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_str_t                           *value;
    ngx_upstream_check_srv_conf_t  *ucscf;
    ngx_uint_t                           requests;

    value = cf->args->elts;

    ucscf = ngx_http_conf_get_module_srv_conf(cf,
                                              ngx_http_upstream_check_module);

    requests = ngx_atoi(value[1].data, value[1].len);
    if (requests == (ngx_uint_t) NGX_ERROR || requests == 0) {
        return "invalid value";
    }

    ucscf->check_keepalive_requests = requests;

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                            *value;
    ngx_http_upstream_check_main_conf_t  *ucmcf;

    ucmcf = ngx_http_conf_get_module_main_conf(cf,
                                               ngx_http_upstream_check_module);
    if (ucmcf->check_shm_size) {
        return "is duplicate";
    }

    value = cf->args->elts;

    ucmcf->check_shm_size = ngx_parse_size(&value[1]);
    if (ucmcf->check_shm_size == (size_t) NGX_ERROR) {
        return "invalid value";
    }

    return NGX_CONF_OK;
}


static ngx_check_status_conf_t *
ngx_http_get_check_status_format_conf(ngx_str_t *str)
{
    ngx_uint_t  i;

    for (i = 0; /* void */ ; i++) {

        if (ngx_check_status_formats[i].format.len == 0) {
            break;
        }

        if (str->len != ngx_check_status_formats[i].format.len) {
            continue;
        }

        if (ngx_strncmp(str->data, ngx_check_status_formats[i].format.data,
                        str->len) == 0)
        {
            return &ngx_check_status_formats[i];
        }
    }

    return NULL;
}


static char *
ngx_http_upstream_check_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                           *value;
    ngx_http_core_loc_conf_t            *clcf;
    ngx_http_upstream_check_loc_conf_t  *uclcf;

    value = cf->args->elts;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_upstream_check_status_handler;

    if (cf->args->nelts == 2) {
        uclcf = ngx_http_conf_get_module_loc_conf(cf,
                                              ngx_http_upstream_check_module);

        uclcf->format = ngx_http_get_check_status_format_conf(&value[1]);
        if (uclcf->format == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid check format \"%V\"", &value[1]);

            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static void *
ngx_http_upstream_check_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_check_main_conf_t  *ucmcf;

    ucmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_check_main_conf_t));
    if (ucmcf == NULL) {
        return NULL;
    }

    ucmcf->peers = ngx_pcalloc(cf->pool,
                               sizeof(ngx_upstream_check_peers_t));
    if (ucmcf->peers == NULL) {
        return NULL;
    }

    if (ngx_array_init(&ucmcf->peers->peers, cf->pool, 16,
                       sizeof(ngx_upstream_check_peer_t)) != NGX_OK)
    {
        return NULL;
    }

    return ucmcf;
}


static char *
ngx_http_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_uint_t                      i;
    ngx_http_upstream_srv_conf_t  **uscfp;
    ngx_http_upstream_main_conf_t  *umcf;

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (ngx_http_upstream_check_init_srv_conf(cf, uscfp[i]) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    return ngx_http_upstream_check_init_shm(cf, conf);
}


static void *
ngx_http_upstream_check_create_srv_conf(ngx_conf_t *cf)
{
    ngx_upstream_check_srv_conf_t  *ucscf;

    ucscf = ngx_pcalloc(cf->pool, sizeof(ngx_upstream_check_srv_conf_t));
    if (ucscf == NULL) {
        return NULL;
    }

    ucscf->port = NGX_CONF_UNSET_UINT;
    ucscf->fall_count = NGX_CONF_UNSET_UINT;
    ucscf->rise_count = NGX_CONF_UNSET_UINT;
    ucscf->check_timeout = NGX_CONF_UNSET_MSEC;
    ucscf->check_keepalive_requests = NGX_CONF_UNSET_UINT;
    ucscf->check_type_conf = NGX_CONF_UNSET_PTR;
    ucscf->tcp_reuse = NGX_CONF_UNSET;

    return ucscf;
}


static void *
ngx_http_upstream_check_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_check_loc_conf_t  *uclcf;

    uclcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_check_loc_conf_t));
    if (uclcf == NULL) {
        return NULL;
    }

    uclcf->format = NGX_CONF_UNSET_PTR;

    return uclcf;
}


static char *
ngx_http_upstream_check_init_srv_conf(ngx_conf_t *cf, void *conf)
{
    ngx_check_conf_t                   *check;
    ngx_http_upstream_srv_conf_t       *us = conf;
    ngx_upstream_check_srv_conf_t *ucscf;

    if (us->srv_conf == NULL) {
        return NGX_CONF_OK;
    }

    ucscf = ngx_http_conf_upstream_srv_conf(us, ngx_http_upstream_check_module);

    if (ucscf->port == NGX_CONF_UNSET_UINT) {
        ucscf->port = 0;
    }

    if (ucscf->fall_count == NGX_CONF_UNSET_UINT) {
        ucscf->fall_count = 2;
    }

    if (ucscf->rise_count == NGX_CONF_UNSET_UINT) {
        ucscf->rise_count = 5;
    }

    if (ucscf->check_interval == NGX_CONF_UNSET_MSEC) {
        ucscf->check_interval = 0;
    }

    if (ucscf->check_timeout == NGX_CONF_UNSET_MSEC) {
        ucscf->check_timeout = 1000;
    }

    if (ucscf->check_keepalive_requests == NGX_CONF_UNSET_UINT) {
        ucscf->check_keepalive_requests = NGX_CHECK_KEEPALIVE_REQUESTS_DEFAULT;
    }

    if (ucscf->tcp_reuse == NGX_CONF_UNSET) {
        ucscf->tcp_reuse = 0;
    }

    if (ucscf->check_type_conf == NGX_CONF_UNSET_PTR) {
        ucscf->check_type_conf = NULL;
    }

    check = ucscf->check_type_conf;

    if (check) {
        if (ucscf->send.len == 0) {
            ucscf->send.data = check->default_send.data;
            ucscf->send.len = check->default_send.len;
        }
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child)
{
    ngx_str_t                            format = ngx_string("html");
    ngx_http_upstream_check_loc_conf_t  *prev = parent;
    ngx_http_upstream_check_loc_conf_t  *conf = child;

    ngx_conf_merge_ptr_value(conf->format, prev->format,
                             ngx_http_get_check_status_format_conf(&format));

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_init_shm(ngx_conf_t *cf, void *conf)
{
    ngx_str_t                            *shm_name;
    ngx_uint_t                            shm_size;
    ngx_shm_zone_t                       *shm_zone;
    ngx_http_upstream_check_main_conf_t  *ucmcf = conf;

    if (1) {

        ngx_http_upstream_check_shm_generation++;

        shm_name = &ucmcf->peers->check_shm_name;

        if (ngx_http_upstream_check_get_shm_name(shm_name, cf->pool,
                ngx_http_upstream_check_shm_generation) != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }

        /* The default check shared memory size is 1M */
        shm_size = 1 * 1024 * 1024;

        shm_size = shm_size < ucmcf->check_shm_size ?
                              ucmcf->check_shm_size : shm_size;

        shm_zone = ngx_shared_memory_add(cf, shm_name, shm_size,
                                         &ngx_http_upstream_check_module);
        if (shm_zone == NULL) {
            return NGX_CONF_ERROR;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, cf->log, 0, MODULE_NAME
                       "http upstream check, upsteam:%V, shm_zone size:%ui",
                       shm_name, shm_size);

        shm_zone->data = cf->pool;
        http_peers_ctx = ucmcf->peers;

        shm_zone->init = ngx_http_upstream_check_init_shm_zone;
    }
    else {
         http_peers_ctx = NULL;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_upstream_check_get_shm_name(ngx_str_t *shm_name, ngx_pool_t *pool,
    ngx_uint_t generation)
{
    u_char  *last;

    shm_name->data = ngx_palloc(pool, SHM_NAME_LEN);
    if (shm_name->data == NULL) {
        return NGX_ERROR;
    }

    last = ngx_snprintf(shm_name->data, SHM_NAME_LEN, "%s#%ui",
                        "ngx_http_upstream_check", generation);

    shm_name->len = last - shm_name->data;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    size_t                               identity_size, size;
    ngx_int_t                            rc;
    ngx_uint_t                           i, number;
    ngx_pool_t                          *pool, *temp_pool;
    ngx_slab_pool_t                     *shpool;
    ngx_upstream_check_peer_t      *peer;
    ngx_upstream_check_peers_t     *peers;
    ngx_upstream_check_srv_conf_t  *ucscf;
    ngx_upstream_check_peer_shm_t  *peer_shm, *opeer_shm;
    ngx_upstream_check_peers_shm_t *candidate, *peers_shm, *opeers_shm;
    ngx_healthcheck_identity_index_t *index;

    opeers_shm = NULL;
    peers_shm = NULL;
    temp_pool = NULL;
    peers = http_peers_ctx;
    if (peers == NULL) {
        return NGX_OK;
    }

    number = peers->peers.nelts;

    pool = shm_zone->data;
    if (pool == NULL) {
        pool = ngx_cycle->pool;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    // alloc peers_shm
    size = sizeof(*peers_shm);
    if (number > 1) {
        if (number - 1 > ((size_t) -1 - size)
                         / sizeof(ngx_upstream_check_peer_shm_t))
        {
            goto failure;
        }
        size += (number - 1) * sizeof(ngx_upstream_check_peer_shm_t);
    }

    identity_size = 0;
    peer = peers->peers.elts;
    for (i = 0; i < number; i++) {
        if (peer[i].upstream_name->len > (size_t) -1 - identity_size
            || peer[i].peer_addr->socklen
               > (size_t) -1 - identity_size - peer[i].upstream_name->len
            || peer[i].check_peer_addr->socklen
               > (size_t) -1 - identity_size - peer[i].upstream_name->len
                 - peer[i].peer_addr->socklen)
        {
            goto failure;
        }
        identity_size += peer[i].upstream_name->len
                         + peer[i].peer_addr->socklen
                         + peer[i].check_peer_addr->socklen;
    }

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, shm_zone->shm.log, 0, MODULE_NAME
                   "http check shm profile, peers: %ui, peer_block: %uz, "
                   "identity_bytes: %uz, zone_size: %uz",
                   number, size, identity_size,
                   shm_zone->shm.size);

    peers_shm = ngx_slab_alloc(shpool, size);

    if (peers_shm == NULL) {
        goto failure;
    }
    ngx_memzero(peers_shm, size);

    peers_shm->magic = NGX_HEALTHCHECK_SHM_MAGIC;
    peers_shm->version = NGX_HEALTHCHECK_SHM_VERSION;
    peers_shm->generation = ngx_http_upstream_check_shm_generation;
    peers_shm->number = number;

    ngx_log_error(NGX_LOG_INFO, shm_zone->shm.log, 0, MODULE_NAME
                  "[ngx-healthcheck][http] old data: %p ",data);

    opeers_shm = ngx_healthcheck_find_latest_peers_shm((ngx_cycle_t *) ngx_cycle,
                                      &ngx_http_upstream_check_module);
    if (data != NULL) {
        candidate = data;

        if (candidate->magic == NGX_HEALTHCHECK_SHM_MAGIC
            && candidate->version == NGX_HEALTHCHECK_SHM_VERSION
            && (opeers_shm == NULL
                || candidate->generation > opeers_shm->generation))
        {
            opeers_shm = candidate;
        }
    }

    temp_pool = ngx_create_pool(ngx_pagesize, shm_zone->shm.log);
    if (temp_pool == NULL) {
        goto failure;
    }

    index = ngx_healthcheck_identity_index_create(temp_pool, opeers_shm);
    if (index == NULL) {
        goto failure;
    }

    for (i = 0; i < number; i++) {

        peer_shm = &peers_shm->peers[i];

        peer_shm->owner = NGX_INVALID_PID;

        if (ngx_healthcheck_copy_shm_identity(shpool, peer_shm, &peer[i])
            != NGX_OK)
        {
            goto failure;
        }

        opeer_shm = ngx_healthcheck_identity_index_take(index, &peer[i]);
        if (opeer_shm) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, shm_zone->shm.log, 0, MODULE_NAME
                           "http upstream check, inherit opeer: %V ",
                           &peer[i].peer_addr->name);

            rc = ngx_http_upstream_check_init_shm_peer(peer_shm, opeer_shm,
                     0, pool, &peer[i].peer_addr->name);
            if (rc != NGX_OK) {
                goto failure;
            }

            continue;
        }

        ucscf = peer[i].conf;
        rc = ngx_http_upstream_check_init_shm_peer(peer_shm, NULL,
                                                   ucscf->default_down, pool,
                                                   &peer[i].peer_addr->name);
        if (rc != NGX_OK) {
            goto failure;
        }
    }

    peers->peers_shm = peers_shm;
    shm_zone->data = peers_shm;
    ngx_destroy_pool(temp_pool);

    return NGX_OK;

failure:
    if (temp_pool != NULL) {
        ngx_destroy_pool(temp_pool);
    }
    ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0, MODULE_NAME
                  "http upstream check_shm_size is too small, "
                  "including peer identities and upstream names; "
                  "you should specify a larger size.");
    return NGX_ERROR;
}


static ngx_int_t
ngx_http_upstream_check_init_shm_peer(ngx_upstream_check_peer_shm_t *psh,
    ngx_upstream_check_peer_shm_t *opsh, ngx_uint_t init_down,
    ngx_pool_t *pool, ngx_str_t *name)
{
    u_char  *file;

    if (opsh) {
        ngx_shmtx_lock(&opsh->mutex);
        psh->access_time  = opsh->access_time;
        psh->access_count = opsh->access_count;

        psh->fall_count   = opsh->fall_count;
        psh->rise_count   = opsh->rise_count;
        psh->busyness     = opsh->busyness;

        psh->down         = opsh->down;

        psh->last_check_delay   = opsh->last_check_delay;
        psh->avg_check_delay    = opsh->avg_check_delay;
        psh->max_check_delay    = opsh->max_check_delay;
        psh->min_check_delay    = opsh->min_check_delay;
        psh->delay_total        = opsh->delay_total;
        psh->delay_sample_count = opsh->delay_sample_count;
        ngx_shmtx_unlock(&opsh->mutex);

    } else {
        psh->access_time  = 0;
        psh->access_count = 0;

        psh->fall_count   = 0;
        psh->rise_count   = 0;
        psh->busyness     = 0;

        psh->down         = init_down;

        psh->last_check_delay   = 0;
        psh->avg_check_delay    = 0;
        psh->max_check_delay    = 0;
        psh->min_check_delay    = 0;
        psh->delay_total        = 0;
        psh->delay_sample_count = 0;
    }

#if (NGX_HAVE_ATOMIC_OPS)

    file = NULL;

#else

    file = ngx_pnalloc(pool, ngx_cycle->lock_file.len + name->len);
    if (file == NULL) {
        return NGX_ERROR;
    }

    (void) ngx_sprintf(file, "%V%V%Z", &ngx_cycle->lock_file, name);

#endif

#if (nginx_version >= 1002000)
    if (ngx_shmtx_create(&psh->mutex, &psh->lock, file) != NGX_OK) {
#else
    if (ngx_shmtx_create(&psh->mutex, (void *) &psh->lock, file) != NGX_OK) {
#endif
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_init_process(ngx_cycle_t *cycle)
{
    ngx_http_upstream_check_main_conf_t *ucmcf;

    ucmcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_upstream_check_module);
    if (ucmcf == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, cycle->log, 0, MODULE_NAME
                     "[ngx-healthcheck][http][init-process] no http section, skip init");
        return NGX_OK;
    }

    return ngx_http_upstream_check_add_timers(cycle);
}
