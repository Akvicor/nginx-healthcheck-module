#ifndef _NGX_HEALTHCHECK_COMMON_H_INCLUDED_
#define _NGX_HEALTHCHECK_COMMON_H_INCLUDED_

#include <stdint.h>
#include <ngx_core.h>

typedef struct {
    ngx_msec_t  last;
    ngx_msec_t  avg;
    ngx_msec_t  min;
    ngx_msec_t  max;
    uint64_t    delay_total;
    uint64_t    delay_sample_count;
} ngx_healthcheck_delay_stats_t;

/* 在保持历史权重的前提下更新一次成功检查的延迟统计。 */
ngx_int_t ngx_healthcheck_update_delay(ngx_healthcheck_delay_stats_t *stats,
    ngx_msec_t delay);

#endif
