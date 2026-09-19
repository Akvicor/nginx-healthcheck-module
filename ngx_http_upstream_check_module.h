#ifndef _NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_
#define _NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

/* 注册配置地址；返回值关联主动检查记录，无检查或失败时返回无效索引。 */
ngx_uint_t ngx_http_upstream_check_add_peer(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us, ngx_addr_t *peer);

/* 业务选路单次无锁读取已发布健康位。 */
ngx_uint_t ngx_http_upstream_check_peer_down(ngx_uint_t index);

/* fair 通过此配置查询区分未启用检查与实际注册失败。 */
ngx_uint_t ngx_http_upstream_check_enabled(ngx_http_upstream_srv_conf_t *upstream);


#endif //_NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_
