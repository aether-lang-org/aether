#include "aether_tracing.h"
#include "aether_thread.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

TracingConfig global_tracing = {0};

static double get_current_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

// Tracing control
void tracing_init(const char* log_filename) {
    if (global_tracing.log_file) {
        fclose(global_tracing.log_file);
    }
    
    if (log_filename) {
        global_tracing.log_file = fopen(log_filename, "w");
        if (global_tracing.log_file) {
            fprintf(global_tracing.log_file, "# Aether Message Trace Log\n");
            fprintf(global_tracing.log_file, "# Format: [T+{time}ms] actor_{id}: {event}\n\n");
            fflush(global_tracing.log_file);
        }
    } else {
        global_tracing.log_file = stderr;
    }
    
    global_tracing.enabled = 1;
    global_tracing.trace_all = 0;
    global_tracing.traced_actor_ids = NULL;
    global_tracing.traced_actor_count = 0;
    global_tracing.start_time = get_current_time_ms();
}

void tracing_shutdown() {
    if (global_tracing.log_file && global_tracing.log_file != stderr) {
        fclose(global_tracing.log_file);
    }
    
    if (global_tracing.traced_actor_ids) {
        free(global_tracing.traced_actor_ids);
    }
    
    memset(&global_tracing, 0, sizeof(TracingConfig));
}

void tracing_enable() {
    global_tracing.enabled = 1;
}

void tracing_disable() {
    global_tracing.enabled = 0;
}

// Actor-specific tracing
void tracing_add_actor(int actor_id) {
    // Check if already traced
    for (int i = 0; i < global_tracing.traced_actor_count; i++) {
        if (global_tracing.traced_actor_ids[i] == actor_id) {
            return;
        }
    }
    
    // Add actor. Grow through a temp so a failed realloc keeps the existing
    // array (and doesn't leak it) instead of overwriting it with NULL and then
    // dereferencing that NULL on the store below.
    int* grown = (int*)realloc(
        global_tracing.traced_actor_ids,
        (global_tracing.traced_actor_count + 1) * sizeof(int)
    );
    if (!grown) return;
    global_tracing.traced_actor_ids = grown;
    global_tracing.traced_actor_ids[global_tracing.traced_actor_count++] = actor_id;
}

void tracing_remove_actor(int actor_id) {
    for (int i = 0; i < global_tracing.traced_actor_count; i++) {
        if (global_tracing.traced_actor_ids[i] == actor_id) {
            // Shift remaining actors
            for (int j = i; j < global_tracing.traced_actor_count - 1; j++) {
                global_tracing.traced_actor_ids[j] = global_tracing.traced_actor_ids[j + 1];
            }
            global_tracing.traced_actor_count--;
            break;
        }
    }
}

void tracing_set_trace_all(int enabled) {
    global_tracing.trace_all = enabled;
}

// Event logging
void tracing_log_received(int actor_id, int msg_type, int msg_payload) {
    if (!global_tracing.enabled || !tracing_should_trace(actor_id)) return;
    if (!global_tracing.log_file) return;
    
    double elapsed = tracing_get_elapsed_time();
    fprintf(global_tracing.log_file, 
            "[T+%.3fms] actor_%d: received msg{type=%d, payload=%d}\n",
            elapsed, actor_id, msg_type, msg_payload);
    fflush(global_tracing.log_file);
}

void tracing_log_sent(int from_actor_id, int to_actor_id, int msg_type, int msg_payload) {
    if (!global_tracing.enabled) return;
    if (!tracing_should_trace(from_actor_id) && !tracing_should_trace(to_actor_id)) return;
    if (!global_tracing.log_file) return;
    
    double elapsed = tracing_get_elapsed_time();
    fprintf(global_tracing.log_file,
            "[T+%.3fms] actor_%d: sent msg{type=%d, payload=%d} to actor_%d\n",
            elapsed, from_actor_id, msg_type, msg_payload, to_actor_id);
    fflush(global_tracing.log_file);
}

void tracing_log_processed(int actor_id, int msg_type, int msg_payload) {
    if (!global_tracing.enabled || !tracing_should_trace(actor_id)) return;
    if (!global_tracing.log_file) return;
    
    double elapsed = tracing_get_elapsed_time();
    fprintf(global_tracing.log_file,
            "[T+%.3fms] actor_%d: processed msg{type=%d, payload=%d}\n",
            elapsed, actor_id, msg_type, msg_payload);
    fflush(global_tracing.log_file);
}

void tracing_log_custom(int actor_id, const char* event) {
    if (!global_tracing.enabled || !tracing_should_trace(actor_id)) return;
    if (!global_tracing.log_file) return;
    
    double elapsed = tracing_get_elapsed_time();
    fprintf(global_tracing.log_file,
            "[T+%.3fms] actor_%d: %s\n",
            elapsed, actor_id, event);
    fflush(global_tracing.log_file);
}

// Helper
int tracing_should_trace(int actor_id) {
    if (global_tracing.trace_all) return 1;
    
    for (int i = 0; i < global_tracing.traced_actor_count; i++) {
        if (global_tracing.traced_actor_ids[i] == actor_id) {
            return 1;
        }
    }
    
    return 0;
}

double tracing_get_elapsed_time() {
    return get_current_time_ms() - global_tracing.start_time;
}

