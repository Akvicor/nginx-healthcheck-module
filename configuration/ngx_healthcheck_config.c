/*
 * Copyright (C) 2017- Changxun Zhou(changxunzhou@qq.com)
 * Copyright (C) 2010-2014 Weibin Yao (yaoweibin@gmail.com)
 * Copyright (C) 2010-2014 Alibaba Group Holding Limited
 */

#include "ngx_healthcheck_config.h"

static ngx_check_conf_t ngx_healthcheck_types[] = {
    { NGX_CHECK_TYPE_TCP, ngx_string("tcp"), ngx_null_string },
    { NGX_CHECK_TYPE_UDP, ngx_string("udp"), ngx_string("NGX_UDP_CHECKER") }
};

void *
ngx_healthcheck_create_main_conf(ngx_conf_t *cf)
{
    ngx_healthcheck_main_conf_t  *main;

    main = ngx_pcalloc(cf->pool, sizeof(*main));
    if (main == NULL) {
        return NULL;
    }
    main->peers = ngx_pcalloc(cf->pool, sizeof(*main->peers));
    if (main->peers == NULL
        || ngx_array_init(&main->peers->peers, cf->pool, 16,
                          sizeof(ngx_upstream_check_peer_t)) != NGX_OK)
    {
        return NULL;
    }
    main->peers->cycle = cf->cycle;
    return main;
}

void *
ngx_healthcheck_create_srv_conf(ngx_conf_t *cf)
{
    ngx_upstream_check_srv_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf != NULL) {
        conf->fall_count = 2;
        conf->rise_count = 5;
        conf->check_timeout = 1000;
        conf->check_keepalive_requests = NGX_CHECK_KEEPALIVE_REQUESTS_DEFAULT;
    }
    return conf;
}

char *
ngx_healthcheck_check(ngx_conf_t *cf, ngx_command_t *cmd, void *data)
{
    ngx_upstream_check_srv_conf_t  *conf = data;
    ngx_str_t                     *value, s;
    ngx_uint_t                     i, j, port, rise, fall, down;
    ngx_msec_t                     interval, timeout;

    port = 0;
    rise = 2;
    fall = 5;
    interval = 30000;
    timeout = 1000;
    down = NGX_CONF_UNSET_UINT;
    conf->tcp_reuse = 0;
    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        if (ngx_strncmp(value[i].data, "type=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;
            for (j = 0; j < 2; j++) {
                if (s.len == ngx_healthcheck_types[j].name.len
                    && ngx_strncmp(s.data, ngx_healthcheck_types[j].name.data,
                                   s.len) == 0)
                {
                    conf->check_type_conf = &ngx_healthcheck_types[j];
                    break;
                }
            }
            if (j == 2) {
                goto invalid;
            }
        } else if (ngx_strncmp(value[i].data, "port=", 5) == 0) {
            port = ngx_atoi(value[i].data + 5, value[i].len - 5);
            if (port == (ngx_uint_t) NGX_ERROR || port == 0) {
                goto invalid;
            }
        } else if (ngx_strncmp(value[i].data, "interval=", 9) == 0) {
            interval = ngx_atoi(value[i].data + 9, value[i].len - 9);
            if (interval == (ngx_msec_t) NGX_ERROR || interval == 0) {
                goto invalid;
            }
        } else if (ngx_strncmp(value[i].data, "timeout=", 8) == 0) {
            timeout = ngx_atoi(value[i].data + 8, value[i].len - 8);
            if (timeout == (ngx_msec_t) NGX_ERROR || timeout == 0) {
                goto invalid;
            }
        } else if (ngx_strncmp(value[i].data, "rise=", 5) == 0) {
            rise = ngx_atoi(value[i].data + 5, value[i].len - 5);
            if (rise == (ngx_uint_t) NGX_ERROR || rise == 0) {
                goto invalid;
            }
        } else if (ngx_strncmp(value[i].data, "fall=", 5) == 0) {
            fall = ngx_atoi(value[i].data + 5, value[i].len - 5);
            if (fall == (ngx_uint_t) NGX_ERROR || fall == 0) {
                goto invalid;
            }
        } else if (ngx_strncmp(value[i].data, "default_down=", 13) == 0) {
            s.data = value[i].data + 13;
            if (ngx_strcasecmp(s.data, (u_char *) "true") == 0) {
                down = 1;
            } else if (ngx_strcasecmp(s.data, (u_char *) "false") == 0) {
                down = 0;
            } else {
                goto invalid;
            }
        } else if (ngx_strncmp(value[i].data, "reuse=", 6) == 0) {
            s.len = value[i].len - 6;
            s.data = value[i].data + 6;
            if (s.len == 2 && ngx_strncasecmp(s.data, (u_char *) "on", 2) == 0) {
                conf->tcp_reuse = 1;
            } else if (s.len == 3
                       && ngx_strncasecmp(s.data, (u_char *) "off", 3) == 0)
            {
                conf->tcp_reuse = 0;
            } else {
                goto invalid;
            }
        } else {
            goto invalid;
        }
    }

    if (conf->check_type_conf == NULL) {
        conf->check_type_conf = &ngx_healthcheck_types[0];
    }
    if (conf->check_type_conf->type == NGX_CHECK_TYPE_UDP && conf->tcp_reuse) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "reuse=on is only valid with type=tcp");
        return NGX_CONF_ERROR;
    }
    conf->port = port;
    conf->check_interval = interval;
    conf->check_timeout = timeout;
    conf->rise_count = rise;
    conf->fall_count = fall;
    /* 默认值依据最终协议计算，显式设置和参数顺序彼此独立。 */
    conf->default_down = down == NGX_CONF_UNSET_UINT
                        ? conf->check_type_conf->type == NGX_CHECK_TYPE_TCP
                        : down;
    conf->send = conf->check_type_conf->default_send;
    return NGX_CONF_OK;

invalid:
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);
    return NGX_CONF_ERROR;
}

char *
ngx_healthcheck_keepalive(ngx_conf_t *cf, ngx_command_t *cmd, void *data)
{
    ngx_upstream_check_srv_conf_t  *conf = data;
    ngx_str_t                     *value;
    ngx_int_t                      requests;

    value = cf->args->elts;
    requests = ngx_atoi(value[1].data, value[1].len);
    if (requests == NGX_ERROR || requests == 0) {
        return "invalid value";
    }
    conf->check_keepalive_requests = requests;
    return NGX_CONF_OK;
}

char *
ngx_healthcheck_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *data)
{
    ngx_healthcheck_main_conf_t  *main = data;
    ngx_str_t                   *value;
    ssize_t                      size;

    if (main->check_shm_size) {
        return "is duplicate";
    }
    value = cf->args->elts;
    size = ngx_parse_size(&value[1]);
    if (size == NGX_ERROR) {
        return "invalid value";
    }
    main->check_shm_size = size;
    return NGX_CONF_OK;
}

ngx_uint_t
ngx_healthcheck_add_peer(ngx_conf_t *cf, ngx_healthcheck_main_conf_t *main,
    ngx_upstream_check_srv_conf_t *conf, ngx_str_t *upstream, ngx_addr_t *address)
{
    ngx_upstream_check_peer_t  *peer;
    ngx_addr_t                *check;
    size_t                     length;

    if (conf == NULL || conf->check_interval == 0) {
        return (ngx_uint_t) NGX_ERROR;
    }
    peer = ngx_array_push(&main->peers->peers);
    if (peer == NULL) {
        return (ngx_uint_t) NGX_ERROR;
    }
    ngx_memzero(peer, sizeof(*peer));
    peer->index = main->peers->peers.nelts - 1;
    peer->upstream_name = upstream;
    peer->conf = conf;
    peer->peer_addr = address;
    peer->check_peer_addr = address;
    if (conf->port == 0) {
        return peer->index;
    }

    check = ngx_pcalloc(cf->pool, sizeof(*check));
    if (check == NULL) {
        return (ngx_uint_t) NGX_ERROR;
    }
    check->socklen = address->socklen;
    check->sockaddr = ngx_palloc(cf->pool, check->socklen);
    if (check->sockaddr == NULL) {
        return (ngx_uint_t) NGX_ERROR;
    }
    ngx_memcpy(check->sockaddr, address->sockaddr, check->socklen);
    switch (check->sockaddr->sa_family) {
    case AF_INET:
        length = NGX_INET_ADDRSTRLEN + sizeof(":65535") - 1;
        break;
#if (NGX_HAVE_INET6)
    case AF_INET6:
        length = NGX_INET6_ADDRSTRLEN + sizeof(":65535") - 1;
        break;
#endif
    default:
        return (ngx_uint_t) NGX_ERROR;
    }
    ngx_inet_set_port(check->sockaddr, (in_port_t) conf->port);
    check->name.data = ngx_pnalloc(cf->pool, length);
    if (check->name.data == NULL) {
        return (ngx_uint_t) NGX_ERROR;
    }
    check->name.len = ngx_sock_ntop(check->sockaddr, check->socklen,
                                    check->name.data, length, 1);
    peer->check_peer_addr = check;
    return peer->index;
}
