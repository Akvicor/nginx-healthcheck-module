/*
 * Copyright (C) 2017- Changxun Zhou(changxunzhou@qq.com)
 * Copyright (C) 2010-2014 Weibin Yao (yaoweibin@gmail.com)
 * Copyright (C) 2010-2014 Alibaba Group Holding Limited
 */

#include "ngx_http_upstream_check_module.h"
#include "configuration/ngx_healthcheck_config.h"
#include "runtime/ngx_healthcheck_runtime.h"
#include "status/ngx_healthcheck_request.h"

/* HTTP-only 旧状态入口的格式配置。 */
typedef struct {
    ngx_uint_t format;
} ngx_http_upstream_check_loc_conf_t;

static char *ngx_http_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf);
static ngx_int_t ngx_http_upstream_check_init_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_upstream_check_init_module(ngx_cycle_t *cycle);
static void ngx_http_upstream_check_exit_process(ngx_cycle_t *cycle);
static void *ngx_http_upstream_check_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_check_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static char *ngx_http_upstream_check_status(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_upstream_check_status_handler(ngx_http_request_t *r);

static ngx_command_t ngx_http_upstream_check_commands[] = {
    { ngx_string("check"), NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
      ngx_healthcheck_check, NGX_HTTP_SRV_CONF_OFFSET, 0, NULL },
    { ngx_string("check_keepalive_requests"), NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_healthcheck_keepalive, NGX_HTTP_SRV_CONF_OFFSET, 0, NULL },
    { ngx_string("check_shm_size"), NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_healthcheck_shm_size, NGX_HTTP_MAIN_CONF_OFFSET, 0, NULL },
    { ngx_string("check_status"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1|NGX_CONF_NOARGS,
      ngx_http_upstream_check_status, NGX_HTTP_LOC_CONF_OFFSET, 0, NULL },
    ngx_null_command
};

static ngx_http_module_t ngx_http_upstream_check_module_ctx = {
    NULL, NULL,
    ngx_healthcheck_create_main_conf,
    ngx_http_upstream_check_init_main_conf,
    ngx_healthcheck_create_srv_conf, NULL,
    ngx_http_upstream_check_create_loc_conf,
    ngx_http_upstream_check_merge_loc_conf
};

ngx_module_t ngx_http_upstream_check_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_check_module_ctx,
    ngx_http_upstream_check_commands,
    NGX_HTTP_MODULE,
    NULL,
    ngx_http_upstream_check_init_module,
    ngx_http_upstream_check_init_process,
    NULL, NULL,
    ngx_http_upstream_check_exit_process,
    NULL,
    NGX_MODULE_V1_PADDING
};

/* 仅有效 worker/single 发布绑定，配置解析通过所属 main conf 访问候选数据。 */
ngx_upstream_check_peers_t *http_peers_ctx;

ngx_uint_t
ngx_http_upstream_check_enabled(ngx_http_upstream_srv_conf_t *upstream)
{
    ngx_upstream_check_srv_conf_t *conf;

    if (upstream->srv_conf == NULL) {
        return 0;
    }
    conf = ngx_http_conf_upstream_srv_conf(upstream, ngx_http_upstream_check_module);
    return conf != NULL && conf->check_interval != 0;
}

ngx_uint_t
ngx_http_upstream_check_add_peer(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *upstream, ngx_addr_t *address)
{
    if (!ngx_http_upstream_check_enabled(upstream)) {
        return (ngx_uint_t) NGX_ERROR;
    }
    return ngx_healthcheck_add_peer(cf,
        ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_check_module),
        ngx_http_conf_upstream_srv_conf(upstream, ngx_http_upstream_check_module),
        &upstream->host, address);
}

ngx_uint_t
ngx_http_upstream_check_peer_down(ngx_uint_t index)
{
    ngx_upstream_check_peer_t *peer;

    if (http_peers_ctx == NULL || index >= http_peers_ctx->peers.nelts) {
        return 0;
    }
    peer = http_peers_ctx->peers.elts;
    if (peer[index].shm == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "http check peer shm is NULL, index: %ui", index);
        return 0;
    }
    return (peer[index].shm->published & 2) != 0;
}

static char *
ngx_http_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf)
{
    return ngx_healthcheck_configure_zone(cf, conf,
                &ngx_http_upstream_check_module, NGX_HEALTHCHECK_HTTP);
}

static ngx_int_t
ngx_http_upstream_check_init_process(ngx_cycle_t *cycle)
{
    ngx_healthcheck_main_conf_t *main;

    if (ngx_process != NGX_PROCESS_WORKER && ngx_process != NGX_PROCESS_SINGLE) {
        return NGX_OK;
    }
    main = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_upstream_check_module);
    http_peers_ctx = main ? main->peers : NULL;
    return ngx_healthcheck_runtime_init(http_peers_ctx);
}

static ngx_int_t
ngx_http_upstream_check_init_module(ngx_cycle_t *cycle)
{
    /* 原生加载回调先于旧共享区释放；探测由后续原生进程初始化启动。 */
    ngx_healthcheck_runtime_stop(http_peers_ctx);
    http_peers_ctx = NULL;
    return NGX_OK;
}

static void
ngx_http_upstream_check_exit_process(ngx_cycle_t *cycle)
{
    if (ngx_process == NGX_PROCESS_WORKER || ngx_process == NGX_PROCESS_SINGLE) {
        ngx_healthcheck_runtime_stop(http_peers_ctx);
    }
}

static ngx_int_t
ngx_http_upstream_check_format(ngx_str_t *value)
{
    ngx_str_t names[] = { ngx_string("html"), ngx_string("csv"), ngx_string("json") };
    ngx_uint_t i;

    for (i = 0; i < 3; i++) {
        if (value->len == names[i].len
            && ngx_strncmp(value->data, names[i].data, value->len) == 0)
        {
            return (ngx_int_t) i;
        }
    }
    return NGX_ERROR;
}

static void *
ngx_http_upstream_check_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_check_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf != NULL) {
        conf->format = NGX_CONF_UNSET_UINT;
    }
    return conf;
}

static char *
ngx_http_upstream_check_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_upstream_check_loc_conf_t *previous = parent;
    ngx_http_upstream_check_loc_conf_t *conf = child;

    ngx_conf_merge_uint_value(conf->format, previous->format, 0);
    return NGX_CONF_OK;
}

static char *
ngx_http_upstream_check_status(ngx_conf_t *cf, ngx_command_t *cmd, void *data)
{
    ngx_http_upstream_check_loc_conf_t *conf = data;
    ngx_http_core_loc_conf_t          *core;
    ngx_str_t                        *value;
    ngx_int_t                         format;

    core = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    core->handler = ngx_http_upstream_check_status_handler;
    if (cf->args->nelts == 2) {
        value = cf->args->elts;
        format = ngx_http_upstream_check_format(&value[1]);
        if (format == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid check format \"%V\"", &value[1]);
            return NGX_CONF_ERROR;
        }
        conf->format = (ngx_uint_t) format;
    }
    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_upstream_check_status_handler(ngx_http_request_t *r)
{
    ngx_http_upstream_check_loc_conf_t *conf;
    ngx_upstream_check_peers_t        *sources[1];
    ngx_int_t                         rc, argument;
    ngx_uint_t                        format, flag;
    ngx_str_t                         value;
    ngx_str_t types[] = { ngx_string("text/html"), ngx_string("text/plain"),
                         ngx_string("application/json") };

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }
    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }
    conf = ngx_http_get_module_loc_conf(r, ngx_http_upstream_check_module);
    format = conf->format;
    flag = 0;
    if (ngx_http_arg(r, (u_char *) "format", 6, &value) == NGX_OK) {
        argument = ngx_http_upstream_check_format(&value);
        if (argument != NGX_ERROR) {
            format = (ngx_uint_t) argument;
        } else {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "http upstream check, bad argument: \"%V\"", &value);
        }
    }
    if (ngx_http_arg(r, (u_char *) "status", 6, &value) == NGX_OK) {
        if (value.len == 4 && ngx_strncasecmp(value.data, (u_char *) "down", 4) == 0) {
            flag = 1;
        } else if (value.len == 2
                   && ngx_strncasecmp(value.data, (u_char *) "up", 2) == 0)
        {
            flag = 2;
        } else {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "http upstream check, bad argument: \"%V\"", &value);
        }
    }
    r->headers_out.content_type = types[format];
    if (http_peers_ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    sources[0] = http_peers_ctx;
    return ngx_healthcheck_status_request(r, sources, 1, flag,
                                         ngx_healthcheck_legacy_output, format);
}
