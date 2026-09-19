#include "configuration/ngx_healthcheck_config.h"

static ngx_int_t ngx_healthcheck_init_zone(ngx_shm_zone_t *zone, void *data);

char *
ngx_healthcheck_configure_zone(ngx_conf_t *cf, ngx_healthcheck_main_conf_t *main,
    void *tag, ngx_uint_t module)
{
    ngx_upstream_check_peers_t      *peers;
    ngx_upstream_check_peers_shm_t  *old;
    ngx_shm_zone_t                 *zone;
    u_char                         *last;

    peers = main->peers;
    peers->module = module;
    old = ngx_healthcheck_find_latest_peers_shm(cf->cycle->old_cycle, tag);
    if (old != NULL && old->generation == (ngx_uint_t) -1) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "healthcheck generation exhausted");
        return NGX_CONF_ERROR;
    }
    peers->generation = old ? old->generation + 1 : 1;
    peers->check_shm_name.data = ngx_pnalloc(cf->pool, 96);
    if (peers->check_shm_name.data == NULL) {
        return NGX_CONF_ERROR;
    }
    last = ngx_snprintf(peers->check_shm_name.data, 96, "ngx_%s_upstream_check#%ui",
                        module == NGX_HEALTHCHECK_HTTP ? "http" : "stream",
                        peers->generation);
    peers->check_shm_name.len = last - peers->check_shm_name.data;
    zone = ngx_shared_memory_add(cf, &peers->check_shm_name,
                                 ngx_max(main->check_shm_size, 1024 * 1024), tag);
    if (zone == NULL) {
        return NGX_CONF_ERROR;
    }
    zone->data = peers;
    zone->noreuse = 1;
    zone->init = ngx_healthcheck_init_zone;
    return NGX_CONF_OK;
}

static ngx_int_t
ngx_healthcheck_init_zone(ngx_shm_zone_t *zone, void *data)
{
    ngx_upstream_check_peers_t       *peers = zone->data;
    ngx_upstream_check_peers_shm_t   *shm, *old;
    ngx_upstream_check_peer_t        *peer;
    ngx_upstream_check_peer_shm_t    *previous, *target;
    ngx_healthcheck_peer_state_t      state;
    ngx_healthcheck_identity_index_t *index;
    ngx_healthcheck_worker_shm_t     *worker;
    ngx_slab_pool_t                  *slab;
    ngx_core_conf_t                  *core;
    ngx_pool_t                       *temporary;
    ngx_uint_t                        i, count;
    size_t                            size;
    ngx_str_t                         name;
    u_char                            buffer[128];

    temporary = NULL;
    /* zone 初始化发生在核心默认值和整个配置解析完成后，允许指令位于 http/stream 后。 */
    core = (ngx_core_conf_t *) ngx_get_conf(peers->cycle->conf_ctx, ngx_core_module);
    /* 核心允许零 worker 的静止 master，内部仍保留一个槽以安全建立共享状态。 */
    peers->workers = core->master && core->worker_processes > 0
                     ? (ngx_uint_t) core->worker_processes : 1;
    slab = (ngx_slab_pool_t *) zone->shm.addr;
    count = peers->peers.nelts;
    size = offsetof(ngx_upstream_check_peers_shm_t, peers);
    if (count > (SIZE_MAX - size) / sizeof(ngx_upstream_check_peer_shm_t)
        || peers->workers > SIZE_MAX / sizeof(ngx_healthcheck_worker_shm_t))
    {
        goto failed;
    }
    size += count * sizeof(ngx_upstream_check_peer_shm_t);
    shm = ngx_slab_calloc(slab, size);
    if (shm == NULL) {
        goto failed;
    }
    shm->magic = NGX_HEALTHCHECK_SHM_MAGIC;
    shm->version = NGX_HEALTHCHECK_SHM_VERSION;
    shm->generation = peers->generation;
    shm->number = count;
    shm->worker_count = peers->workers;
    shm->workers = ngx_slab_calloc(slab,
                            peers->workers * sizeof(ngx_healthcheck_worker_shm_t));
    if (shm->workers == NULL) {
        goto failed;
    }
    if (ngx_healthcheck_mutex_init(&shm->activation_mutex, &shm->activation_lock,
                                  peers->cycle->pool, &peers->check_shm_name)
        != NGX_OK)
    {
        goto failed;
    }
    name.data = buffer;
    for (i = 0; i < peers->workers; i++) {
        worker = &shm->workers[i];
        name.len = ngx_snprintf(buffer, sizeof(buffer), "%V-worker-%ui",
                                &peers->check_shm_name, i) - buffer;
        if (ngx_healthcheck_mutex_init(&worker->mutex, &worker->lock,
                                      peers->cycle->pool, &name) != NGX_OK)
        {
            goto failed;
        }
    }
    old = ngx_healthcheck_find_latest_peers_shm(peers->cycle->old_cycle, zone->tag);
    temporary = ngx_create_pool(ngx_pagesize, zone->shm.log);
    if (temporary == NULL) {
        goto failed;
    }
    index = ngx_healthcheck_identity_index_create(temporary, old);
    if (index == NULL) {
        goto failed;
    }
    peer = peers->peers.elts;
    for (i = 0; i < count; i++) {
        target = &shm->peers[i];
        name.len = ngx_snprintf(buffer, sizeof(buffer), "%V-peer-%ui",
                                &peers->check_shm_name, i) - buffer;
        if (ngx_healthcheck_copy_shm_identity(slab, target, &peer[i]) != NGX_OK
            || ngx_healthcheck_mutex_init(&target->mutex, &target->lock,
                                         peers->cycle->pool, &name) != NGX_OK)
        {
            goto failed;
        }
        ngx_memzero(&state, sizeof(state));
        state.health.down = peer[i].conf->default_down;
        previous = ngx_healthcheck_identity_index_take(index, &peer[i]);
        if (previous != NULL) {
            if (ngx_healthcheck_read_peer(previous, &state, 0) != NGX_OK) {
                ngx_memzero(&state, sizeof(state));
                state.health.down = peer[i].conf->default_down;
                ngx_log_error(NGX_LOG_WARN, zone->shm.log, 0,
                              "healthcheck snapshot unavailable for %V/%V; "
                              "initialize default_down=%ui",
                              peer[i].upstream_name, &peer[i].check_peer_addr->name,
                              state.health.down);
            }
        }
        if (peer[i].conf->check_type_conf->type == NGX_CHECK_TYPE_UDP) {
            state.health.last_check_delay = 0;
            state.health.avg_check_delay = 0;
            state.health.min_check_delay = 0;
            state.health.max_check_delay = 0;
            state.health.delay_total = 0;
            state.health.delay_sample_count = 0;
        }
        /* 只继承健康值，新 cycle 重新建立全部运行资格和首次启动期限。 */
        state.owner = NGX_INVALID_PID;
        state.worker = i % peers->workers;
        state.instance = 0;
        state.term = 0;
        state.round = 0;
        state.deadline = 0;
        state.phase = NGX_HEALTHCHECK_INITIAL;
        ngx_healthcheck_publish(target, &state);
        peer[i].shm = target;
    }
    peers->peers_shm = shm;
    ngx_destroy_pool(temporary);
    return NGX_OK;

failed:
    if (temporary != NULL) {
        ngx_destroy_pool(temporary);
    }
    ngx_log_error(NGX_LOG_EMERG, zone->shm.log, 0,
                  "healthcheck shared state initialization failed for %V; "
                  "check check_shm_size and peer/worker capacity", &zone->shm.name);
    return NGX_ERROR;
}
