/*
 * Copyright (C) 2017- Changxun Zhou(changxunzhou@qq.com)
 * Copyright (C) 2010-2014 Weibin Yao (yaoweibin@gmail.com)
 * Copyright (C) 2010-2014 Alibaba Group Holding Limited
 */

#include "ngx_healthcheck_config.h"

/* check 指令省略参数时的默认值。 */
#define NGX_HEALTHCHECK_INTERVAL_DEFAULT      5000
#define NGX_HEALTHCHECK_INTERVAL_MIN          100
#define NGX_HEALTHCHECK_TIMEOUT_DEFAULT_MAX   5000
#define NGX_HEALTHCHECK_FALL_DEFAULT          3
#define NGX_HEALTHCHECK_RISE_DEFAULT          2

/* 保留连接默认至少每 30s 重新握手一次，用于发现只有新建连接才能暴露的故障。 */
#define NGX_HEALTHCHECK_REHANDSHAKE_MIN       30000
#define NGX_HEALTHCHECK_KEEPINTVL_DEFAULT     1
#define NGX_HEALTHCHECK_KEEPCNT_DEFAULT       3

/* Linux 对 keepalive 参数的上限；超出时 setsockopt 失败，配置阶段直接拒绝。 */
#define NGX_HEALTHCHECK_KEEPALIVE_TIME_MAX    32767
#define NGX_HEALTHCHECK_KEEPCNT_MAX           127

/* 重新握手间隔以原生 timer 可表示的范围为上限。 */
#define NGX_HEALTHCHECK_REHANDSHAKE_MAX       (((ngx_msec_t) -1) >> 1)

/* 重新握手间隔的来源：关闭、模块默认或显式时间。 */
typedef enum {
    NGX_HEALTHCHECK_HANDSHAKE_OFF,
    NGX_HEALTHCHECK_HANDSHAKE_DEFAULT,
    NGX_HEALTHCHECK_HANDSHAKE_TIME
} ngx_healthcheck_handshake_e;

/* 解析过程中的参数；默认值和相互约束在整条指令解析完成后统一计算。 */
typedef struct {
    ngx_msec_t                   interval;
    ngx_msec_t                   timeout;
    ngx_flag_t                   timeout_set;
    ngx_healthcheck_handshake_e  reuse;
    ngx_msec_t                   reuse_time;
    ngx_flag_t                   keepalive;
    ngx_healthcheck_handshake_e  keepalive_handshake;
    ngx_msec_t                   keepalive_time;
    ngx_uint_t                   keepalive_idle;
    ngx_uint_t                   keepalive_interval;
    ngx_uint_t                   keepalive_count;
} ngx_healthcheck_check_args_t;

static ngx_check_conf_t ngx_healthcheck_types[] = {
    { NGX_CHECK_TYPE_TCP, ngx_string("tcp"), ngx_null_string },
    { NGX_CHECK_TYPE_UDP, ngx_string("udp"), ngx_string("NGX_UDP_CHECKER") }
};

static ngx_int_t ngx_healthcheck_parse_handshake(ngx_str_t *value,
    ngx_healthcheck_handshake_e *mode, ngx_msec_t *time);
static ngx_int_t ngx_healthcheck_parse_keepalive(ngx_str_t *value,
    ngx_healthcheck_check_args_t *args);
static char *ngx_healthcheck_finish_conf(ngx_conf_t *cf,
    ngx_upstream_check_srv_conf_t *conf, ngx_healthcheck_check_args_t *args);
static char *ngx_healthcheck_finish_keepalive(ngx_conf_t *cf,
    ngx_upstream_check_srv_conf_t *conf, ngx_healthcheck_check_args_t *args,
    uint64_t handshake);

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

/* 未配置 check 的 upstream 保持全零，以 check_interval == 0 识别。 */
void *
ngx_healthcheck_create_srv_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool, sizeof(ngx_upstream_check_srv_conf_t));
}

char *
ngx_healthcheck_check(ngx_conf_t *cf, ngx_command_t *cmd, void *data)
{
    ngx_upstream_check_srv_conf_t  *conf = data;
    ngx_healthcheck_check_args_t    args;
    ngx_str_t                     *value, s;
    ngx_uint_t                     i, j, port, rise, fall, down;
    ngx_int_t                      number;

    ngx_memzero(&args, sizeof(args));
    args.interval = NGX_HEALTHCHECK_INTERVAL_DEFAULT;
    port = 0;
    rise = NGX_HEALTHCHECK_RISE_DEFAULT;
    fall = NGX_HEALTHCHECK_FALL_DEFAULT;
    down = NGX_CONF_UNSET_UINT;
    conf->check_type_conf = NULL;
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
            number = ngx_atoi(value[i].data + 5, value[i].len - 5);
            if (number < 1 || number > 65535) {
                goto invalid;
            }
            port = (ngx_uint_t) number;
        } else if (ngx_strncmp(value[i].data, "interval=", 9) == 0) {
            number = ngx_atoi(value[i].data + 9, value[i].len - 9);
            if (number == NGX_ERROR || number == 0) {
                goto invalid;
            }
            args.interval = (ngx_msec_t) number;
        } else if (ngx_strncmp(value[i].data, "timeout=", 8) == 0) {
            number = ngx_atoi(value[i].data + 8, value[i].len - 8);
            if (number == NGX_ERROR || number == 0) {
                goto invalid;
            }
            args.timeout = (ngx_msec_t) number;
            args.timeout_set = 1;
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
            if (s.len == 0
                || ngx_healthcheck_parse_handshake(&s, &args.reuse, &args.reuse_time)
                   != NGX_OK)
            {
                goto invalid;
            }
        } else if (ngx_strncmp(value[i].data, "keepalive=", 10) == 0) {
            s.len = value[i].len - 10;
            s.data = value[i].data + 10;
            if (ngx_healthcheck_parse_keepalive(&s, &args) != NGX_OK) {
                goto invalid;
            }
        } else {
            goto invalid;
        }
    }

    if (conf->check_type_conf == NULL) {
        conf->check_type_conf = &ngx_healthcheck_types[0];
    }
    conf->port = port;
    conf->rise_count = rise;
    conf->fall_count = fall;
    conf->default_down = down == NGX_CONF_UNSET_UINT ? 0 : down;
    conf->send = conf->check_type_conf->default_send;
    return ngx_healthcheck_finish_conf(cf, conf, &args);

invalid:
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);
    return NGX_CONF_ERROR;
}

/* 解析 off、on（或空值）或秒级时间；时间支持 Nginx 秒级单位，拒绝 ms 与 0。 */
static ngx_int_t
ngx_healthcheck_parse_handshake(ngx_str_t *value,
    ngx_healthcheck_handshake_e *mode, ngx_msec_t *time)
{
    time_t  seconds;

    if (value->len == 3 && ngx_strncasecmp(value->data, (u_char *) "off", 3) == 0) {
        *mode = NGX_HEALTHCHECK_HANDSHAKE_OFF;
        return NGX_OK;
    }
    if (value->len == 0
        || (value->len == 2 && ngx_strncasecmp(value->data, (u_char *) "on", 2) == 0))
    {
        *mode = NGX_HEALTHCHECK_HANDSHAKE_DEFAULT;
        return NGX_OK;
    }
    seconds = ngx_parse_time(value, 1);
    if (seconds == (time_t) NGX_ERROR || seconds == 0
        || (uint64_t) seconds > NGX_HEALTHCHECK_REHANDSHAKE_MAX / 1000)
    {
        return NGX_ERROR;
    }
    *mode = NGX_HEALTHCHECK_HANDSHAKE_TIME;
    *time = (ngx_msec_t) seconds * 1000;
    return NGX_OK;
}

/*
 * keepalive=off|on|rehandshake[:idle[:intvl[:cnt]]]。
 * 首位为重新握手间隔（off 表示仅断开后重连，留空或 on 为默认），其后依次对应
 * TCP_KEEPIDLE、TCP_KEEPINTVL、TCP_KEEPCNT，留空使用模块默认值。
 */
static ngx_int_t
ngx_healthcheck_parse_keepalive(ngx_str_t *value, ngx_healthcheck_check_args_t *args)
{
    u_char     *p, *last, *end;
    ngx_str_t   field;
    ngx_uint_t  n;
    time_t      seconds;
    ngx_int_t   count;

    if (value->len == 0) {
        return NGX_ERROR;
    }
    if (value->len == 3 && ngx_strncasecmp(value->data, (u_char *) "off", 3) == 0) {
        args->keepalive = 0;
        return NGX_OK;
    }

    args->keepalive = 1;
    args->keepalive_handshake = NGX_HEALTHCHECK_HANDSHAKE_DEFAULT;
    args->keepalive_idle = 0;
    args->keepalive_interval = 0;
    args->keepalive_count = 0;

    p = value->data;
    last = value->data + value->len;
    for (n = 0; p <= last; n++) {
        end = ngx_strlchr(p, last, ':');
        if (end == NULL) {
            end = last;
        }
        field.data = p;
        field.len = end - p;

        if (n == 0) {
            if (ngx_healthcheck_parse_handshake(&field, &args->keepalive_handshake,
                                                &args->keepalive_time) != NGX_OK)
            {
                return NGX_ERROR;
            }
        } else if (n == 1 || n == 2) {
            if (field.len != 0) {
                seconds = ngx_parse_time(&field, 1);
                if (seconds == (time_t) NGX_ERROR || seconds == 0
                    || seconds > NGX_HEALTHCHECK_KEEPALIVE_TIME_MAX)
                {
                    return NGX_ERROR;
                }
                if (n == 1) {
                    args->keepalive_idle = (ngx_uint_t) seconds;
                } else {
                    args->keepalive_interval = (ngx_uint_t) seconds;
                }
            }
        } else if (n == 3) {
            if (field.len != 0) {
                count = ngx_atoi(field.data, field.len);
                if (count < 1 || count > NGX_HEALTHCHECK_KEEPCNT_MAX) {
                    return NGX_ERROR;
                }
                args->keepalive_count = (ngx_uint_t) count;
            }
        } else {
            return NGX_ERROR;
        }
        p = end + 1;
    }
    return NGX_OK;
}

/* 按整条指令计算默认值并校验参数之间的约束。 */
static char *
ngx_healthcheck_finish_conf(ngx_conf_t *cf, ngx_upstream_check_srv_conf_t *conf,
    ngx_healthcheck_check_args_t *args)
{
    uint64_t  handshake, reuse, keepalive, interval;

    interval = args->interval;
    if (interval < NGX_HEALTHCHECK_INTERVAL_MIN) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "check interval must be at least %ui ms",
                           (ngx_uint_t) NGX_HEALTHCHECK_INTERVAL_MIN);
        return NGX_CONF_ERROR;
    }
    if (args->timeout_set) {
        if (args->timeout >= args->interval) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "check timeout must be less than interval");
            return NGX_CONF_ERROR;
        }
    } else {
        /* 默认取 interval 的 90%（整数运算），并以 5s 为上限。 */
        args->timeout = ngx_min(NGX_HEALTHCHECK_TIMEOUT_DEFAULT_MAX,
                                args->interval - args->interval / 10);
    }
    conf->check_interval = args->interval;
    conf->check_timeout = args->timeout;

    if (conf->check_type_conf->type != NGX_CHECK_TYPE_TCP
        && (args->reuse != NGX_HEALTHCHECK_HANDSHAKE_OFF || args->keepalive))
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "reuse and keepalive are only valid with type=tcp");
        return NGX_CONF_ERROR;
    }

    /* off 视为无穷大，参与重新握手间隔的取较小值。 */
    handshake = ngx_max(NGX_HEALTHCHECK_REHANDSHAKE_MIN, interval * 2);
    handshake = ngx_min(handshake, NGX_HEALTHCHECK_REHANDSHAKE_MAX);
    reuse = UINT64_MAX;
    keepalive = UINT64_MAX;
    if (args->reuse == NGX_HEALTHCHECK_HANDSHAKE_DEFAULT) {
        reuse = handshake;
    } else if (args->reuse == NGX_HEALTHCHECK_HANDSHAKE_TIME) {
        reuse = args->reuse_time;
    }
    if (args->keepalive
        && args->keepalive_handshake == NGX_HEALTHCHECK_HANDSHAKE_DEFAULT)
    {
        keepalive = handshake;
    } else if (args->keepalive
               && args->keepalive_handshake == NGX_HEALTHCHECK_HANDSHAKE_TIME)
    {
        keepalive = args->keepalive_time;
    }
    if (reuse <= interval || keepalive <= interval) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "check rehandshake interval must be greater than interval");
        return NGX_CONF_ERROR;
    }

    conf->keep_connection = args->reuse != NGX_HEALTHCHECK_HANDSHAKE_OFF
                            || args->keepalive;
    handshake = ngx_min(reuse, keepalive);
    conf->rehandshake = handshake == UINT64_MAX ? 0 : (ngx_msec_t) handshake;
    conf->keepalive = args->keepalive;
    if (!args->keepalive) {
        return NGX_CONF_OK;
    }
    return ngx_healthcheck_finish_keepalive(cf, conf, args, handshake);
}

/* handshake 为实际重新握手间隔（毫秒），UINT64_MAX 表示仅断开后重连。 */
static char *
ngx_healthcheck_finish_keepalive(ngx_conf_t *cf, ngx_upstream_check_srv_conf_t *conf,
    ngx_healthcheck_check_args_t *args, uint64_t handshake)
{
#if !(NGX_HAVE_KEEPALIVE_TUNABLE)
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "check keepalive requires TCP_KEEPIDLE, TCP_KEEPINTVL and "
                       "TCP_KEEPCNT, which are not supported on this platform");
    return NGX_CONF_ERROR;
#else
    ngx_uint_t  idle;

    if (args->keepalive_idle) {
        idle = args->keepalive_idle;
        if (handshake != UINT64_MAX && (uint64_t) idle * 1000 >= handshake) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "check keepalive idle must be less than the "
                               "rehandshake interval");
            return NGX_CONF_ERROR;
        }
    } else {
        /* 默认每个 interval 一次 keepalive 往返，并保证早于重新握手发生。 */
        idle = (ngx_uint_t) ((conf->check_interval + 999) / 1000);
        if (handshake != UINT64_MAX) {
            idle = (ngx_uint_t) ngx_min((uint64_t) idle, (handshake - 1) / 1000);
        }
        if (idle < 1) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "check rehandshake interval is too short for "
                               "keepalive probes");
            return NGX_CONF_ERROR;
        }
        idle = ngx_min(idle, NGX_HEALTHCHECK_KEEPALIVE_TIME_MAX);
    }
    conf->keepalive_idle = idle;
    conf->keepalive_interval = args->keepalive_interval
                               ? args->keepalive_interval
                               : NGX_HEALTHCHECK_KEEPINTVL_DEFAULT;
    conf->keepalive_count = args->keepalive_count
                            ? args->keepalive_count
                            : NGX_HEALTHCHECK_KEEPCNT_DEFAULT;
    if (conf->keepalive_interval > conf->keepalive_idle) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "check keepalive interval must not exceed keepalive idle");
        return NGX_CONF_ERROR;
    }
    return NGX_CONF_OK;
#endif
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

    /* 地址校验和分配全部成功后才加入检查数组，失败时不留下已注册的探测记录。 */
    check = address;
    if (conf->port != 0) {
        switch (address->sockaddr->sa_family) {
        case AF_INET:
            length = NGX_INET_ADDRSTRLEN + sizeof(":65535") - 1;
            break;
#if (NGX_HAVE_INET6)
        case AF_INET6:
            length = NGX_INET6_ADDRSTRLEN + sizeof(":65535") - 1;
            break;
#endif
        default:
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "check port=%ui cannot be used with server \"%V\"",
                               conf->port, &address->name);
            return (ngx_uint_t) NGX_ERROR;
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
        ngx_inet_set_port(check->sockaddr, (in_port_t) conf->port);
        check->name.data = ngx_pnalloc(cf->pool, length);
        if (check->name.data == NULL) {
            return (ngx_uint_t) NGX_ERROR;
        }
        check->name.len = ngx_sock_ntop(check->sockaddr, check->socklen,
                                        check->name.data, length, 1);
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
    peer->check_peer_addr = check;
    return peer->index;
}
