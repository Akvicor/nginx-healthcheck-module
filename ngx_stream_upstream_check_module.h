#ifndef _NGX_STREAM_UPSTREAM_CHECK_MODELE_H_INCLUDED_
#define _NGX_STREAM_UPSTREAM_CHECK_MODELE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>

/* 注册配置后端，返回与主动检查关联的索引。 */
ngx_uint_t ngx_stream_upstream_check_add_peer(ngx_conf_t *cf,
    ngx_stream_upstream_srv_conf_t *us, ngx_addr_t *peer);

/* 业务选路单次无锁读取已发布健康位。 */
ngx_uint_t ngx_stream_upstream_check_peer_down(ngx_uint_t index);

#endif //_NGX_STREAM_UPSTREAM_CHECK_MODELE_H_INCLUDED_
