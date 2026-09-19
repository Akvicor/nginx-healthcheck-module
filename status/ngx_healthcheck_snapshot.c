#include "ngx_healthcheck_snapshot.h"

ngx_int_t
ngx_healthcheck_snapshot_init(ngx_pool_t *pool, ngx_upstream_check_peers_t *peers,
    ngx_healthcheck_snapshot_t *snapshot)
{
    ngx_memzero(snapshot, sizeof(*snapshot));
    if (ngx_array_init(&snapshot->peers, pool, 4,
                      sizeof(ngx_healthcheck_snapshot_peer_t)) != NGX_OK)
    {
        return NGX_ERROR;
    }
    if (peers == NULL) {
        return NGX_OK;
    }
    snapshot->generation = peers->generation;
    snapshot->total = peers->peers.nelts;
    return NGX_OK;
}

ngx_int_t
ngx_healthcheck_snapshot_step(ngx_upstream_check_peers_t *peers, ngx_uint_t flag,
    ngx_healthcheck_snapshot_t *snapshot, ngx_uint_t *remaining)
{
    ngx_upstream_check_peer_t       *peer;
    ngx_healthcheck_snapshot_peer_t *row;
    ngx_healthcheck_peer_state_t     state;
    ngx_uint_t                      i;

    if (peers == NULL) {
        return NGX_OK;
    }
    peer = peers->peers.elts;
    while (snapshot->next < peers->peers.nelts) {
        if (*remaining == 0) {
            return NGX_BUSY;
        }
        i = snapshot->next;
        if (peer[i].shm == NULL) {
            return NGX_ERROR;
        }
        if (ngx_healthcheck_try_read_peer(peer[i].shm, &state) != NGX_OK) {
            return NGX_AGAIN;
        }
        (*remaining)--;
        snapshot->next++;
        snapshot->up += !state.health.down;
        if ((flag & 1) ? !state.health.down : ((flag & 2) && state.health.down)) {
            continue;
        }
        row = ngx_array_push(&snapshot->peers);
        if (row == NULL) {
            return NGX_ERROR;
        }
        row->index = peer[i].index;
        row->upstream_name = peer[i].upstream_name;
        row->peer_addr = peer[i].peer_addr;
        row->conf = peer[i].conf;
        row->health = state.health;
    }
    return NGX_OK;
}
