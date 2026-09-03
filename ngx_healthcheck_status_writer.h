#ifndef _NGX_HEALTHCHECK_STATUS_WRITER_H_INCLUDED_
#define _NGX_HEALTHCHECK_STATUS_WRITER_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HEALTHCHECK_STATUS_BLOCK_SIZE  (128 * 1024)


/* 状态 writer 在请求池中按需构建固定大小的数据块链。 */
typedef struct {
    ngx_pool_t   *pool;
    ngx_chain_t  *head;
    ngx_chain_t  *tail;
    off_t         length;
    ngx_uint_t    blocks;
} ngx_healthcheck_status_writer_t;


/* 初始化 writer；空输出不会分配数据块。 */
void ngx_healthcheck_status_writer_init(ngx_healthcheck_status_writer_t *writer,
    ngx_pool_t *pool);

/* 追加任意长度的数据，数据可以连续跨越多个固定块。 */
ngx_int_t ngx_healthcheck_status_writer_append(
    ngx_healthcheck_status_writer_t *writer, const u_char *data, size_t len);

/* 格式化状态输出当前使用的 %V、%s、%ui、%M 和 %% 格式。 */
ngx_int_t ngx_healthcheck_status_writer_printf(
    ngx_healthcheck_status_writer_t *writer, const char *fmt, ...);

/* 设置链尾标志并统一发送 GET 或 HEAD 响应。 */
ngx_int_t ngx_healthcheck_status_writer_send(ngx_http_request_t *r,
    ngx_healthcheck_status_writer_t *writer);


#endif /* _NGX_HEALTHCHECK_STATUS_WRITER_H_INCLUDED_ */
