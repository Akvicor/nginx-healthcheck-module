#ifndef NGX_HEALTHCHECK_STATE_H
#define NGX_HEALTHCHECK_STATE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <stdint.h>

/* 版本 2 使用独立锁和双槽发布，旧布局通过版本校验隔离。 */
#define NGX_HEALTHCHECK_SHM_MAGIC    0x48434b31U
#define NGX_HEALTHCHECK_SHM_VERSION  2
#define NGX_HEALTHCHECK_HTTP         1
#define NGX_HEALTHCHECK_STREAM       2
#define NGX_HEALTHCHECK_LOCK_NS      UINT64_C(1000000)
#define NGX_HEALTHCHECK_BATCH        256

/* 阶段只随实际进度变化，普通事件和心跳不能延长期限。 */
typedef enum {
    NGX_HEALTHCHECK_INITIAL,
    NGX_HEALTHCHECK_WAITING,
    NGX_HEALTHCHECK_PREPARING,
    NGX_HEALTHCHECK_INFLIGHT
} ngx_healthcheck_phase_t;

/* 可继承和展示的完整健康样本；时间使用宽位单调毫秒。 */
typedef struct {
    uint64_t       access_time;
    ngx_uint_t     rise_count;
    ngx_uint_t     fall_count;
    ngx_uint_t     down;
    ngx_msec_t     last_check_delay;
    ngx_msec_t     avg_check_delay;
    ngx_msec_t     min_check_delay;
    ngx_msec_t     max_check_delay;
    uint64_t       delay_total;
    uint64_t       delay_sample_count;
} ngx_healthcheck_health_t;

/* 健康、资格和进度共用一次发布，避免死亡恢复读到半份结果。 */
typedef struct {
    ngx_healthcheck_health_t  health;
    ngx_pid_t                 owner;
    ngx_uint_t                worker;
    uint64_t                  instance;
    uint64_t                  term;
    uint64_t                  round;
    uint64_t                  deadline;
    ngx_healthcheck_phase_t   phase;
} ngx_healthcheck_peer_state_t;

/* 发布字低位为槽号，第二位为供业务选路单次读取的 down 位。 */
typedef struct {
    ngx_shmtx_t                      mutex;
    ngx_shmtx_sh_t                   lock;
    ngx_atomic_t                     published;
    ngx_healthcheck_peer_state_t      slots[2];
    struct sockaddr                 *sockaddr;
    socklen_t                        socklen;
    ngx_str_t                        upstream_name;
    struct sockaddr                 *check_sockaddr;
    socklen_t                        check_socklen;
    ngx_uint_t                       check_type;
} ngx_upstream_check_peer_shm_t;

/* worker 编号固定，进程实例代次在每次登记时递增。 */
typedef struct {
    ngx_pid_t     pid;
    uint64_t      instance;
    uint64_t      heartbeat;
    ngx_uint_t    exiting;
} ngx_healthcheck_worker_state_t;

typedef struct {
    ngx_shmtx_t                       mutex;
    ngx_shmtx_sh_t                    lock;
    ngx_atomic_t                      published;
    ngx_healthcheck_worker_state_t     slots[2];
} ngx_healthcheck_worker_shm_t;

/* 激活时间一次发布；worker 登记表和 peer 表都属于同一共享区。 */
typedef struct {
    uint32_t                         magic;
    uint32_t                         version;
    ngx_uint_t                       generation;
    ngx_uint_t                       number;
    ngx_uint_t                       worker_count;
    ngx_shmtx_t                      activation_mutex;
    ngx_shmtx_sh_t                   activation_lock;
    ngx_atomic_t                     activated;
    uint64_t                         activated_at;
    ngx_healthcheck_worker_shm_t     *workers;
    ngx_upstream_check_peer_shm_t     peers[1];
} ngx_upstream_check_peers_shm_t;

/* 取即时单调纳秒；失败时调用方保留状态并跳过时间判定。 */
ngx_int_t ngx_healthcheck_now(uint64_t *now);

/* 期限运算饱和到未来，防止溢出变成已经过期。 */
uint64_t ngx_healthcheck_add_time(uint64_t a, uint64_t b);
uint64_t ngx_healthcheck_mul_time(uint64_t a, uint64_t b);

/* limit 是绝对纳秒期限；0 表示从当前时刻起采用 1ms 预算。 */
ngx_int_t ngx_healthcheck_lock(ngx_shmtx_t *mutex, uint64_t limit);
ngx_int_t ngx_healthcheck_mutex_init(ngx_shmtx_t *mutex,
    ngx_shmtx_sh_t *lock, ngx_pool_t *pool, ngx_str_t *name);

/* 锁内发布另一完整槽，业务只读取发布字中的健康位。 */
void ngx_healthcheck_publish(ngx_upstream_check_peer_shm_t *peer,
    const ngx_healthcheck_peer_state_t *state);
void ngx_healthcheck_publish_worker(ngx_healthcheck_worker_shm_t *worker,
    const ngx_healthcheck_worker_state_t *state);

/* 状态请求使用非阻塞读取；竞争返回 NGX_AGAIN，保留死亡锁恢复能力。 */
ngx_int_t ngx_healthcheck_try_read_peer(ngx_upstream_check_peer_shm_t *peer,
    ngx_healthcheck_peer_state_t *state);

/* 一次读取完整槽，快照由调用方持有，锁在返回前释放。 */
ngx_int_t ngx_healthcheck_read_peer(ngx_upstream_check_peer_shm_t *peer,
    ngx_healthcheck_peer_state_t *state, uint64_t limit);
ngx_int_t ngx_healthcheck_read_worker(ngx_healthcheck_worker_shm_t *worker,
    ngx_healthcheck_worker_state_t *state, uint64_t limit);

#endif
