#include "ngx_healthcheck_runtime.h"

static void ngx_healthcheck_watch(ngx_event_t *event);
static void ngx_healthcheck_begin(ngx_event_t *event);
static ngx_int_t ngx_healthcheck_submit(ngx_healthcheck_probe_t *probe,
    uint64_t limit);

void
ngx_healthcheck_timer(ngx_event_t *event, uint64_t delay)
{
    uint64_t maximum;

    maximum = ((ngx_msec_t) -1) >> 1;
    event->cancelable = 1;
    ngx_add_timer(event, (ngx_msec_t) ngx_min(delay, maximum));
}

static ngx_int_t
ngx_healthcheck_stopping(ngx_healthcheck_runtime_t *runtime)
{
    if (runtime->stopping || ngx_quit || ngx_exiting || ngx_terminate) {
        ngx_healthcheck_runtime_stop(runtime->peers);
        return 1;
    }
    return 0;
}

static ngx_int_t
ngx_healthcheck_register(ngx_healthcheck_runtime_t *runtime, uint64_t limit)
{
    ngx_upstream_check_peers_shm_t  *shm;
    ngx_healthcheck_worker_shm_t    *worker;
    ngx_healthcheck_worker_state_t   state;
    uint64_t                        now;

    shm = runtime->peers->peers_shm;
    if (ngx_healthcheck_now(&now) != NGX_OK) {
        return NGX_AGAIN;
    }
    if (!shm->activated) {
        if (ngx_healthcheck_lock(&shm->activation_mutex, limit) != NGX_OK) {
            return NGX_AGAIN;
        }
        if (!shm->activated) {
            shm->activated_at = now / 1000000;
            ngx_memory_barrier();
            shm->activated = 1;
        }
        ngx_shmtx_unlock(&shm->activation_mutex);
    }
    worker = &shm->workers[runtime->worker];
    if (ngx_healthcheck_lock(&worker->mutex, limit) != NGX_OK) {
        return NGX_AGAIN;
    }
    state = worker->slots[worker->published & 1];
    if (!runtime->registered) {
        if (state.instance == UINT64_MAX) {
            ngx_shmtx_unlock(&worker->mutex);
            ngx_log_error(NGX_LOG_ERR, runtime->cycle->log, 0,
                          "healthcheck worker instance exhausted");
            return NGX_ERROR;
        }
        state.instance++;
        state.pid = ngx_pid;
        state.exiting = 0;
    } else if (state.instance != runtime->instance || state.pid != ngx_pid
               || state.exiting)
    {
        ngx_shmtx_unlock(&worker->mutex);
        return NGX_DECLINED;
    }
    state.heartbeat = now / 1000000;
    ngx_healthcheck_publish_worker(worker, &state);
    runtime->instance = state.instance;
    runtime->registered = 1;
    ngx_shmtx_unlock(&worker->mutex);
    return NGX_OK;
}

static void
ngx_healthcheck_start_timers(ngx_healthcheck_runtime_t *runtime)
{
    ngx_upstream_check_peer_t *peer;
    ngx_uint_t                i;
    uint64_t                  delay;

    if (runtime->timers_started || !runtime->registered) {
        return;
    }
    runtime->timers_started = 1;
    peer = runtime->peers->peers.elts;
    for (i = 0; i < runtime->peers->peers.nelts; i++) {
        if (i % runtime->peers->workers != runtime->worker) {
            continue;
        }
        delay = ngx_max(peer[i].conf->check_interval, 1000);
        ngx_healthcheck_timer(&peer[i].check_ev, (uint64_t) ngx_random() % delay);
    }
}

ngx_int_t
ngx_healthcheck_runtime_init(ngx_upstream_check_peers_t *peers)
{
    ngx_healthcheck_runtime_t  *runtime;
    ngx_upstream_check_peer_t  *peer;
    ngx_core_conf_t           *core;
    ngx_uint_t                 i;
    uint64_t                   now;

    if (peers == NULL || peers->peers.nelts == 0) {
        return NGX_OK;
    }
    runtime = ngx_pcalloc(peers->cycle->pool, sizeof(*runtime));
    if (runtime == NULL) {
        return NGX_ERROR;
    }
    peers->runtime = runtime;
    runtime->peers = peers;
    runtime->cycle = peers->cycle;
    runtime->worker = ngx_process == NGX_PROCESS_SINGLE ? 0 : ngx_worker;
    runtime->name = peers->module == NGX_HEALTHCHECK_HTTP ? "http" : "stream";
    runtime->log_level = peers->module == NGX_HEALTHCHECK_HTTP
                         ? NGX_LOG_DEBUG_HTTP : NGX_LOG_DEBUG_STREAM;
    core = (ngx_core_conf_t *) ngx_get_conf(peers->cycle->conf_ctx, ngx_core_module);
    runtime->grace = ngx_max(UINT64_C(1000),
                            ngx_healthcheck_mul_time(core->timer_resolution, 2));
    runtime->interval = ngx_max(100, core->timer_resolution);
    runtime->preference = ngx_max(runtime->grace,
        ngx_healthcheck_mul_time(peers->peers.nelts / NGX_HEALTHCHECK_BATCH
                    + (peers->peers.nelts % NGX_HEALTHCHECK_BATCH != 0),
                    runtime->interval));
    runtime->watch.handler = ngx_healthcheck_watch;
    runtime->watch.data = runtime;
    runtime->watch.log = peers->cycle->log;
    runtime->watch.cancelable = 1;
    peer = peers->peers.elts;
    for (i = 0; i < peers->peers.nelts; i++) {
        peer[i].runtime = runtime;
        peer[i].check_ev.handler = ngx_healthcheck_begin;
        peer[i].check_ev.data = &peer[i];
        peer[i].check_ev.log = peers->cycle->log;
        peer[i].check_ev.cancelable = 1;
    }
    srandom(ngx_pid);
    if (ngx_healthcheck_now(&now) == NGX_OK) {
        (void) ngx_healthcheck_register(runtime,
                        ngx_healthcheck_add_time(now, NGX_HEALTHCHECK_LOCK_NS));
    }
    ngx_healthcheck_start_timers(runtime);
    ngx_healthcheck_timer(&runtime->watch, runtime->interval);
    return NGX_OK;
}

void
ngx_healthcheck_runtime_stop(ngx_upstream_check_peers_t *peers)
{
    ngx_healthcheck_runtime_t       *runtime;
    ngx_upstream_check_peer_t       *peer;
    ngx_healthcheck_worker_shm_t    *worker;
    ngx_healthcheck_worker_state_t   state;
    ngx_uint_t                      i;

    if (peers == NULL || peers->runtime == NULL) {
        return;
    }
    runtime = peers->runtime;
    runtime->stopping = 1;
    if (runtime->cleaned) {
        return;
    }
    if (runtime->watch.timer_set) {
        ngx_del_timer(&runtime->watch);
    }
    if (runtime->watch.posted) {
        ngx_delete_posted_event(&runtime->watch);
    }
    peer = peers->peers.elts;
    for (i = 0; i < peers->peers.nelts; i++) {
        if (peer[i].check_ev.timer_set) {
            ngx_del_timer(&peer[i].check_ev);
        }
        if (peer[i].check_ev.posted) {
            ngx_delete_posted_event(&peer[i].check_ev);
        }
        if (peer[i].probe != NULL) {
            ngx_healthcheck_probe_cleanup(peer[i].probe, 0);
            peer[i].probe->active = 0;
            peer[i].probe->pending = 0;
        }
    }
    /* 退出只尝试登记一次，释放本地资源不依赖旧 peer 锁。 */
    if (runtime->registered) {
        worker = &peers->peers_shm->workers[runtime->worker];
        if (ngx_healthcheck_lock(&worker->mutex, 0) == NGX_OK) {
            state = worker->slots[worker->published & 1];
            if (state.pid == ngx_pid && state.instance == runtime->instance) {
                state.exiting = 1;
                ngx_healthcheck_publish_worker(worker, &state);
            }
            ngx_shmtx_unlock(&worker->mutex);
        }
    }
    runtime->cleaned = 1;
}

/* 固定先获取自身登记锁，再获取 peer 锁；成功返回时两者均由调用方持有。 */
static ngx_int_t
ngx_healthcheck_owned_lock(ngx_healthcheck_probe_t *probe,
    ngx_healthcheck_peer_state_t *state, uint64_t limit)
{
    ngx_healthcheck_runtime_t       *runtime = probe->peer->runtime;
    ngx_healthcheck_worker_shm_t    *worker;
    ngx_healthcheck_worker_state_t  *registration;
    uint64_t                        now;

    if (runtime->stopping || ngx_quit || ngx_exiting || ngx_terminate
        || probe->generation != runtime->peers->generation)
    {
        return NGX_DECLINED;
    }
    if (limit == 0 && runtime->budget != 0) {
        limit = runtime->budget;
    }
    if (limit == 0) {
        if (ngx_healthcheck_now(&now) != NGX_OK) {
            return NGX_AGAIN;
        }
        limit = ngx_healthcheck_add_time(now, NGX_HEALTHCHECK_LOCK_NS);
    }
    worker = &runtime->peers->peers_shm->workers[runtime->worker];
    if (ngx_healthcheck_lock(&worker->mutex, limit) != NGX_OK) {
        return NGX_AGAIN;
    }
    registration = &worker->slots[worker->published & 1];
    if (registration->instance != probe->instance || registration->exiting
        || registration->pid != ngx_pid)
    {
        ngx_shmtx_unlock(&worker->mutex);
        return NGX_DECLINED;
    }
    if (ngx_healthcheck_lock(&probe->peer->shm->mutex, limit) != NGX_OK) {
        ngx_shmtx_unlock(&worker->mutex);
        return NGX_AGAIN;
    }
    *state = probe->peer->shm->slots[probe->peer->shm->published & 1];
    if (state->worker != runtime->worker || state->instance != probe->instance
        || state->term != probe->term || state->round != probe->round
        || state->owner != ngx_pid)
    {
        ngx_shmtx_unlock(&probe->peer->shm->mutex);
        ngx_shmtx_unlock(&worker->mutex);
        return NGX_DECLINED;
    }
    return NGX_OK;
}

static void
ngx_healthcheck_owned_unlock(ngx_healthcheck_probe_t *probe)
{
    ngx_healthcheck_runtime_t  *runtime = probe->peer->runtime;

    ngx_shmtx_unlock(&probe->peer->shm->mutex);
    ngx_shmtx_unlock(&runtime->peers->peers_shm->workers[runtime->worker].mutex);
}

ngx_int_t
ngx_healthcheck_probe_valid(ngx_healthcheck_probe_t *probe)
{
    ngx_healthcheck_peer_state_t  state;
    ngx_int_t                    rc;

    rc = ngx_healthcheck_owned_lock(probe, &state, 0);
    if (rc == NGX_OK) {
        ngx_healthcheck_owned_unlock(probe);
    }
    return rc;
}

ngx_int_t
ngx_healthcheck_probe_waiting(ngx_healthcheck_probe_t *probe)
{
    ngx_healthcheck_peer_state_t  state;
    ngx_int_t                    rc;

    if (probe->waiting_published) {
        return NGX_OK;
    }
    rc = ngx_healthcheck_owned_lock(probe, &state, 0);
    if (rc != NGX_OK) {
        return rc;
    }
    if (state.phase == NGX_HEALTHCHECK_PREPARING) {
        state.phase = NGX_HEALTHCHECK_INFLIGHT;
        state.deadline = probe->deadline;
        ngx_healthcheck_publish(probe->peer->shm, &state);
    }
    probe->waiting_published = 1;
    ngx_healthcheck_owned_unlock(probe);
    return NGX_OK;
}

static ngx_int_t
ngx_healthcheck_submit(ngx_healthcheck_probe_t *probe, uint64_t limit)
{
    ngx_healthcheck_peer_state_t     state;
    ngx_healthcheck_delay_stats_t    delay;
    ngx_healthcheck_runtime_t       *runtime = probe->peer->runtime;
    ngx_upstream_check_srv_conf_t   *conf = probe->peer->conf;
    ngx_uint_t                      changed, previous;
    ngx_int_t                       rc;
    uint64_t                        now;

    rc = ngx_healthcheck_owned_lock(probe, &state, limit);
    if (rc != NGX_OK) {
        return rc;
    }
    if ((state.phase != NGX_HEALTHCHECK_PREPARING
         && state.phase != NGX_HEALTHCHECK_INFLIGHT)
        || ngx_quit || ngx_exiting || ngx_terminate)
    {
        ngx_healthcheck_owned_unlock(probe);
        return NGX_DECLINED;
    }
    if (ngx_healthcheck_now(&now) != NGX_OK) {
        ngx_healthcheck_owned_unlock(probe);
        return NGX_AGAIN;
    }
    previous = state.health.down;
    if (probe->result) {
        state.health.fall_count = 0;
        state.health.rise_count = state.health.rise_count < (ngx_uint_t) -1
                                  ? state.health.rise_count + 1 : conf->rise_count;
        if (state.health.rise_count >= conf->rise_count) {
            state.health.down = 0;
        }
        if (probe->sample) {
            delay.last = state.health.last_check_delay;
            delay.avg = state.health.avg_check_delay;
            delay.min = state.health.min_check_delay;
            delay.max = state.health.max_check_delay;
            delay.delay_total = state.health.delay_total;
            delay.delay_sample_count = state.health.delay_sample_count;
            if (ngx_healthcheck_update_delay(&delay, probe->delay) == NGX_OK) {
                state.health.last_check_delay = delay.last;
                state.health.avg_check_delay = delay.avg;
                state.health.min_check_delay = delay.min;
                state.health.max_check_delay = delay.max;
                state.health.delay_total = delay.delay_total;
                state.health.delay_sample_count = delay.delay_sample_count;
            }
        }
    } else {
        state.health.rise_count = 0;
        state.health.fall_count = state.health.fall_count < (ngx_uint_t) -1
                                  ? state.health.fall_count + 1 : conf->fall_count;
        if (state.health.fall_count >= conf->fall_count) {
            state.health.down = 1;
        }
    }
    state.health.access_time = now / 1000000;
    state.phase = NGX_HEALTHCHECK_WAITING;
    state.deadline = ngx_healthcheck_add_time(state.health.access_time,
        ngx_healthcheck_add_time(conf->check_interval,
                                ngx_max(conf->check_interval / 2, 1)));
    changed = previous != state.health.down;
    ngx_healthcheck_publish(probe->peer->shm, &state);
    ngx_healthcheck_owned_unlock(probe);
    probe->pending = 0;

    if (changed) {
        if (runtime->peers->module == NGX_HEALTHCHECK_HTTP) {
            ngx_log_error(NGX_LOG_ERR, runtime->cycle->log, 0,
                          "[ngx_healthcheck:http] %s check peer: %V",
                          state.health.down ? "disable" : "enable",
                          &probe->peer->check_peer_addr->name);
        } else {
            ngx_log_error(NGX_LOG_NOTICE, runtime->cycle->log, 0,
                          "[ngx_healthcheck:stream][status-update] "
                          "change status to %s for peer: %V",
                          state.health.down ? "DOWN" : "UP",
                          &probe->peer->check_peer_addr->name);
        }
    }
    ngx_log_debug8(runtime->log_level, runtime->cycle->log, probe->error,
                   "healthcheck %s result=%s status=%s upstream=%V peer=%V "
                   "stage=%s source=%s reason=%s",
                   runtime->name, probe->result ? "S" : "F",
                   state.health.down ? "down" : "up", probe->peer->upstream_name,
                   &probe->peer->check_peer_addr->name, probe->stage,
                   probe->source, probe->reason);
    return NGX_OK;
}

void
ngx_healthcheck_finish(ngx_healthcheck_probe_t *probe, ngx_uint_t result,
    const char *reason, const char *source, ngx_err_t error)
{
    ngx_int_t  rc;
    uint64_t   now;

    if (!probe->active || probe->pending) {
        return;
    }
    probe->active = 0;
    probe->pending = 1;
    probe->result = result;
    probe->reason = reason;
    probe->source = source;
    probe->error = error;
    probe->sample = result && probe->peer->conf->check_type_conf->type
                             == NGX_CHECK_TYPE_TCP
                    && probe->pc.connection && probe->pc.connection->requests == 0;
    if (probe->sample) {
        if (ngx_healthcheck_now(&now) != NGX_OK) {
            probe->sample = 0;
        } else {
            probe->delay = (ngx_msec_t) ngx_min(now / 1000000 - probe->started,
                                                (ngx_msec_t) -1);
        }
    }
    rc = ngx_healthcheck_submit(probe, 0);
    if (rc == NGX_DECLINED) {
        probe->pending = 0;
    }
    if (result && probe->pc.connection) {
        probe->pc.connection->requests++;
    }
    ngx_healthcheck_probe_cleanup(probe, rc == NGX_OK && result);
}

/* 首次等待由全局激活时间锚定，其余阶段直接使用已发布期限。 */
static uint64_t
ngx_healthcheck_deadline(ngx_upstream_check_peer_t *peer,
    ngx_healthcheck_peer_state_t *state)
{
    if (state->phase != NGX_HEALTHCHECK_INITIAL) {
        return state->deadline;
    }
    ngx_memory_barrier();
    return ngx_healthcheck_add_time(peer->runtime->peers->peers_shm->activated_at,
        ngx_healthcheck_add_time(ngx_max(peer->conf->check_interval, 1000),
                                ngx_max(peer->conf->check_interval / 2, 1)));
}

static ngx_int_t
ngx_healthcheck_claim(ngx_upstream_check_peer_t *peer, ngx_uint_t takeover,
    ngx_healthcheck_peer_state_t *observed, uint64_t limit)
{
    ngx_healthcheck_runtime_t       *runtime = peer->runtime;
    ngx_healthcheck_worker_shm_t    *worker;
    ngx_healthcheck_worker_state_t  *registration;
    ngx_healthcheck_peer_state_t     state;
    ngx_healthcheck_probe_t         *probe;
    ngx_connection_t               *connection;
    uint64_t                        now, milliseconds;
    ngx_int_t                       rc;

    if (!runtime->registered || ngx_healthcheck_stopping(runtime)) {
        return NGX_DECLINED;
    }
    if (peer->probe == NULL) {
        peer->probe = ngx_pcalloc(runtime->cycle->pool, sizeof(*peer->probe));
        if (peer->probe == NULL) {
            return NGX_ERROR;
        }
        peer->probe->peer = peer;
    }
    probe = peer->probe;
    if (probe->active || probe->pending) {
        return NGX_AGAIN;
    }
    if (ngx_healthcheck_now(&now) != NGX_OK) {
        return NGX_AGAIN;
    }
    milliseconds = now / 1000000;
    if (limit == 0) {
        limit = ngx_healthcheck_add_time(now, NGX_HEALTHCHECK_LOCK_NS);
    }
    worker = &runtime->peers->peers_shm->workers[runtime->worker];
    if (ngx_healthcheck_lock(&worker->mutex, limit) != NGX_OK) {
        return NGX_AGAIN;
    }
    registration = &worker->slots[worker->published & 1];
    if (registration->instance != runtime->instance || registration->exiting
        || registration->pid != ngx_pid)
    {
        ngx_shmtx_unlock(&worker->mutex);
        return NGX_DECLINED;
    }
    if (ngx_healthcheck_lock(&peer->shm->mutex, limit) != NGX_OK) {
        ngx_shmtx_unlock(&worker->mutex);
        return NGX_AGAIN;
    }
    state = peer->shm->slots[peer->shm->published & 1];
    rc = NGX_AGAIN;
    if (takeover) {
        if (state.term != observed->term || state.round != observed->round
            || state.phase != observed->phase || state.deadline != observed->deadline
            || milliseconds < ngx_healthcheck_add_time(
                    ngx_healthcheck_deadline(peer, &state), runtime->grace)
            || (state.worker == runtime->worker
                && state.instance == runtime->instance))
        {
            goto unlock;
        }
    } else {
        if (state.worker != runtime->worker
            || (state.instance != 0 && state.instance != runtime->instance))
        {
            rc = NGX_DECLINED;
            goto unlock;
        }
        if ((state.phase != NGX_HEALTHCHECK_INITIAL
             && state.phase != NGX_HEALTHCHECK_WAITING)
            || (state.health.access_time != 0
                && milliseconds < ngx_healthcheck_add_time(state.health.access_time,
                                                           peer->conf->check_interval)))
        {
            goto unlock;
        }
    }
    if (state.round == UINT64_MAX
        || ((state.worker != runtime->worker || state.instance != runtime->instance)
            && state.term == UINT64_MAX))
    {
        rc = NGX_ERROR;
        goto unlock;
    }
    if (state.worker != runtime->worker || state.instance != runtime->instance) {
        state.term++;
    }
    state.worker = runtime->worker;
    state.owner = ngx_pid;
    state.instance = runtime->instance;
    state.round++;
    state.phase = NGX_HEALTHCHECK_PREPARING;
    state.deadline = ngx_healthcheck_add_time(milliseconds, peer->conf->check_timeout);
    ngx_healthcheck_publish(peer->shm, &state);
    rc = NGX_OK;

unlock:
    ngx_shmtx_unlock(&peer->shm->mutex);
    ngx_shmtx_unlock(&worker->mutex);
    if (rc != NGX_OK) {
        return rc;
    }
    /* 排队的 idle 通知属于旧轮次，清除后才让保留连接关联新身份。 */
    connection = probe->pc.connection;
    if (connection != NULL) {
        if (connection->read->posted) {
            ngx_delete_posted_event(connection->read);
        }
        if (connection->write->posted) {
            ngx_delete_posted_event(connection->write);
        }
    }
    probe->instance = state.instance;
    probe->term = state.term;
    probe->round = state.round;
    probe->generation = runtime->peers->generation;
    probe->started = milliseconds;
    probe->deadline = state.deadline;
    probe->active = 1;
    probe->attempted = 0;
    probe->sent = 0;
    probe->sending = 1;
    probe->prepared = 0;
    probe->sample = 0;
    probe->waiting_published = 0;
    probe->stage = "prepare";
    probe->reason = "pending";
    probe->source = "none";
    probe->error = 0;
    probe->timeout.handler = ngx_healthcheck_timeout;
    probe->timeout.data = probe;
    probe->timeout.log = runtime->cycle->log;
    probe->timeout.timedout = 0;
    ngx_healthcheck_timer(&probe->timeout, peer->conf->check_timeout);
    if (!peer->check_ev.timer_set) {
        ngx_healthcheck_timer(&peer->check_ev, ngx_max(peer->conf->check_interval / 2, 1));
    }
    ngx_healthcheck_probe_start(probe);
    return NGX_OK;
}

static void
ngx_healthcheck_begin(ngx_event_t *event)
{
    ngx_upstream_check_peer_t *peer = event->data;
    ngx_int_t                 rc;

    if (ngx_healthcheck_stopping(peer->runtime)) {
        return;
    }
    ngx_healthcheck_timer(event, ngx_max(peer->conf->check_interval / 2, 1));
    if (peer->probe != NULL && (peer->probe->active || peer->probe->pending)) {
        return;
    }
    rc = ngx_healthcheck_claim(peer, 0, NULL, 0);
    if (rc == NGX_DECLINED && event->timer_set) {
        ngx_del_timer(event);
        if (peer->probe != NULL) {
            ngx_healthcheck_probe_cleanup(peer->probe, 0);
        }
    }
}

/* 稳定评分输入与机器字长、字节序无关。 */
static uint32_t
ngx_healthcheck_score(ngx_upstream_check_peer_t *peer, ngx_uint_t worker)
{
    u_char     key[17];
    uint64_t   value;
    ngx_uint_t i;

    key[0] = (u_char) peer->runtime->peers->module;
    value = peer->index;
    for (i = 0; i < 8; i++) {
        key[8 - i] = (u_char) (value >> (i * 8));
    }
    value = worker;
    for (i = 0; i < 8; i++) {
        key[16 - i] = (u_char) (value >> (i * 8));
    }
    return ngx_murmur_hash2(key, sizeof(key));
}

static ngx_uint_t
ngx_healthcheck_preferred(ngx_upstream_check_peer_t *peer,
    ngx_healthcheck_peer_state_t *state, uint64_t now, uint64_t limit)
{
    ngx_healthcheck_runtime_t       *runtime = peer->runtime;
    ngx_healthcheck_worker_state_t   candidate;
    ngx_uint_t                      i, best;
    uint32_t                        score, highest;
    uint64_t                        fresh;

    best = (ngx_uint_t) -1;
    highest = 0;
    for (i = 0; i < runtime->peers->workers; i++) {
        if (ngx_healthcheck_now(&fresh) != NGX_OK || fresh >= limit) {
            return 0;
        }
        if (ngx_healthcheck_read_worker(&runtime->peers->peers_shm->workers[i],
                                       &candidate, limit) != NGX_OK)
        {
            continue;
        }
        if (candidate.instance == 0 || candidate.exiting
            || (i == state->worker && candidate.instance == state->instance)
            || now < candidate.heartbeat || now - candidate.heartbeat >= runtime->grace)
        {
            continue;
        }
        score = ngx_healthcheck_score(peer, i);
        if (best == (ngx_uint_t) -1 || score > highest) {
            best = i;
            highest = score;
        }
    }
    return best == runtime->worker;
}

static void
ngx_healthcheck_watch(ngx_event_t *event)
{
    ngx_healthcheck_runtime_t    *runtime = event->data;
    ngx_upstream_check_peer_t    *peers, *peer;
    ngx_healthcheck_peer_state_t state;
    ngx_healthcheck_probe_t     *probe;
    ngx_uint_t                  visited, count;
    ngx_int_t                   rc;
    uint64_t                    now, limit, eligible;

    if (ngx_healthcheck_stopping(runtime)) {
        return;
    }
    if (ngx_healthcheck_now(&now) != NGX_OK) {
        goto rearm;
    }
    limit = ngx_healthcheck_add_time(now, NGX_HEALTHCHECK_LOCK_NS);
    runtime->budget = limit;
    rc = ngx_healthcheck_register(runtime, limit);
    if (rc == NGX_DECLINED) {
        ngx_healthcheck_runtime_stop(runtime->peers);
        return;
    }
    if (!runtime->registered) {
        goto rearm;
    }
    peers = runtime->peers->peers.elts;
    count = runtime->peers->peers.nelts;
    for (visited = 0; visited < ngx_min(count, NGX_HEALTHCHECK_BATCH); visited++) {
        peer = &peers[runtime->cursor];
        runtime->cursor = (runtime->cursor + 1) % count;
        probe = peer->probe;
        if (probe != NULL && probe->pending) {
            rc = ngx_healthcheck_submit(probe, limit);
            if (rc == NGX_DECLINED) {
                probe->pending = 0;
            }
        }
        if (ngx_healthcheck_read_peer(peer->shm, &state, limit) != NGX_OK) {
            goto next;
        }
        if (state.phase == NGX_HEALTHCHECK_INITIAL && state.instance == 0
            && state.worker == runtime->worker && !peer->check_ev.timer_set)
        {
            ngx_healthcheck_timer(&peer->check_ev,
                (uint64_t) ngx_random() % ngx_max(peer->conf->check_interval, 1000));
        }
        if (probe != NULL
            && (state.worker != runtime->worker || state.instance != probe->instance
                || state.term != probe->term || state.round != probe->round))
        {
            ngx_healthcheck_probe_cleanup(probe, 0);
            probe->active = 0;
            probe->pending = 0;
            if (peer->check_ev.timer_set) {
                ngx_del_timer(&peer->check_ev);
            }
        }
        if (state.worker == runtime->worker && state.instance == runtime->instance) {
            if (probe != NULL && probe->active) {
                ngx_healthcheck_probe_resume(probe);
            }
            goto next;
        }
        eligible = ngx_healthcheck_add_time(ngx_healthcheck_deadline(peer, &state),
                                            runtime->grace);
        if (ngx_healthcheck_now(&now) != NGX_OK) {
            break;
        }
        if (now / 1000000 >= eligible
            && (now / 1000000 >= ngx_healthcheck_add_time(eligible, runtime->preference)
                || ngx_healthcheck_preferred(peer, &state, now / 1000000, limit)))
        {
            (void) ngx_healthcheck_claim(peer, 1, &state, limit);
        }
next:
        if (ngx_healthcheck_now(&now) != NGX_OK || now >= limit) {
            break;
        }
    }
rearm:
    runtime->budget = 0;
    if (!runtime->stopping) {
        ngx_healthcheck_timer(event, runtime->interval);
    }
}
