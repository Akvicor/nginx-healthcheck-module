#include "ngx_healthcheck_state.h"

ngx_int_t
ngx_healthcheck_now(uint64_t *now)
{
    struct timespec  ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1 || ts.tv_sec < 0) {
        return NGX_ERROR;
    }
    *now = ngx_healthcheck_add_time(
        ngx_healthcheck_mul_time((uint64_t) ts.tv_sec, UINT64_C(1000000000)),
        (uint64_t) ts.tv_nsec);
    return NGX_OK;
}

uint64_t
ngx_healthcheck_add_time(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

uint64_t
ngx_healthcheck_mul_time(uint64_t a, uint64_t b)
{
    return b != 0 && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

/* 只有确认 PID 已不存在才恢复锁，暂停和权限不足均保留当前持有者。 */
static ngx_uint_t
ngx_healthcheck_recover_lock(ngx_shmtx_t *mutex)
{
#if (NGX_HAVE_ATOMIC_OPS)
    ngx_pid_t holder = (ngx_pid_t) *mutex->lock;

    if (holder > 0 && kill(holder, 0) == -1 && ngx_errno == NGX_ESRCH) {
        return ngx_shmtx_force_unlock(mutex, holder);
    }
#endif
    return 0;
}

ngx_int_t
ngx_healthcheck_lock(ngx_shmtx_t *mutex, uint64_t limit)
{
    uint64_t now;

    if (ngx_healthcheck_now(&now) != NGX_OK) {
        return NGX_AGAIN;
    }
    if (limit == 0) {
        limit = ngx_healthcheck_add_time(now, NGX_HEALTHCHECK_LOCK_NS);
    }
    for ( ;; ) {
        if (now >= limit) {
            return NGX_AGAIN;
        }
        if (ngx_shmtx_trylock(mutex)) {
            return NGX_OK;
        }
        (void) ngx_healthcheck_recover_lock(mutex);
        ngx_cpu_pause();
        if (ngx_healthcheck_now(&now) != NGX_OK) {
            return NGX_AGAIN;
        }
    }
}

ngx_int_t
ngx_healthcheck_mutex_init(ngx_shmtx_t *mutex, ngx_shmtx_sh_t *lock,
    ngx_pool_t *pool, ngx_str_t *name)
{
    u_char  *file;

    file = NULL;
#if !(NGX_HAVE_ATOMIC_OPS)
    file = ngx_pnalloc(pool, ngx_cycle->lock_file.len + name->len + 1);
    if (file == NULL) {
        return NGX_ERROR;
    }
    (void) ngx_sprintf(file, "%V%V%Z", &ngx_cycle->lock_file, name);
#endif
    /* 本协议只使用 trylock，避免为每个记录创建无用途的等待信号量。 */
    mutex->spin = (ngx_uint_t) -1;
    return ngx_shmtx_create(mutex, lock, file);
}

void
ngx_healthcheck_publish(ngx_upstream_check_peer_shm_t *peer,
    const ngx_healthcheck_peer_state_t *state)
{
    ngx_atomic_uint_t  slot;

    slot = (peer->published & 1) ^ 1;
    peer->slots[slot] = *state;
    ngx_memory_barrier();
    peer->published = slot | (state->health.down ? 2 : 0);
}

void
ngx_healthcheck_publish_worker(ngx_healthcheck_worker_shm_t *worker,
    const ngx_healthcheck_worker_state_t *state)
{
    ngx_atomic_uint_t  slot;

    slot = (worker->published & 1) ^ 1;
    worker->slots[slot] = *state;
    ngx_memory_barrier();
    worker->published = slot;
}

ngx_int_t
ngx_healthcheck_try_read_peer(ngx_upstream_check_peer_shm_t *peer,
    ngx_healthcheck_peer_state_t *state)
{
    if (!ngx_shmtx_trylock(&peer->mutex)) {
        if (!ngx_healthcheck_recover_lock(&peer->mutex)
            || !ngx_shmtx_trylock(&peer->mutex))
        {
            return NGX_AGAIN;
        }
    }
    *state = peer->slots[peer->published & 1];
    ngx_shmtx_unlock(&peer->mutex);
    return NGX_OK;
}

ngx_int_t
ngx_healthcheck_read_peer(ngx_upstream_check_peer_shm_t *peer,
    ngx_healthcheck_peer_state_t *state, uint64_t limit)
{
    if (ngx_healthcheck_lock(&peer->mutex, limit) != NGX_OK) {
        return NGX_AGAIN;
    }
    *state = peer->slots[peer->published & 1];
    ngx_shmtx_unlock(&peer->mutex);
    return NGX_OK;
}

ngx_int_t
ngx_healthcheck_read_worker(ngx_healthcheck_worker_shm_t *worker,
    ngx_healthcheck_worker_state_t *state, uint64_t limit)
{
    if (ngx_healthcheck_lock(&worker->mutex, limit) != NGX_OK) {
        return NGX_AGAIN;
    }
    *state = worker->slots[worker->published & 1];
    ngx_shmtx_unlock(&worker->mutex);
    return NGX_OK;
}
