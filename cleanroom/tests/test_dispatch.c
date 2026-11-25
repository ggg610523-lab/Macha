/* Exercise libplsr-dispatch.so from the host: queues, sync/async, once,
 * semaphores, groups and apply. Handles are dlopen'd so the test does not
 * need the Darwin spelling at link time. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *(*queue_create_fn)(const char *, void *);
typedef void *(*get_queue_fn)(long, unsigned long);
typedef void (*sync_fn)(void *, void *);
typedef void (*sync_f_fn)(void *, void *, void (*)(void *));
typedef void (*async_f_fn)(void *, void *, void (*)(void *));
typedef void *(*sem_create_fn)(long);
typedef long (*sem_wait_fn)(void *, unsigned long long);
typedef long (*sem_signal_fn)(void *);
typedef void (*once_fn)(long *, void *, void (*)(void *));
typedef void *(*group_create_fn)(void);
typedef void (*group_fn)(void *);
typedef long (*group_wait_fn)(void *, unsigned long long);
typedef void (*apply_fn)(size_t, void *, void *);

static int failures;
#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL: %s\n", msg);                           \
            failures++;                                                   \
        }                                                                 \
    } while (0)

struct block {
    void *isa;
    int flags;
    int reserved;
    void (*invoke)(void *);
    void *descriptor;
    int *counter;
};

struct index_block {
    void *isa;
    int flags;
    int reserved;
    void (*invoke)(void *, size_t);
    void *descriptor;
    long sum;
};

static void block_count(void *p)
{
    struct block *b = p;
    (*b->counter)++;
}

static void index_add(void *p, size_t i)
{
    struct index_block *b = p;
    b->sum += (long)i;
}

static void sync_count(void *ctx) { (*(int *)ctx)++; }

static sem_signal_fn g_sem_signal;
static void *g_sem;

static void async_count(void *ctx)
{
    (*(int *)ctx)++;
    if (g_sem_signal && g_sem)
        g_sem_signal(g_sem);
}

static void once_count(void *ctx) { (*(int *)ctx)++; }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/libplsr-dispatch.so";
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    queue_create_fn queue_create;
    get_queue_fn get_global;
    sync_fn sync;
    sync_f_fn sync_f;
    async_f_fn async_f;
    sem_create_fn sem_create;
    sem_wait_fn sem_wait;
    sem_signal_fn sem_signal;
    once_fn once_f;
    group_create_fn group_create;
    group_fn group_enter, group_leave;
    group_wait_fn group_wait;
    apply_fn apply;
    void *q, *sem, *group;
    int counter = 0, async_hits = 0, once = 0;
    long once_predicate = 0;
    struct block b = {0};
    struct index_block ib = {0};

    if (!h) {
        fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
        return 2;
    }
#define LOAD(var, name)                                                   \
    do {                                                                  \
        *(void **)(&var) = dlsym(h, name);                                \
        if (!var) {                                                       \
            fprintf(stderr, "missing %s\n", name);                        \
            return 2;                                                     \
        }                                                                 \
    } while (0)
    LOAD(queue_create, "_dispatch_queue_create");
    LOAD(get_global, "_dispatch_get_global_queue");
    LOAD(sync, "_dispatch_sync");
    LOAD(sync_f, "_dispatch_sync_f");
    LOAD(async_f, "_dispatch_async_f");
    LOAD(sem_create, "_dispatch_semaphore_create");
    LOAD(sem_wait, "_dispatch_semaphore_wait");
    LOAD(sem_signal, "_dispatch_semaphore_signal");
    LOAD(once_f, "_dispatch_once_f");
    LOAD(group_create, "_dispatch_group_create");
    LOAD(group_enter, "_dispatch_group_enter");
    LOAD(group_leave, "_dispatch_group_leave");
    LOAD(group_wait, "_dispatch_group_wait");
    LOAD(apply, "_dispatch_apply");
#undef LOAD

    q = queue_create("plsr.test", NULL);
    CHECK(q != NULL, "queue_create returned NULL");
    CHECK(get_global(0, 0) != NULL, "get_global_queue returned NULL");

    sync_f(q, &counter, sync_count);
    CHECK(counter == 1, "dispatch_sync_f did not run the function");

    b.invoke = block_count;
    b.counter = &counter;
    sync(q, &b);
    CHECK(counter == 2, "dispatch_sync did not invoke the block");

    sem = sem_create(0);
    CHECK(sem != NULL, "semaphore_create returned NULL");
    g_sem = sem;
    g_sem_signal = sem_signal;
    async_f(q, &async_hits, async_count);
    CHECK(sem_wait(sem, 2000000000ull) == 0, "async work did not signal");
    CHECK(async_hits == 1, "dispatch_async_f did not run the function");

    once_f(&once_predicate, &once, once_count);
    once_f(&once_predicate, &once, once_count);
    CHECK(once == 1, "dispatch_once_f ran more than once");

    group = group_create();
    CHECK(group != NULL, "group_create returned NULL");
    group_enter(group);
    group_leave(group);
    CHECK(group_wait(group, 0) == 0, "empty group did not complete immediately");
    group_enter(group);
    CHECK(group_wait(group, 0) != 0, "open group reported completion");
    group_leave(group);

    ib.invoke = index_add;
    apply(4, q, &ib);
    CHECK(ib.sum == 6, "dispatch_apply did not visit every index");

    if (failures) {
        fprintf(stderr, "%d dispatch check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: dispatch queues, sync/async, once, semaphores, groups, apply\n");
    return 0;
}
