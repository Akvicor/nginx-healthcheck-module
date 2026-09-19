#ifndef NGX_HEALTHCHECK_CONFIG_H
#define NGX_HEALTHCHECK_CONFIG_H

#include "common.h.in"

/* HTTP/Stream 共用配置分配和指令解析，入口负责传入所属模块的配置对象。 */
void *ngx_healthcheck_create_main_conf(ngx_conf_t *cf);
void *ngx_healthcheck_create_srv_conf(ngx_conf_t *cf);
char *ngx_healthcheck_check(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
char *ngx_healthcheck_keepalive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
char *ngx_healthcheck_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

/* 注册持久配置地址，检查端口覆盖只改变探测地址。 */
ngx_uint_t ngx_healthcheck_add_peer(ngx_conf_t *cf,
    ngx_healthcheck_main_conf_t *main, ngx_upstream_check_srv_conf_t *conf,
    ngx_str_t *upstream, ngx_addr_t *address);

/* 注册属于候选 cycle 的独立共享区，解析阶段不发布运行绑定。 */
char *ngx_healthcheck_configure_zone(ngx_conf_t *cf,
    ngx_healthcheck_main_conf_t *main, void *tag, ngx_uint_t module);

#endif
