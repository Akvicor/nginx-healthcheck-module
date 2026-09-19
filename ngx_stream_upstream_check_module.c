/*
 * Copyright (C) 2017- Changxun Zhou(changxunzhou@qq.com)
 */

#include "ngx_stream_upstream_check_module.h"
#include "configuration/ngx_healthcheck_config.h"
#include "runtime/ngx_healthcheck_runtime.h"

static char *ngx_stream_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf);
static ngx_int_t ngx_stream_upstream_check_init_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_stream_upstream_check_init_module(ngx_cycle_t *cycle);
static void ngx_stream_upstream_check_exit_process(ngx_cycle_t *cycle);

static ngx_command_t ngx_stream_upstream_check_commands[] = {
    { ngx_string("check"), NGX_STREAM_UPS_CONF|NGX_CONF_1MORE,
      ngx_healthcheck_check, NGX_STREAM_SRV_CONF_OFFSET, 0, NULL },
    { ngx_string("check_keepalive_requests"), NGX_STREAM_UPS_CONF|NGX_CONF_TAKE1,
      ngx_healthcheck_keepalive, NGX_STREAM_SRV_CONF_OFFSET, 0, NULL },
    { ngx_string("check_shm_size"), NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_healthcheck_shm_size, NGX_STREAM_MAIN_CONF_OFFSET, 0, NULL },
    ngx_null_command
};

static ngx_stream_module_t ngx_stream_upstream_check_module_ctx = {
    NULL, NULL,
    ngx_healthcheck_create_main_conf,
    ngx_stream_upstream_check_init_main_conf,
    ngx_healthcheck_create_srv_conf, NULL
};

ngx_module_t ngx_stream_upstream_check_module = {
    NGX_MODULE_V1,
    &ngx_stream_upstream_check_module_ctx,
    ngx_stream_upstream_check_commands,
    NGX_STREAM_MODULE,
    NULL,
    ngx_stream_upstream_check_init_module,
    ngx_stream_upstream_check_init_process,
    NULL, NULL,
    ngx_stream_upstream_check_exit_process,
    NULL,
    NGX_MODULE_V1_PADDING
};

/* 绑定只在有效进程中发布，状态展示通过所属 cycle 的 generation 取值。 */
ngx_upstream_check_peers_t *stream_peers_ctx;

ngx_uint_t
ngx_stream_upstream_check_add_peer(ngx_conf_t *cf,
    ngx_stream_upstream_srv_conf_t *upstream, ngx_addr_t *address)
{
    ngx_upstream_check_srv_conf_t *conf;

    if (upstream->srv_conf == NULL) {
        return (ngx_uint_t) NGX_ERROR;
    }
    conf = ngx_stream_conf_upstream_srv_conf(upstream, ngx_stream_upstream_check_module);
    return ngx_healthcheck_add_peer(cf,
        ngx_stream_conf_get_module_main_conf(cf, ngx_stream_upstream_check_module),
        conf, &upstream->host, address);
}

ngx_uint_t
ngx_stream_upstream_check_peer_down(ngx_uint_t index)
{
    ngx_upstream_check_peer_t *peer;

    if (stream_peers_ctx == NULL || index >= stream_peers_ctx->peers.nelts) {
        return 0;
    }
    peer = stream_peers_ctx->peers.elts;
    if (peer[index].shm == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "stream check peer shm is NULL, index: %ui", index);
        return 0;
    }
    return (peer[index].shm->published & 2) != 0;
}

static char *
ngx_stream_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf)
{
    return ngx_healthcheck_configure_zone(cf, conf,
                &ngx_stream_upstream_check_module, NGX_HEALTHCHECK_STREAM);
}

static ngx_int_t
ngx_stream_upstream_check_init_process(ngx_cycle_t *cycle)
{
    ngx_healthcheck_main_conf_t *main;

    if (ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE) {
        return NGX_OK;
    }
    main = ngx_stream_cycle_get_module_main_conf(cycle, ngx_stream_upstream_check_module);
    stream_peers_ctx = main ? main->peers : NULL;
    return ngx_healthcheck_runtime_init(stream_peers_ctx);
}

static ngx_int_t
ngx_stream_upstream_check_init_module(ngx_cycle_t *cycle)
{
    /* 原生加载回调先于旧共享区释放；探测由后续原生进程初始化启动。 */
    ngx_healthcheck_runtime_stop(stream_peers_ctx);
    stream_peers_ctx = NULL;
    return NGX_OK;
}

static void
ngx_stream_upstream_check_exit_process(ngx_cycle_t *cycle)
{
    if (ngx_process == NGX_PROCESS_WORKER || ngx_process == NGX_PROCESS_SINGLE) {
        ngx_healthcheck_runtime_stop(stream_peers_ctx);
    }
}
