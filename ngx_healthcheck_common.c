/*
 * Copyright (C) 2017- Changxun Zhou(changxunzhou@qq.com)
 * desc: nginx upstream server health check.
 * date: 2020-06-21 23:40
 */
#include "common.h.in"

typedef struct ngx_healthcheck_identity_node_s ngx_healthcheck_identity_node_t;

struct ngx_healthcheck_identity_node_s {
    ngx_upstream_check_peer_shm_t   *peer;
    ngx_healthcheck_identity_node_t *next;
    ngx_flag_t                       consumed;
};

struct ngx_healthcheck_identity_index_s {
    ngx_uint_t                       bucket_count;
    ngx_healthcheck_identity_node_t **buckets;
};


ngx_int_t
ngx_healthcheck_update_delay(ngx_healthcheck_delay_stats_t *stats,
    ngx_msec_t delay)
{
    uint64_t old_avg;

    if (stats == NULL) {
        return NGX_ERROR;
    }

    if (stats->delay_sample_count == 0) {
        stats->delay_total = (uint64_t) delay;
        stats->delay_sample_count = 1;
        stats->last = delay;
        stats->avg = delay;
        stats->min = delay;
        stats->max = delay;
        return NGX_OK;
    }

    if (stats->delay_total > UINT64_MAX - (uint64_t) delay
        || stats->delay_sample_count == UINT64_MAX)
    {
        old_avg = stats->delay_total / stats->delay_sample_count;
        stats->delay_sample_count = (stats->delay_sample_count >> 1) + 1;

        if (old_avg != 0
            && stats->delay_sample_count > UINT64_MAX / old_avg)
        {
            stats->delay_sample_count = UINT64_MAX / old_avg;
        }

        if (stats->delay_sample_count == 0) {
            return NGX_ERROR;
        }

        stats->delay_total = old_avg * stats->delay_sample_count;

        while (stats->delay_total > UINT64_MAX - (uint64_t) delay) {
            stats->delay_sample_count >>= 1;
            if (stats->delay_sample_count == 0) {
                return NGX_ERROR;
            }
            stats->delay_total = old_avg * stats->delay_sample_count;
        }
    }

    stats->delay_total += (uint64_t) delay;
    stats->delay_sample_count++;
    if (stats->delay_sample_count == 0) {
        return NGX_ERROR;
    }

    stats->last = delay;
    stats->avg = (ngx_msec_t) (stats->delay_total
                               / stats->delay_sample_count);

    if (delay < stats->min) {
        stats->min = delay;
    }
    if (delay > stats->max) {
        stats->max = delay;
    }

    return NGX_OK;
}


static ngx_uint_t
ngx_healthcheck_hash_bytes(ngx_uint_t hash, const void *data, size_t len)
{
    const u_char *p;

    p = data;
    while (len--) {
        hash ^= *p++;
        hash *= 16777619U;
    }

    return hash;
}


static ngx_uint_t
ngx_healthcheck_shm_identity_hash(ngx_upstream_check_peer_shm_t *peer)
{
    ngx_uint_t hash;

    hash = 2166136261U;
    hash = ngx_healthcheck_hash_bytes(hash, peer->upstream_name.data,
                                     peer->upstream_name.len);
    hash = ngx_healthcheck_hash_bytes(hash, peer->sockaddr, peer->socklen);
    hash = ngx_healthcheck_hash_bytes(hash, peer->check_sockaddr,
                                     peer->check_socklen);
    return ngx_healthcheck_hash_bytes(hash, &peer->check_type,
                                      sizeof(peer->check_type));
}


static ngx_uint_t
ngx_healthcheck_peer_identity_hash(ngx_upstream_check_peer_t *peer)
{
    ngx_uint_t hash, type;

    type = peer->conf->check_type_conf->type;
    hash = 2166136261U;
    hash = ngx_healthcheck_hash_bytes(hash, peer->upstream_name->data,
                                     peer->upstream_name->len);
    hash = ngx_healthcheck_hash_bytes(hash, peer->peer_addr->sockaddr,
                                     peer->peer_addr->socklen);
    hash = ngx_healthcheck_hash_bytes(hash, peer->check_peer_addr->sockaddr,
                                     peer->check_peer_addr->socklen);
    return ngx_healthcheck_hash_bytes(hash, &type, sizeof(type));
}


static ngx_int_t
ngx_healthcheck_identity_equal(ngx_upstream_check_peer_shm_t *old,
    ngx_upstream_check_peer_t *peer)
{
    if (old->check_type != peer->conf->check_type_conf->type
        || old->upstream_name.len != peer->upstream_name->len
        || old->socklen != peer->peer_addr->socklen
        || old->check_socklen != peer->check_peer_addr->socklen)
    {
        return 0;
    }

    return ngx_memcmp(old->upstream_name.data, peer->upstream_name->data,
                      old->upstream_name.len) == 0
           && ngx_memcmp(old->sockaddr, peer->peer_addr->sockaddr,
                         old->socklen) == 0
           && ngx_memcmp(old->check_sockaddr,
                         peer->check_peer_addr->sockaddr,
                         old->check_socklen) == 0;
}


ngx_healthcheck_identity_index_t *
ngx_healthcheck_identity_index_create(ngx_pool_t *pool,
    ngx_upstream_check_peers_shm_t *peers_shm)
{
    ngx_uint_t                         i, bucket;
    ngx_healthcheck_identity_index_t  *index;
    ngx_healthcheck_identity_node_t   *node;

    index = ngx_pcalloc(pool, sizeof(*index));
    if (index == NULL) {
        return NULL;
    }

    if (peers_shm != NULL
        && peers_shm->number > (((ngx_uint_t) -1) - 1) / 2)
    {
        return NULL;
    }

    index->bucket_count = peers_shm && peers_shm->number
                          ? peers_shm->number * 2 + 1 : 1;
    if (index->bucket_count > (size_t) -1 / sizeof(index->buckets[0])) {
        return NULL;
    }
    index->buckets = ngx_pcalloc(pool, index->bucket_count
                                      * sizeof(index->buckets[0]));
    if (index->buckets == NULL) {
        return NULL;
    }

    if (peers_shm == NULL
        || peers_shm->magic != NGX_HEALTHCHECK_SHM_MAGIC
        || peers_shm->version != NGX_HEALTHCHECK_SHM_VERSION)
    {
        return index;
    }

    for (i = 0; i < peers_shm->number; i++) {
        node = ngx_pcalloc(pool, sizeof(*node));
        if (node == NULL) {
            return NULL;
        }

        node->peer = &peers_shm->peers[i];
        bucket = ngx_healthcheck_shm_identity_hash(node->peer)
                 % index->bucket_count;
        node->next = index->buckets[bucket];
        index->buckets[bucket] = node;
    }

    return index;
}


ngx_upstream_check_peer_shm_t *
ngx_healthcheck_identity_index_take(ngx_healthcheck_identity_index_t *index,
    ngx_upstream_check_peer_t *peer)
{
    ngx_uint_t                        bucket;
    ngx_healthcheck_identity_node_t  *node;

    bucket = ngx_healthcheck_peer_identity_hash(peer) % index->bucket_count;
    for (node = index->buckets[bucket]; node; node = node->next) {
        if (!node->consumed
            && ngx_healthcheck_identity_equal(node->peer, peer))
        {
            node->consumed = 1;
            return node->peer;
        }
    }

    return NULL;
}


ngx_upstream_check_peers_shm_t *
ngx_healthcheck_find_latest_peers_shm(ngx_cycle_t *cycle, void *tag)
{
    ngx_uint_t                       i;
    ngx_list_part_t                 *part;
    ngx_shm_zone_t                  *zones;
    ngx_upstream_check_peers_shm_t  *candidate, *latest;

    latest = NULL;
    part = &cycle->shared_memory.part;
    zones = part->elts;

    for (i = 0; /* void */ ; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            zones = part->elts;
            i = 0;
        }

        if (zones[i].tag != tag || zones[i].data == NULL) {
            continue;
        }

        candidate = zones[i].data;
        if (candidate->magic != NGX_HEALTHCHECK_SHM_MAGIC
            || candidate->version != NGX_HEALTHCHECK_SHM_VERSION)
        {
            continue;
        }

        if (latest == NULL || candidate->generation > latest->generation) {
            latest = candidate;
        }
    }

    return latest;
}


ngx_int_t
ngx_healthcheck_copy_shm_identity(ngx_slab_pool_t *shpool,
    ngx_upstream_check_peer_shm_t *peer_shm,
    ngx_upstream_check_peer_t *peer)
{
    peer_shm->socklen = peer->peer_addr->socklen;
    peer_shm->sockaddr = ngx_slab_alloc(shpool, peer_shm->socklen);
    if (peer_shm->sockaddr == NULL) {
        return NGX_ERROR;
    }
    ngx_memcpy(peer_shm->sockaddr, peer->peer_addr->sockaddr,
               peer_shm->socklen);

    peer_shm->check_socklen = peer->check_peer_addr->socklen;
    peer_shm->check_sockaddr = ngx_slab_alloc(shpool,
                                              peer_shm->check_socklen);
    if (peer_shm->check_sockaddr == NULL) {
        return NGX_ERROR;
    }
    ngx_memcpy(peer_shm->check_sockaddr, peer->check_peer_addr->sockaddr,
               peer_shm->check_socklen);

    peer_shm->upstream_name.len = peer->upstream_name->len;
    if (peer->upstream_name->len != 0) {
        peer_shm->upstream_name.data = ngx_slab_alloc(shpool,
                                                   peer->upstream_name->len);
        if (peer_shm->upstream_name.data == NULL) {
            return NGX_ERROR;
        }
        ngx_memcpy(peer_shm->upstream_name.data, peer->upstream_name->data,
                   peer->upstream_name->len);
    }
    peer_shm->check_type = peer->conf->check_type_conf->type;

    return NGX_OK;
}
