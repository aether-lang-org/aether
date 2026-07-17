/* std.worker — see aether_worker.h.
 *
 * A job is: run `work` on its own OS thread; on return, hand the job either to
 * the host's installed poster (which marshals it to the loop thread and calls
 * aether_worker_deliver there) or, if no poster is installed, onto a
 * mutex-guarded ready-list that aether_worker_drain() pops on the caller's
 * thread. Either way `done(result)` runs on a thread the CALLER controls, never
 * on the worker — that is the whole point.
 *
 * Closure envs are NOT freed here: per the closure-lifetime rules a captured
 * env is a bounded per-closure leak by design (retain-on-capture never
 * releases), the fail-safe direction (leak >> use-after-free). We own and free
 * only the WorkerJob wrapper. The `void*` result is likewise owned by the
 * `done` closure — we never touch it. */
#include "aether_worker.h"
#include "../../runtime/utils/aether_thread.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* ---- job ---------------------------------------------------------------- */

typedef struct WorkerJob {
    AetherWorkerClosure work;
    AetherWorkerClosure done;
    void*               result;
    int                 detached;   /* no done, no post-back */
    struct WorkerJob*   next;       /* ready-list link (drain path) */
} WorkerJob;

/* ---- module state ------------------------------------------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Ready-list of completed jobs awaiting aether_worker_drain() (poster-less
 * path). Singly-linked FIFO: head popped, tail pushed. */
static WorkerJob* g_ready_head = NULL;
static WorkerJob* g_ready_tail = NULL;

/* Host-installed "post to the loop thread" trampoline. fn == NULL means none. */
static AetherWorkerClosure g_poster = { NULL, NULL };

/* Jobs launched but not yet delivered. */
static atomic_int g_pending = 0;

/* ---- closure invocation ------------------------------------------------- */
/* The compiler emits closure calls as fn(env, args...); we recover the real
 * signature with a cast (the fn pointer is type-erased to void(*)(void)). */

static void* call_work(AetherWorkerClosure c) {
    return ((void* (*)(void*))c.fn)(c.env);
}

static void call_done(AetherWorkerClosure c, void* result) {
    ((void (*)(void*, void*))c.fn)(c.env, result);
}

static void call_poster(AetherWorkerClosure c, void* job) {
    ((void (*)(void*, void*))c.fn)(c.env, job);
}

/* ---- ready-list (under g_lock) ----------------------------------------- */

static void ready_push(WorkerJob* job) {
    job->next = NULL;
    if (g_ready_tail) g_ready_tail->next = job;
    else              g_ready_head = job;
    g_ready_tail = job;
}

static WorkerJob* ready_pop(void) {
    WorkerJob* job = g_ready_head;
    if (job) {
        g_ready_head = job->next;
        if (!g_ready_head) g_ready_tail = NULL;
        job->next = NULL;
    }
    return job;
}

/* ---- worker thread entry ----------------------------------------------- */

static void* worker_entry(void* arg) {
    WorkerJob* job = (WorkerJob*)arg;

    job->result = call_work(job->work);

    if (job->detached) {
        /* No post-back, no delivery: the result is the work closure's own
         * business (it either returns something disposable or NULL). */
        atomic_fetch_sub(&g_pending, 1);
        free(job);
        return NULL;
    }

    /* Snapshot the poster under the lock; if one is installed, hand the job to
     * it (off-thread) so the host can marshal to the loop thread. Otherwise
     * queue for drain. */
    pthread_mutex_lock(&g_lock);
    AetherWorkerClosure poster = g_poster;
    int have_poster = (poster.fn != NULL);
    if (!have_poster) ready_push(job);
    pthread_mutex_unlock(&g_lock);

    if (have_poster) {
        call_poster(poster, job);   /* host marshals -> aether_worker_deliver */
    }
    return NULL;
}

/* ---- launch ------------------------------------------------------------- */

static int launch(AetherWorkerClosure work, AetherWorkerClosure done, int detached) {
    if (!work.fn) return 0;

    WorkerJob* job = (WorkerJob*)calloc(1, sizeof(WorkerJob));
    if (!job) return 0;
    job->work = work;
    job->done = done;
    job->detached = detached;

    atomic_fetch_add(&g_pending, 1);

    pthread_t th;
    if (pthread_create(&th, NULL, worker_entry, job) != 0) {
        atomic_fetch_sub(&g_pending, 1);
        free(job);
        return 0;
    }
    pthread_detach(th);   /* self-reaping; we never join */
    return 1;
}

int aether_worker_run(AetherWorkerClosure work, AetherWorkerClosure done) {
    return launch(work, done, 0);
}

int aether_worker_run_detached(AetherWorkerClosure work) {
    AetherWorkerClosure none = { NULL, NULL };
    return launch(work, none, 1);
}

/* ---- poster / delivery / drain ----------------------------------------- */

void aether_worker_set_main_poster(AetherWorkerClosure poster) {
    pthread_mutex_lock(&g_lock);
    g_poster = poster;
    pthread_mutex_unlock(&g_lock);
}

void aether_worker_deliver(void* arg) {
    if (!arg) return;
    WorkerJob* job = (WorkerJob*)arg;
    if (job->done.fn) call_done(job->done, job->result);
    atomic_fetch_sub(&g_pending, 1);
    free(job);
}

int aether_worker_drain(int max) {
    int ran = 0;
    for (;;) {
        if (max > 0 && ran >= max) break;
        pthread_mutex_lock(&g_lock);
        WorkerJob* job = ready_pop();
        pthread_mutex_unlock(&g_lock);
        if (!job) break;
        /* deliver() runs done + frees + decrements pending, all on this
         * (the caller's) thread. */
        aether_worker_deliver(job);
        ran++;
    }
    return ran;
}

int aether_worker_pending(void) {
    return atomic_load(&g_pending);
}
