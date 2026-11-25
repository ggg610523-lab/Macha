/*
 * libplsr-dispatch.so: clean-room libdispatch slice.
 *
 * Serial queues, sync/async execution, dispatch_once, semaphores, groups and
 * dispatch_apply. Handles are our own objects; Darwin block arguments are
 * invoked through the Block_layout invoke pointer at offset 16.
 *
 * Not implemented: concurrent queues (every queue serialises), QoS classes,
 * sources, timers, data objects, and workqueue priority.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DISPATCH_TIME_FOREVER (~(uint64_t)0)
#define DISPATCH_TIME_NOW 0ull

typedef struct plsr_queue {
    pthread_mutex_t lock;
    char *label;
} plsr_queue;

typedef struct plsr_semaphore {
    sem_t sem;
} plsr_semaphore;

typedef struct plsr_group {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    long count;
} plsr_group;

struct plsr_work {
    plsr_queue *queue;
    void (*fn)(void *);
    void *ctx;
    void *block;
    int owns_block;
};

static plsr_queue *g_main_queue;
static plsr_queue *g_global_queue;
static pthread_once_t g_default_once = PTHREAD_ONCE_INIT;

static char *dup_label(const char *label)
{
    return strdup(label ? label : "com.plsr.queue");
}

static plsr_queue *queue_new(const char *label)
{
    plsr_queue *q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q);
        return NULL;
    }
    q->label = dup_label(label);
    return q;
}

static void default_queues_init(void)
{
    g_main_queue = queue_new("com.apple.main-thread");
    g_global_queue = queue_new("com.apple.root.default-qos");
}

/* Block_layout: isa, flags, reserved, invoke at offset 16. */
static void run_block(void *block)
{
    void (*invoke)(void *);
    if (!block)
        return;
    memcpy(&invoke, (char *)block + 16, sizeof(invoke));
    if (invoke)
        invoke(block);
}

static void run_block_index(void *block, size_t index)
{
    void (*invoke)(void *, size_t);
    if (!block)
        return;
    memcpy(&invoke, (char *)block + 16, sizeof(invoke));
    if (invoke)
        invoke(block, index);
}

#define BLOCK_HAS_COPY_DISPOSE (1u << 25)
#define BLOCK_IS_GLOBAL (1u << 28)

struct plsr_block_layout {
    void *isa;
    uint32_t flags;
    uint32_t reserved;
    void (*invoke)(void *);
    void *descriptor;
};

struct plsr_block_descriptor {
    unsigned long reserved;
    unsigned long size;
    void (*copy)(void *dst, const void *src);
    void (*dispose)(const void *src);
};

/* dispatch_async must own a block that outlives the caller's stack frame. */
static void *block_copy_for_queue(void *block)
{
    const struct plsr_block_layout *src = block;
    struct plsr_block_layout *dst;
    size_t size;
    if (!src)
        return NULL;
    if (src->flags & BLOCK_IS_GLOBAL)
        return block;
    size = sizeof(*src);
    if (src->descriptor) {
        const struct plsr_block_descriptor *d = src->descriptor;
        if (d->size > size && d->size < (1u << 20))
            size = d->size;
    }
    dst = calloc(1, size);
    if (!dst)
        return block;
    memcpy(dst, src, size);
    if ((src->flags & BLOCK_HAS_COPY_DISPOSE) && src->descriptor) {
        const struct plsr_block_descriptor *d = src->descriptor;
        if (d->copy)
            d->copy(dst, src);
    }
    if (getenv("PLSR_DISPATCH_TRACE"))
        dprintf(2, "dispatch: copied block %p -> %p size=%zu invoke=%p\n",
                block, (void *)dst, size, (void *)dst->invoke);
    return dst;
}

static void block_release_copy(void *block)
{
    struct plsr_block_layout *b = block;
    if (!b)
        return;
    if ((b->flags & BLOCK_HAS_COPY_DISPOSE) && b->descriptor) {
        const struct plsr_block_descriptor *d = b->descriptor;
        if (d->dispose)
            d->dispose(block);
    }
    free(block);
}

/* ---- queues ---- */

void *plsr_dispatch_queue_create(const char *label, void *attr)
    __asm__("_dispatch_queue_create");
void *plsr_dispatch_queue_create(const char *label, void *attr)
{
    (void)attr;   /* serial vs concurrent is not modelled yet */
    return queue_new(label);
}

void *plsr_dispatch_get_main_queue(void) __asm__("_dispatch_get_main_queue");
void *plsr_dispatch_get_main_queue(void)
{
    pthread_once(&g_default_once, default_queues_init);
    return g_main_queue;
}

void *plsr_dispatch_get_global_queue(long id, unsigned long flags)
    __asm__("_dispatch_get_global_queue");
void *plsr_dispatch_get_global_queue(long id, unsigned long flags)
{
    (void)id;
    (void)flags;
    pthread_once(&g_default_once, default_queues_init);
    return g_global_queue;
}

const char *plsr_dispatch_queue_get_label(const void *q) __asm__("_dispatch_queue_get_label");
const char *plsr_dispatch_queue_get_label(const void *q)
{
    const plsr_queue *queue = q;
    return queue && queue->label ? queue->label : "";
}

void plsr_dispatch_release(void *obj) __asm__("_dispatch_release");
void plsr_dispatch_release(void *obj) { (void)obj; }
void *plsr_dispatch_retain(void *obj) __asm__("_dispatch_retain");
void *plsr_dispatch_retain(void *obj) { return obj; }

/* ---- sync / async ---- */

void plsr_dispatch_sync_f(void *q, void *ctx, void (*fn)(void *))
    __asm__("_dispatch_sync_f");
void plsr_dispatch_sync_f(void *q, void *ctx, void (*fn)(void *))
{
    plsr_queue *queue = q ? q : plsr_dispatch_get_global_queue(0, 0);
    if (!queue || !fn)
        return;
    pthread_mutex_lock(&queue->lock);
    fn(ctx);
    pthread_mutex_unlock(&queue->lock);
}

void plsr_dispatch_sync(void *q, void *block) __asm__("_dispatch_sync");
void plsr_dispatch_sync(void *q, void *block)
{
    plsr_queue *queue = q ? q : plsr_dispatch_get_global_queue(0, 0);
    if (!queue)
        return;
    pthread_mutex_lock(&queue->lock);
    run_block(block);
    pthread_mutex_unlock(&queue->lock);
}

static void *async_trampoline(void *p)
{
    struct plsr_work *w = p;
    pthread_mutex_lock(&w->queue->lock);
    if (w->fn)
        w->fn(w->ctx);
    else {
        run_block(w->block);
        if (w->owns_block)
            block_release_copy(w->block);
    }
    pthread_mutex_unlock(&w->queue->lock);
    free(w);
    return NULL;
}

static void enqueue(plsr_queue *queue, void (*fn)(void *), void *ctx, void *block)
{
    struct plsr_work *w;
    pthread_t tid;
    int r;
    if (!queue || (!fn && !block))
        return;
    w = calloc(1, sizeof(*w));
    if (!w) {
        if (fn)
            fn(ctx);
        else
            run_block(block);
        return;
    }
    w->queue = queue;
    w->fn = fn;
    w->ctx = ctx;
    w->block = block_copy_for_queue(block);
    w->owns_block = block && w->block && w->block != block;
    r = pthread_create(&tid, NULL, async_trampoline, w);
    if (r != 0) {
        if (fn)
            fn(ctx);
        else {
            run_block(w->block);
            if (w->owns_block)
                block_release_copy(w->block);
        }
        free(w);
        return;
    }
    pthread_detach(tid);
}

void plsr_dispatch_async_f(void *q, void *ctx, void (*fn)(void *))
    __asm__("_dispatch_async_f");
void plsr_dispatch_async_f(void *q, void *ctx, void (*fn)(void *))
{
    enqueue(q ? q : plsr_dispatch_get_global_queue(0, 0), fn, ctx, NULL);
}

void plsr_dispatch_async(void *q, void *block) __asm__("_dispatch_async");
void plsr_dispatch_async(void *q, void *block)
{
    enqueue(q ? q : plsr_dispatch_get_global_queue(0, 0), NULL, NULL, block);
}

void plsr_dispatch_barrier_sync(void *q, void *block) __asm__("_dispatch_barrier_sync");
void plsr_dispatch_barrier_sync(void *q, void *block) { plsr_dispatch_sync(q, block); }
void plsr_dispatch_barrier_async(void *q, void *block) __asm__("_dispatch_barrier_async");
void plsr_dispatch_barrier_async(void *q, void *block) { plsr_dispatch_async(q, block); }

/* ---- once ---- */

/* State: 0 = untouched, 1 = running, 2 = complete. A global mutex here
 * deadlocks when an initializer recursively enters dispatch_once for a
 * different predicate, which is common in Swift/Foundation startup. */
void plsr_dispatch_once(long *predicate, void *block) __asm__("_dispatch_once");
void plsr_dispatch_once(long *predicate, void *block)
{
    long state;
    if (!predicate || !block)
        return;
    state = __atomic_load_n(predicate, __ATOMIC_ACQUIRE);
    if (state == 2)
        return;
    if (state == 0 &&
        __atomic_compare_exchange_n(predicate, &state, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        run_block(block);
        __atomic_store_n(predicate, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(predicate, __ATOMIC_ACQUIRE) == 1)
        sched_yield();
}

void plsr_dispatch_once_f(long *predicate, void *ctx, void (*fn)(void *))
    __asm__("_dispatch_once_f");
void plsr_dispatch_once_f(long *predicate, void *ctx, void (*fn)(void *))
{
    long state;
    if (!predicate || !fn)
        return;
    state = __atomic_load_n(predicate, __ATOMIC_ACQUIRE);
    if (state == 2)
        return;
    if (state == 0 &&
        __atomic_compare_exchange_n(predicate, &state, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        fn(ctx);
        __atomic_store_n(predicate, 2, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(predicate, __ATOMIC_ACQUIRE) == 1)
        sched_yield();
}

/* ---- semaphores ---- */

void *plsr_dispatch_semaphore_create(long value)
    __asm__("_dispatch_semaphore_create");
void *plsr_dispatch_semaphore_create(long value)
{
    plsr_semaphore *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    if (sem_init(&s->sem, 0, (unsigned)value) != 0) {
        free(s);
        return NULL;
    }
    return s;
}

long plsr_dispatch_semaphore_signal(void *sem) __asm__("_dispatch_semaphore_signal");
long plsr_dispatch_semaphore_signal(void *sem)
{
    plsr_semaphore *s = sem;
    return s ? (sem_post(&s->sem) == 0 ? 0 : errno) : EINVAL;
}

long plsr_dispatch_semaphore_wait(void *sem, uint64_t timeout)
    __asm__("_dispatch_semaphore_wait");
long plsr_dispatch_semaphore_wait(void *sem, uint64_t timeout)
{
    plsr_semaphore *s = sem;
    if (!s)
        return EINVAL;
    if (timeout == DISPATCH_TIME_FOREVER)
        return sem_wait(&s->sem) == 0 ? 0 : errno;
    if (timeout == DISPATCH_TIME_NOW)
        return sem_trywait(&s->sem) == 0 ? 0 : errno;
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
            return errno;
        ts.tv_sec += (time_t)(timeout / 1000000000ull);
        ts.tv_nsec += (long)(timeout % 1000000000ull);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        return sem_timedwait(&s->sem, &ts) == 0 ? 0 : errno;
    }
}

/* ---- groups ---- */

void *plsr_dispatch_group_create(void) __asm__("_dispatch_group_create");
void *plsr_dispatch_group_create(void)
{
    plsr_group *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    pthread_mutex_init(&g->lock, NULL);
    pthread_cond_init(&g->cond, NULL);
    return g;
}

void plsr_dispatch_group_enter(void *group) __asm__("_dispatch_group_enter");
void plsr_dispatch_group_enter(void *group)
{
    plsr_group *g = group;
    if (!g)
        return;
    pthread_mutex_lock(&g->lock);
    g->count++;
    pthread_mutex_unlock(&g->lock);
}

void plsr_dispatch_group_leave(void *group) __asm__("_dispatch_group_leave");
void plsr_dispatch_group_leave(void *group)
{
    plsr_group *g = group;
    if (!g)
        return;
    pthread_mutex_lock(&g->lock);
    if (g->count > 0)
        g->count--;
    if (g->count == 0)
        pthread_cond_broadcast(&g->cond);
    pthread_mutex_unlock(&g->lock);
}

long plsr_dispatch_group_wait(void *group, uint64_t timeout)
    __asm__("_dispatch_group_wait");
long plsr_dispatch_group_wait(void *group, uint64_t timeout)
{
    plsr_group *g = group;
    int r = 0;
    if (!g)
        return EINVAL;
    pthread_mutex_lock(&g->lock);
    while (g->count > 0) {
        if (timeout == DISPATCH_TIME_FOREVER) {
            pthread_cond_wait(&g->cond, &g->lock);
        } else if (timeout == DISPATCH_TIME_NOW) {
            r = ETIMEDOUT;
            break;
        } else {
            struct timespec ts;
            if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
                r = errno;
                break;
            }
            ts.tv_sec += (time_t)(timeout / 1000000000ull);
            ts.tv_nsec += (long)(timeout % 1000000000ull);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            r = pthread_cond_timedwait(&g->cond, &g->lock, &ts);
            if (r == ETIMEDOUT)
                break;
        }
    }
    pthread_mutex_unlock(&g->lock);
    return r;
}

struct group_notify {
    plsr_group *group;
    plsr_queue *queue;
    void *block;
    int owns_block;
};

static void *group_notify_thread(void *p)
{
    struct group_notify *n = p;
    plsr_dispatch_group_wait(n->group, DISPATCH_TIME_FOREVER);
    plsr_dispatch_sync(n->queue, n->block);
    if (n->owns_block)
        block_release_copy(n->block);
    free(n);
    return NULL;
}

void plsr_dispatch_group_notify(void *group, void *q, void *block)
    __asm__("_dispatch_group_notify");
void plsr_dispatch_group_notify(void *group, void *q, void *block)
{
    struct group_notify *n;
    pthread_t tid;
    if (!group || !block)
        return;
    n = calloc(1, sizeof(*n));
    if (!n)
        return;
    n->group = group;
    n->queue = q ? q : plsr_dispatch_get_global_queue(0, 0);
    n->block = block_copy_for_queue(block);
    n->owns_block = block && n->block && n->block != block;
    if (pthread_create(&tid, NULL, group_notify_thread, n) != 0) {
        if (n->owns_block)
            block_release_copy(n->block);
        free(n);
        return;
    }
    pthread_detach(tid);
}

/* ---- apply ---- */

void plsr_dispatch_apply(size_t iterations, void *q, void *block)
    __asm__("_dispatch_apply");
void plsr_dispatch_apply(size_t iterations, void *q, void *block)
{
    plsr_queue *queue = q ? q : plsr_dispatch_get_global_queue(0, 0);
    if (!queue || !block)
        return;
    for (size_t i = 0; i < iterations; i++) {
        pthread_mutex_lock(&queue->lock);
        run_block_index(block, i);
        pthread_mutex_unlock(&queue->lock);
    }
}

/* ---- lifecycle, attributes, specifics ---- */

void plsr_dispatch_activate(void *obj) __asm__("_dispatch_activate");
void plsr_dispatch_activate(void *obj) { (void)obj; }
void plsr_dispatch_resume(void *obj) __asm__("_dispatch_resume");
void plsr_dispatch_resume(void *obj) { (void)obj; }
void plsr_dispatch_suspend(void *obj) __asm__("_dispatch_suspend");
void plsr_dispatch_suspend(void *obj) { (void)obj; }
void plsr_dispatch_set_context(void *obj, void *ctx) __asm__("_dispatch_set_context");
void plsr_dispatch_set_context(void *obj, void *ctx) { (void)obj; (void)ctx; }

void *plsr_dispatch_queue_create_with_target(const char *label, void *attr,
                                             void *target)
    __asm__("_dispatch_queue_create_with_target$V2");
void *plsr_dispatch_queue_create_with_target(const char *label, void *attr,
                                             void *target)
{
    (void)attr;
    (void)target;
    return queue_new(label);
}

void *plsr_qattr_initially_inactive(void *attr)
    __asm__("_dispatch_queue_attr_make_initially_inactive");
void *plsr_qattr_initially_inactive(void *attr) { return attr; }
void *plsr_qattr_autorelease(void *attr, unsigned long freq)
    __asm__("_dispatch_queue_attr_make_with_autorelease_frequency");
void *plsr_qattr_autorelease(void *attr, unsigned long freq)
{
    (void)freq;
    return attr;
}
void *plsr_qattr_qos(void *attr, unsigned int qos, int relative)
    __asm__("_dispatch_queue_attr_make_with_qos_class");
void *plsr_qattr_qos(void *attr, unsigned int qos, int relative)
{
    (void)qos;
    (void)relative;
    return attr;
}
unsigned int plsr_dispatch_queue_get_qos_class(void *q, int *relative)
    __asm__("_dispatch_queue_get_qos_class");
unsigned int plsr_dispatch_queue_get_qos_class(void *q, int *relative)
{
    (void)q;
    if (relative)
        *relative = 0;
    return 0;
}

struct specific_entry {
    plsr_queue *queue;
    const void *key;
    void *value;
    void (*destructor)(void *);
    struct specific_entry *next;
};
static struct specific_entry *g_specific;
static pthread_mutex_t g_specific_lock = PTHREAD_MUTEX_INITIALIZER;

void plsr_dispatch_queue_set_specific(void *q, const void *key, void *value,
                                      void (*destructor)(void *))
    __asm__("_dispatch_queue_set_specific");
void plsr_dispatch_queue_set_specific(void *q, const void *key, void *value,
                                      void (*destructor)(void *))
{
    struct specific_entry *e;
    pthread_mutex_lock(&g_specific_lock);
    for (e = g_specific; e; e = e->next)
        if (e->queue == q && e->key == key)
            break;
    if (!e) {
        e = calloc(1, sizeof(*e));
        if (e) {
            e->queue = q;
            e->key = key;
            e->next = g_specific;
            g_specific = e;
        }
    }
    if (e) {
        e->value = value;
        e->destructor = destructor;
    }
    pthread_mutex_unlock(&g_specific_lock);
}

void *plsr_dispatch_queue_get_specific(void *q, const void *key)
    __asm__("_dispatch_queue_get_specific");
void *plsr_dispatch_queue_get_specific(void *q, const void *key)
{
    struct specific_entry *e;
    void *value = NULL;
    pthread_mutex_lock(&g_specific_lock);
    for (e = g_specific; e; e = e->next)
        if (e->queue == q && e->key == key) {
            value = e->value;
            break;
        }
    pthread_mutex_unlock(&g_specific_lock);
    return value;
}

void *plsr_dispatch_get_specific(const void *key) __asm__("_dispatch_get_specific");
void *plsr_dispatch_get_specific(const void *key)
{
    return plsr_dispatch_queue_get_specific(plsr_dispatch_get_main_queue(), key);
}

/* ---- assertions ---- */

void plsr_dispatch_assert_queue(void *q) __asm__("_dispatch_assert_queue$V2");
void plsr_dispatch_assert_queue(void *q) { (void)q; }
void plsr_dispatch_assert_queue_barrier(void *q)
    __asm__("_dispatch_assert_queue_barrier");
void plsr_dispatch_assert_queue_barrier(void *q) { (void)q; }
void plsr_dispatch_assert_queue_not(void *q)
    __asm__("_dispatch_assert_queue_not$V2");
void plsr_dispatch_assert_queue_not(void *q) { (void)q; }

/* ---- timed work ---- */

struct after_work {
    uint64_t deadline;
    void *queue;
    void *block;
    void (*fn)(void *);
    void *ctx;
};

static void *after_thread(void *p)
{
    struct after_work *w = p;
    struct timespec nowts;
    uint64_t now;
    clock_gettime(CLOCK_MONOTONIC, &nowts);
    now = (uint64_t)nowts.tv_sec * 1000000000ull + (uint64_t)nowts.tv_nsec;
    if (w->deadline != DISPATCH_TIME_FOREVER && w->deadline > now) {
        uint64_t ns = w->deadline - now;
        struct timespec ts = { (time_t)(ns / 1000000000ull),
                               (long)(ns % 1000000000ull) };
        nanosleep(&ts, NULL);
    }
    if (w->fn)
        plsr_dispatch_sync_f(w->queue, w->ctx, w->fn);
    else
        plsr_dispatch_sync(w->queue, w->block);
    free(w);
    return NULL;
}

uint64_t plsr_dispatch_time(uint64_t when, int64_t delta)
    __asm__("_dispatch_time");
uint64_t plsr_dispatch_time(uint64_t when, int64_t delta)
{
    if (when == DISPATCH_TIME_FOREVER)
        return when;
    return when + (uint64_t)delta;
}

uint64_t plsr_dispatch_walltime(const struct timespec *ts, int64_t delta)
    __asm__("_dispatch_walltime");
uint64_t plsr_dispatch_walltime(const struct timespec *ts, int64_t delta)
{
    uint64_t base;
    if (ts) {
        base = (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        base = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    }
    return base + (uint64_t)delta;
}

void plsr_dispatch_after(uint64_t when, void *q, void *block)
    __asm__("_dispatch_after");
void plsr_dispatch_after(uint64_t when, void *q, void *block)
{
    struct after_work *w = calloc(1, sizeof(*w));
    pthread_t tid;
    if (!w || !block)
        return free(w);
    w->deadline = when;
    w->queue = q ? q : plsr_dispatch_get_global_queue(0, 0);
    w->block = block;
    if (pthread_create(&tid, NULL, after_thread, w) != 0) {
        free(w);
        return;
    }
    pthread_detach(tid);
}

void plsr_dispatch_after_f(uint64_t when, void *q, void *ctx, void (*fn)(void *))
    __asm__("_dispatch_after_f");
void plsr_dispatch_after_f(uint64_t when, void *q, void *ctx, void (*fn)(void *))
{
    struct after_work *w = calloc(1, sizeof(*w));
    pthread_t tid;
    if (!w || !fn)
        return free(w);
    w->deadline = when;
    w->queue = q ? q : plsr_dispatch_get_global_queue(0, 0);
    w->fn = fn;
    w->ctx = ctx;
    if (pthread_create(&tid, NULL, after_thread, w) != 0) {
        free(w);
        return;
    }
    pthread_detach(tid);
}

void plsr_dispatch_apply_f(size_t iterations, void *q, void *ctx,
                           void (*fn)(void *, size_t))
    __asm__("_dispatch_apply_f");
void plsr_dispatch_apply_f(size_t iterations, void *q, void *ctx,
                           void (*fn)(void *, size_t))
{
    plsr_queue *queue = q ? q : plsr_dispatch_get_global_queue(0, 0);
    if (!queue || !fn)
        return;
    for (size_t i = 0; i < iterations; i++) {
        pthread_mutex_lock(&queue->lock);
        fn(ctx, i);
        pthread_mutex_unlock(&queue->lock);
    }
}

/* ---- block objects ---- */

typedef struct plsr_block_ctrl {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int cancelled;
    int done;
    void *block;
} plsr_block_ctrl;

void *plsr_dispatch_block_create(unsigned long flags, void *block)
    __asm__("_dispatch_block_create");
void *plsr_dispatch_block_create(unsigned long flags, void *block)
{
    plsr_block_ctrl *c;
    (void)flags;
    if (!block)
        return NULL;
    c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cond, NULL);
    c->block = block;
    return c;
}

void *plsr_dispatch_block_create_with_qos(unsigned long flags, unsigned int qos,
                                          void *block)
    __asm__("_dispatch_block_create_with_qos_class");
void *plsr_dispatch_block_create_with_qos(unsigned long flags, unsigned int qos,
                                          void *block)
{
    (void)qos;
    return plsr_dispatch_block_create(flags, block);
}

void plsr_dispatch_block_cancel(void *ctrl) __asm__("_dispatch_block_cancel");
void plsr_dispatch_block_cancel(void *ctrl)
{
    plsr_block_ctrl *c = ctrl;
    if (!c)
        return;
    pthread_mutex_lock(&c->lock);
    c->cancelled = 1;
    pthread_cond_broadcast(&c->cond);
    pthread_mutex_unlock(&c->lock);
}

int plsr_dispatch_block_testcancel(void *ctrl) __asm__("_dispatch_block_testcancel");
int plsr_dispatch_block_testcancel(void *ctrl)
{
    plsr_block_ctrl *c = ctrl;
    return c ? c->cancelled : 0;
}

long plsr_dispatch_block_wait(void *ctrl, uint64_t timeout)
    __asm__("_dispatch_block_wait");
long plsr_dispatch_block_wait(void *ctrl, uint64_t timeout)
{
    plsr_block_ctrl *c = ctrl;
    int r = 0;
    if (!c)
        return EINVAL;
    pthread_mutex_lock(&c->lock);
    while (!c->done && !c->cancelled) {
        if (timeout == DISPATCH_TIME_FOREVER) {
            pthread_cond_wait(&c->cond, &c->lock);
        } else if (timeout == DISPATCH_TIME_NOW) {
            r = ETIMEDOUT;
            break;
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(timeout / 1000000000ull);
            ts.tv_nsec += (long)(timeout % 1000000000ull);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            r = pthread_cond_timedwait(&c->cond, &c->lock, &ts);
            if (r == ETIMEDOUT)
                break;
        }
    }
    pthread_mutex_unlock(&c->lock);
    return r;
}

struct block_notify {
    plsr_block_ctrl *ctrl;
    void *queue;
    void *block;
};

static void *block_notify_thread(void *p)
{
    struct block_notify *n = p;
    plsr_dispatch_block_wait(n->ctrl, DISPATCH_TIME_FOREVER);
    if (!n->ctrl->cancelled)
        plsr_dispatch_sync(n->queue, n->block);
    free(n);
    return NULL;
}

void plsr_dispatch_block_notify(void *ctrl, void *q, void *block)
    __asm__("_dispatch_block_notify");
void plsr_dispatch_block_notify(void *ctrl, void *q, void *block)
{
    struct block_notify *n;
    pthread_t tid;
    if (!ctrl || !block)
        return;
    n = calloc(1, sizeof(*n));
    if (!n)
        return;
    n->ctrl = ctrl;
    n->queue = q ? q : plsr_dispatch_get_global_queue(0, 0);
    n->block = block;
    if (pthread_create(&tid, NULL, block_notify_thread, n) != 0) {
        free(n);
        return;
    }
    pthread_detach(tid);
}

/* ---- data objects ---- */

struct plsr_data {
    const unsigned char *bytes;
    size_t size;
    void (*destructor)(const void *, size_t);
};

static struct plsr_data g_empty_data;
void *plsr_dispatch_data_empty __asm__("__dispatch_data_empty") = &g_empty_data;

void plsr_data_destructor_free(const void *buf, size_t size)
    __asm__("__dispatch_data_destructor_free");
void plsr_data_destructor_free(const void *buf, size_t size)
{
    (void)size;
    free((void *)buf);
}
void plsr_data_destructor_munmap(const void *buf, size_t size)
    __asm__("__dispatch_data_destructor_munmap");
void plsr_data_destructor_munmap(const void *buf, size_t size)
{
    (void)size;
    munmap((void *)buf, size);
}

static struct plsr_data *data_new(const void *buf, size_t size)
{
    struct plsr_data *d = calloc(1, sizeof(*d));
    unsigned char *copy = NULL;
    if (!d)
        return NULL;
    if (buf && size) {
        copy = malloc(size);
        if (!copy) {
            free(d);
            return NULL;
        }
        memcpy(copy, buf, size);
    }
    d->bytes = copy;
    d->size = size;
    d->destructor = copy ? plsr_data_destructor_free : NULL;
    return d;
}

void *plsr_dispatch_data_create(const void *buf, size_t size, void *q,
                                void (*destructor)(const void *, size_t))
    __asm__("_dispatch_data_create");
void *plsr_dispatch_data_create(const void *buf, size_t size, void *q,
                                void (*destructor)(const void *, size_t))
{
    (void)q;
    (void)destructor;
    return data_new(buf, size);
}

void *plsr_dispatch_data_create_map(void *data, const void **buf, size_t *size)
    __asm__("_dispatch_data_create_map");
void *plsr_dispatch_data_create_map(void *data, const void **buf, size_t *size)
{
    struct plsr_data *d = data;
    if (buf)
        *buf = d ? d->bytes : NULL;
    if (size)
        *size = d ? d->size : 0;
    return d;
}

size_t plsr_dispatch_data_get_size(void *data) __asm__("_dispatch_data_get_size");
void *plsr_dispatch_data_copy_region(void *data, size_t offset, size_t *size)
    __asm__("_dispatch_data_copy_region");
void *plsr_dispatch_data_create_subrange(void *data, size_t offset, size_t size)
    __asm__("_dispatch_data_create_subrange");
void *plsr_dispatch_data_create_concat(void *a, void *b)
    __asm__("_dispatch_data_create_concat");

size_t plsr_dispatch_data_get_size(void *data)
{
    struct plsr_data *d = data;
    return d ? d->size : 0;
}

void *plsr_dispatch_data_create_subrange(void *data, size_t offset, size_t size)
{
    struct plsr_data *d = data;
    if (!d || offset >= d->size)
        return data_new(NULL, 0);
    if (offset + size > d->size)
        size = d->size - offset;
    return data_new(d->bytes + offset, size);
}

void *plsr_dispatch_data_copy_region(void *data, size_t offset, size_t *size)
{
    struct plsr_data *d = data;
    size_t n = d && offset < d->size ? d->size - offset : 0;
    if (size)
        *size = n;
    if (!n)
        return NULL;
    return data_new(d->bytes + offset, n);
}

void *plsr_dispatch_data_create_concat(void *a, void *b)
{
    struct plsr_data *x = a, *y = b;
    size_t xn = x ? x->size : 0, yn = y ? y->size : 0;
    struct plsr_data *d = calloc(1, sizeof(*d));
    unsigned char *buf;
    if (!d)
        return NULL;
    buf = malloc(xn + yn ? xn + yn : 1);
    if (!buf) {
        free(d);
        return NULL;
    }
    if (xn)
        memcpy(buf, x->bytes, xn);
    if (yn)
        memcpy(buf + xn, y->bytes, yn);
    d->bytes = buf;
    d->size = xn + yn;
    d->destructor = plsr_data_destructor_free;
    return d;
}

void plsr_dispatch_data_apply(void *data, void *block)
    __asm__("_dispatch_data_apply");
void plsr_dispatch_data_apply(void *data, void *block)
{
    struct plsr_data *d = data;
    void (*invoke)(void *, void *, size_t, size_t);
    if (!d || !block)
        return;
    memcpy(&invoke, (char *)block + 16, sizeof(invoke));
    if (invoke)
        invoke(block, d, 0, d->size);
}

/* ---- read/write convenience ---- */

void plsr_dispatch_read(int fd, size_t length, void *q, void *handler)
    __asm__("_dispatch_read");
void plsr_dispatch_read(int fd, size_t length, void *q, void *handler)
{
    unsigned char *buf = malloc(length ? length : 1);
    ssize_t n = 0;
    int err = 0;
    void (*invoke)(void *, void *, int);
    (void)q;
    if (!buf)
        return;
    while ((size_t)n < length) {
        ssize_t r = read(fd, buf + n, length - (size_t)n);
        if (r == 0)
            break;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            err = errno;
            break;
        }
        n += r;
    }
    if (handler) {
        void *data = data_new(buf, (size_t)n);
        memcpy(&invoke, (char *)handler + 16, sizeof(invoke));
        if (invoke)
            invoke(handler, data, err);
    }
    free(buf);
}

void plsr_dispatch_write(int fd, void *data, void *q, void *handler)
    __asm__("_dispatch_write");
void plsr_dispatch_write(int fd, void *data, void *q, void *handler)
{
    struct plsr_data *d = data;
    ssize_t off = 0;
    int err = 0;
    void (*invoke)(void *, void *, int);
    (void)q;
    if (!d)
        return;
    while ((size_t)off < d->size) {
        ssize_t w = write(fd, d->bytes + off, d->size - (size_t)off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            err = errno;
            break;
        }
        off += w;
    }
    if (handler) {
        memcpy(&invoke, (char *)handler + 16, sizeof(invoke));
        if (invoke)
            invoke(handler, data, err);
    }
}

/* ---- sources and I/O (handlers stored, no kernel events yet) ---- */

typedef struct plsr_source {
    uintptr_t handle;
    unsigned long mask;
    uintptr_t data;
    void *event_handler;
    void (*event_handler_f)(void *);
    void *cancel_handler;
    int cancelled;
} plsr_source;

static int g_source_type_dummy;
void *plsr_st_data_add __asm__("__dispatch_source_type_data_add") = &g_source_type_dummy;
void *plsr_st_data_or __asm__("__dispatch_source_type_data_or") = &g_source_type_dummy;
void *plsr_st_data_replace __asm__("__dispatch_source_type_data_replace") = &g_source_type_dummy;
void *plsr_st_mach_recv __asm__("__dispatch_source_type_mach_recv") = &g_source_type_dummy;
void *plsr_st_mach_send __asm__("__dispatch_source_type_mach_send") = &g_source_type_dummy;
void *plsr_st_memorypressure __asm__("__dispatch_source_type_memorypressure") = &g_source_type_dummy;
void *plsr_st_proc __asm__("__dispatch_source_type_proc") = &g_source_type_dummy;
void *plsr_st_read __asm__("__dispatch_source_type_read") = &g_source_type_dummy;
void *plsr_st_signal __asm__("__dispatch_source_type_signal") = &g_source_type_dummy;
void *plsr_st_timer __asm__("__dispatch_source_type_timer") = &g_source_type_dummy;
void *plsr_st_vnode __asm__("__dispatch_source_type_vnode") = &g_source_type_dummy;
void *plsr_st_write __asm__("__dispatch_source_type_write") = &g_source_type_dummy;

void *plsr_dispatch_source_create(void *type, uintptr_t handle,
                                  unsigned long mask, void *q)
    __asm__("_dispatch_source_create");
void *plsr_dispatch_source_create(void *type, uintptr_t handle,
                                  unsigned long mask, void *q)
{
    plsr_source *s;
    (void)type;
    (void)q;
    s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->handle = handle;
    s->mask = mask;
    return s;
}

void plsr_dispatch_source_cancel(void *src) __asm__("_dispatch_source_cancel");
void plsr_dispatch_source_cancel(void *src)
{
    plsr_source *s = src;
    if (s)
        s->cancelled = 1;
}
int plsr_dispatch_source_testcancel(void *src)
    __asm__("_dispatch_source_testcancel");
int plsr_dispatch_source_testcancel(void *src)
{
    plsr_source *s = src;
    return s ? s->cancelled : 0;
}
uintptr_t plsr_dispatch_source_get_data(void *src)
    __asm__("_dispatch_source_get_data");
uintptr_t plsr_dispatch_source_get_data(void *src)
{
    plsr_source *s = src;
    return s ? s->data : 0;
}
uintptr_t plsr_dispatch_source_get_handle(void *src)
    __asm__("_dispatch_source_get_handle");
uintptr_t plsr_dispatch_source_get_handle(void *src)
{
    plsr_source *s = src;
    return s ? s->handle : 0;
}
unsigned long plsr_dispatch_source_get_mask(void *src)
    __asm__("_dispatch_source_get_mask");
unsigned long plsr_dispatch_source_get_mask(void *src)
{
    plsr_source *s = src;
    return s ? s->mask : 0;
}
void plsr_dispatch_source_merge_data(void *src, uintptr_t value)
    __asm__("_dispatch_source_merge_data");
void plsr_dispatch_source_merge_data(void *src, uintptr_t value)
{
    plsr_source *s = src;
    if (s)
        s->data += value;
}
void plsr_dispatch_source_set_event_handler(void *src, void *handler)
    __asm__("_dispatch_source_set_event_handler");
void plsr_dispatch_source_set_event_handler(void *src, void *handler)
{
    plsr_source *s = src;
    if (s)
        s->event_handler = handler;
}
void plsr_dispatch_source_set_event_handler_f(void *src, void (*fn)(void *))
    __asm__("_dispatch_source_set_event_handler_f");
void plsr_dispatch_source_set_event_handler_f(void *src, void (*fn)(void *))
{
    plsr_source *s = src;
    if (s)
        s->event_handler_f = fn;
}
void plsr_dispatch_source_set_cancel_handler(void *src, void *handler)
    __asm__("_dispatch_source_set_cancel_handler");
void plsr_dispatch_source_set_cancel_handler(void *src, void *handler)
{
    plsr_source *s = src;
    if (s)
        s->cancel_handler = handler;
}
void plsr_dispatch_source_set_registration_handler(void *src, void *handler)
    __asm__("_dispatch_source_set_registration_handler");
void plsr_dispatch_source_set_registration_handler(void *src, void *handler)
{
    (void)src;
    (void)handler;
}
void plsr_dispatch_source_set_timer(void *src, uint64_t start, uint64_t interval,
                                    uint64_t leeway)
    __asm__("_dispatch_source_set_timer");
void plsr_dispatch_source_set_timer(void *src, uint64_t start, uint64_t interval,
                                    uint64_t leeway)
{
    (void)src;
    (void)start;
    (void)interval;
    (void)leeway;
}

void *plsr_dispatch_io_create(long type, int fd, void *q, void *cleanup)
    __asm__("_dispatch_io_create");
void *plsr_dispatch_io_create(long type, int fd, void *q, void *cleanup)
{
    (void)type;
    (void)fd;
    (void)q;
    (void)cleanup;
    return NULL;
}
void *plsr_dispatch_io_create_with_path(long type, const char *path, int oflag,
                                        unsigned int mode, void *q, void *cleanup)
    __asm__("_dispatch_io_create_with_path");
void *plsr_dispatch_io_create_with_path(long type, const char *path, int oflag,
                                        unsigned int mode, void *q, void *cleanup)
{
    (void)type;
    (void)path;
    (void)oflag;
    (void)mode;
    (void)q;
    (void)cleanup;
    return NULL;
}
void *plsr_dispatch_io_create_with_io(long type, void *io, void *q, void *cleanup)
    __asm__("_dispatch_io_create_with_io");
void *plsr_dispatch_io_create_with_io(long type, void *io, void *q, void *cleanup)
{
    (void)type;
    (void)io;
    (void)q;
    (void)cleanup;
    return NULL;
}
void plsr_dispatch_io_close(void *io, unsigned long flags)
    __asm__("_dispatch_io_close");
void plsr_dispatch_io_close(void *io, unsigned long flags)
{
    (void)io;
    (void)flags;
}
void plsr_dispatch_io_read(void *io, int64_t offset, size_t length, void *q,
                           void *handler) __asm__("_dispatch_io_read");
void plsr_dispatch_io_read(void *io, int64_t offset, size_t length, void *q,
                           void *handler)
{
    (void)io;
    (void)offset;
    (void)length;
    (void)q;
    (void)handler;
}
void plsr_dispatch_io_write(void *io, int64_t offset, void *data, void *q,
                            void *handler) __asm__("_dispatch_io_write");
void plsr_dispatch_io_write(void *io, int64_t offset, void *data, void *q,
                            void *handler)
{
    (void)io;
    (void)offset;
    (void)data;
    (void)q;
    (void)handler;
}
void plsr_dispatch_io_set_interval(void *io, uint64_t interval,
                                   unsigned long flags)
    __asm__("_dispatch_io_set_interval");
void plsr_dispatch_io_set_interval(void *io, uint64_t interval,
                                   unsigned long flags)
{
    (void)io;
    (void)interval;
    (void)flags;
}

void plsr_dispatch_main(void) __asm__("_dispatch_main");
void plsr_dispatch_main(void)
{
    for (;;)
        pause();
}

uint32_t plsr_dispatch_main_port(void) __asm__("__dispatch_get_main_queue_port_4CF");
uint32_t plsr_dispatch_main_port(void) { return 0; }
void plsr_dispatch_main_callback(void) __asm__("__dispatch_main_queue_callback_4CF");
void plsr_dispatch_main_callback(void) {}

void *plsr_dispatch_main_q __asm__("__dispatch_main_q") = NULL;
void *plsr_queue_attr_concurrent __asm__("__dispatch_queue_attr_concurrent") = NULL;

/* ---- group async ---- */

struct group_async {
    void *group;
    void *block;
};

static void *group_async_thread(void *p)
{
    struct group_async *w = p;
    run_block(w->block);
    plsr_dispatch_group_leave(w->group);
    free(w);
    return NULL;
}

void plsr_dispatch_group_async(void *group, void *q, void *block)
    __asm__("_dispatch_group_async");
void plsr_dispatch_group_async(void *group, void *q, void *block)
{
    struct group_async *w;
    pthread_t tid;
    (void)q;
    if (!group || !block)
        return;
    w = calloc(1, sizeof(*w));
    if (!w)
        return;
    w->group = group;
    w->block = block;
    plsr_dispatch_group_enter(group);
    if (pthread_create(&tid, NULL, group_async_thread, w) != 0) {
        plsr_dispatch_group_leave(group);
        free(w);
        return;
    }
    pthread_detach(tid);
}

__attribute__((visibility("default"), used))
const char *const plsr_host_imports[] = {
    "/usr/lib/system/libdispatch.dylib",
    "/usr/lib/libdispatch.dylib",
    NULL
};
