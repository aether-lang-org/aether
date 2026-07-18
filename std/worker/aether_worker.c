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

/* Threads are the whole point of this module — but the cooperative /
 * AETHER_NO_THREADING build has no scheduler threads and deliberately leaves
 * pthread_create unstubbed (aether_thread.h: "calling them on a threadless
 * platform is a logic error and should fail at link time"). So when threading
 * is off we do NOT spawn: `work` runs synchronously on the caller's thread at
 * launch, and its result is delivered through the normal loop path (poster or
 * drain). No parallelism — but the contract (work runs; done fires on the loop
 * thread) still holds, which is exactly the cooperative model. This mirrors the
 * h2 server's "sequential fallback" when its pool is unavailable. */
#if defined(AETHER_NO_THREADING)
#  define AETHER_WORKER_HAS_THREADS 0
#else
#  define AETHER_WORKER_HAS_THREADS 1
#endif

/* ---- job ---------------------------------------------------------------- */

typedef struct WorkerJob {
    AetherWorkerClosure work;
    AetherWorkerClosure done;
    void*               result;
    int                 detached;   /* no done, no post-back */
    struct WorkerJob*   next;       /* ready-list link (drain path) */
} WorkerJob;

/* ---- module state ------------------------------------------------------- */

/* g_lock guards the ready-list and the poster slot. It is initialised lazily:
 * the Win32 threading shim maps pthread_mutex_t to a CRITICAL_SECTION, which
 * has no static initialiser (PTHREAD_MUTEX_INITIALIZER is POSIX-only), so we
 * cannot init it at file scope. ensure_init() runs pthread_mutex_init exactly
 * once, gated by an atomic CAS; first entry is a plain call before any worker
 * thread exists, so there is effectively no contention. */
static pthread_mutex_t g_lock;
static atomic_int g_init_state = 0;   /* 0 = uninit, 1 = initialising, 2 = ready */

static void ensure_init(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_init_state, &expected, 1)) {
        pthread_mutex_init(&g_lock, NULL);
        atomic_store(&g_init_state, 2);
        return;
    }
    /* Another thread is initialising (or already did): wait until ready. */
    while (atomic_load(&g_init_state) != 2) { /* spin — bounded, one-shot */ }
}

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

/* ---- job execution ----------------------------------------------------- */

/* Reclaim a job's per-job closure environments. The env is a plain malloc
 * block the compiler heap-allocated for the closure's captures; because we
 * received the closures through an extern boundary, codegen does NOT free them
 * (the env-drain is suppressed — closure_extern_retains_no_uaf), so ownership
 * is ours once the closures have fired. Freeing here keeps a long-lived app
 * (thousands of worker.run calls) from leaking an env per job. NOTE: if a
 * capture is a retained AetherString, its refcount is not released by this
 * plain free (the retain-on-capture ref is never released by design) — that
 * residual is the language's existing bounded per-closure behaviour, not new.
 * The poster env is host-owned (installed once) and is never touched here. */
static void free_job_envs(WorkerJob* job) {
    if (job->work.env) free(job->work.env);
    if (job->done.env) free(job->done.env);
}

/* Run the work closure, then route the completion. Called on a worker thread
 * (threaded build) or inline on the caller's thread (AETHER_NO_THREADING). */
static void run_job(WorkerJob* job) {
    job->result = call_work(job->work);

    if (job->detached) {
        /* No post-back, no delivery: the result is the work closure's own
         * business (it either returns something disposable or NULL). */
        free_job_envs(job);
        atomic_fetch_sub(&g_pending, 1);
        free(job);
        return;
    }

    /* Snapshot the poster under the lock; if one is installed, hand the job to
     * it so the host can marshal to the loop thread. Otherwise queue for drain. */
    pthread_mutex_lock(&g_lock);
    AetherWorkerClosure poster = g_poster;
    int have_poster = (poster.fn != NULL);
    if (!have_poster) ready_push(job);
    pthread_mutex_unlock(&g_lock);

    if (have_poster) {
        call_poster(poster, job);   /* host marshals -> aether_worker_deliver */
    }
}

#if AETHER_WORKER_HAS_THREADS
static void* worker_entry(void* arg) {
    run_job((WorkerJob*)arg);
    return NULL;
}
#endif

/* ---- launch ------------------------------------------------------------- */

static int launch(AetherWorkerClosure work, AetherWorkerClosure done, int detached) {
    if (!work.fn) return 0;
    ensure_init();   /* runs on the caller's thread, before the worker starts */

    WorkerJob* job = (WorkerJob*)calloc(1, sizeof(WorkerJob));
    if (!job) return 0;
    job->work = work;
    job->done = done;
    job->detached = detached;

    atomic_fetch_add(&g_pending, 1);

#if AETHER_WORKER_HAS_THREADS
    pthread_t th;
    if (pthread_create(&th, NULL, worker_entry, job) != 0) {
        atomic_fetch_sub(&g_pending, 1);
        free(job);
        return 0;
    }
    pthread_detach(th);   /* self-reaping; we never join */
#else
    /* Cooperative / threadless build: no off-thread execution. Run the work
     * synchronously here; the completion still flows through the poster/drain
     * path, so `done` fires on the loop thread on the next deliver/drain. */
    run_job(job);
#endif
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
    ensure_init();
    pthread_mutex_lock(&g_lock);
    g_poster = poster;
    pthread_mutex_unlock(&g_lock);
}

void aether_worker_deliver(void* arg) {
    if (!arg) return;
    WorkerJob* job = (WorkerJob*)arg;
    if (job->done.fn) call_done(job->done, job->result);
    free_job_envs(job);   /* work + done envs are ours now that both have fired */
    atomic_fetch_sub(&g_pending, 1);
    free(job);
}

int aether_worker_drain(int max) {
    ensure_init();
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
