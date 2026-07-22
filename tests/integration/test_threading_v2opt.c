#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

// Aether runtime libraries
#include "actor_state_machine.h"
#include "multicore_scheduler.h"
#include "aether_cpu_detect.h"
#include "aether_string.h"
#include "aether_io.h"
#include "aether_math.h"
#include "aether_tracing.h"
#include "aether_bounds_check.h"
#include "aether_runtime_types.h"

extern __thread int current_core_id;

// Message: Increment (1 fields)
typedef struct Increment {
    int _message_id;
    int amount;
} Increment;

typedef struct __attribute__((aligned(64))) Counter {
    atomic_int active;       // Hot: checked every loop iteration
    int id;                  // Hot: used for identification
    Mailbox mailbox;         // Hot: message queue
    void (*step)(void*);     // Hot: message handler
    pthread_t thread;        // Warm: thread handle
    int auto_process;        // Warm: auto-processing flag
    atomic_int assigned_core; // Cold: core assignment (atomic for work-stealing)

} Counter;

void Counter_step(Counter* self) {
    Message msg;
    
    // Likely path: mailbox has message
    if (__builtin_expect(!mailbox_receive(&self->mailbox, &msg), 0)) {
        atomic_store_explicit(&self->active, 0, memory_order_relaxed);
        return;
    }
    
    void* _msg_data = msg.data;
    int _msg_id = *(int*)_msg_data;
    
}

Counter* spawn_Counter() {
    // Use cache-aligned allocation for better performance
    Counter* actor = aligned_alloc(64, sizeof(Counter));
    if (!actor) return NULL;
    
    actor->id = atomic_fetch_add(&next_actor_id, 1);
    atomic_init(&actor->active, 1);
    actor->assigned_core = -1;
    actor->step = (void (*)(void*))Counter_step;
    actor->auto_process = 1;
    mailbox_init(&actor->mailbox);
    
    
    if (actor->auto_process) {
        pthread_create(&actor->thread, NULL, (void*(*)(void*))aether_actor_thread, actor);
    }
    
    scheduler_register_actor((ActorBase*)actor, -1);
    return actor;
}

void send_Counter(Counter* actor, int type, int payload) {
    Message msg = {type, 0, payload, NULL};
    if (actor->assigned_core == current_core_id) {
        scheduler_send_local((ActorBase*)actor, msg);
    } else {
        scheduler_send_remote((ActorBase*)actor, msg, current_core_id);
    }
}

int main() {
    // Initialize multi-core actor scheduler
    int num_cores = cpu_recommend_cores();
    MulticoreScheduler* scheduler = scheduler_create(num_cores);
    if (!scheduler) {
        fprintf(stderr, "Failed to create actor scheduler\n");
        return 1;
    }
    scheduler_start(scheduler);
    current_core_id = 0;
    
    {
printf("%d\n", 42);
    }
    
    // Wait for actors to complete and clean up
    scheduler_join(scheduler);
    scheduler_destroy(scheduler);
    return 0;
}
