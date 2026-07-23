// Stress test to find performance bottlenecks
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "profiler_server.h"
#include "../../runtime/memory/aether_memory_stats.h"
#include "../../runtime/memory/aether_arena.h"

#ifdef _WIN32
    #include <windows.h>
    #define sleep(n) Sleep((n) * 1000)
    #define usleep(n) Sleep((n) / 1000)
#else
    #include <unistd.h>
#endif

// Simulate high-load actor system
typedef struct {
    int id;
    int messages_processed;
    Arena* arena;
} StressActor;

double get_time_ms() {
    struct timespec ts;
    #ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
    #else
    clock_gettime(CLOCK_MONOTONIC, &ts);
    #endif
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

StressActor* create_stress_actor(int id) {
    StressActor* actor = malloc(sizeof(StressActor));
    actor->id = id;
    actor->messages_processed = 0;
    actor->arena = arena_create(1024 * 64); // 64KB per actor
    
    memory_stats_record_alloc(sizeof(StressActor));
    
    ProfilerEvent event = {
        .type = PROF_EVENT_ACTOR_SPAWN,
        .timestamp_ms = profiler_get_time_ms(),
        .actor_id = id,
        .memory_bytes = sizeof(StressActor)
    };
    profiler_record_event(&event);
    
    return actor;
}

void process_messages(StressActor* actor, int count) {
    double start = get_time_ms();
    
    for (int i = 0; i < count; i++) {
        // Simulate message processing with memory allocation
        arena_alloc(actor->arena, 32 + (i % 128));
        memory_stats_record_alloc(32 + (i % 128));
        
        actor->messages_processed++;
        
        // Record every 100th message to reduce overhead
        if (i % 100 == 0) {
            ProfilerEvent event = {
                .type = PROF_EVENT_ACTOR_MESSAGE_PROCESSED,
                .timestamp_ms = profiler_get_time_ms(),
                .actor_id = actor->id,
                .message_type = 1,
                .memory_bytes = 32 + (i % 128)
            };
            profiler_record_event(&event);
        }
    }
    
    double elapsed = get_time_ms() - start;
    printf("  Actor %d: Processed %d messages in %.2fms (%.0f msg/sec)\n",
           actor->id, count, elapsed, (count / elapsed) * 1000.0);
}

int main() {
    printf("╔════════════════════════════════════════════════╗\n");
    printf("║   Aether Profiler - Stress Test               ║\n");
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    memory_stats_init();
    
    ProfilerConfig config = {
        .enabled = 1,
        .port = 8081,
        .bind_address = "0.0.0.0",
        .max_events = 50000,  // Larger buffer for stress test
        .collection_interval_ms = 50
    };
    
    profiler_init(&config);
    profiler_start_server();
    
    printf("Open browser: http://localhost:8081\n\n");
    sleep(2);
    
    // Test 1: Many actors, few messages each
    printf("═══ Test 1: Many Actors (100 actors × 100 messages) ═══\n");
    double test1_start = get_time_ms();
    
    StressActor** actors1 = malloc(100 * sizeof(StressActor*));
    for (int i = 0; i < 100; i++) {
        actors1[i] = create_stress_actor(i + 1);
    }
    
    for (int i = 0; i < 100; i++) {
        process_messages(actors1[i], 100);
    }
    
    double test1_time = get_time_ms() - test1_start;
    printf("\n✓ Test 1 Complete: %.2fms (10,000 messages)\n", test1_time);
    printf("  Throughput: %.0f msg/sec\n\n", (10000.0 / test1_time) * 1000.0);
    
    sleep(2);
    
    // Test 2: Few actors, many messages each
    printf("═══ Test 2: High Load (5 actors × 5,000 messages) ═══\n");
    double test2_start = get_time_ms();
    
    StressActor** actors2 = malloc(5 * sizeof(StressActor*));
    for (int i = 0; i < 5; i++) {
        actors2[i] = create_stress_actor(i + 101);
    }
    
    for (int i = 0; i < 5; i++) {
        process_messages(actors2[i], 5000);
    }
    
    double test2_time = get_time_ms() - test2_start;
    printf("\n✓ Test 2 Complete: %.2fms (25,000 messages)\n", test2_time);
    printf("  Throughput: %.0f msg/sec\n\n", (25000.0 / test2_time) * 1000.0);
    
    sleep(2);
    
    // Test 3: Arena reset performance
    printf("═══ Test 3: Arena Reset (1,000 cycles) ═══\n");
    double test3_start = get_time_ms();
    
    Arena* test_arena = arena_create(1024 * 1024); // 1MB
    
    for (int round = 0; round < 1000; round++) {
        // Allocate 1000 small objects
        for (int i = 0; i < 1000; i++) {
            arena_alloc(test_arena, 64);
        }
        // Reset arena (fast bulk free)
        arena_reset(test_arena);
    }
    
    double test3_time = get_time_ms() - test3_start;
    printf("✓ Test 3 Complete: %.2fms (1,000,000 allocations)\n", test3_time);
    printf("  Throughput: %.0f ops/sec\n\n", (1000000.0 / test3_time) * 1000.0);
    
    arena_destroy(test_arena);
    
    // Memory stats
    printf("═══ Memory Statistics ═══\n");
    memory_stats_print();
    
    // Performance summary
    printf("\n╔════════════════════════════════════════════════╗\n");
    printf("║   Performance Summary                          ║\n");
    printf("╠════════════════════════════════════════════════╣\n");
    printf("║ Test 1: %7.0f msg/sec (many actors)          ║\n", (10000.0 / test1_time) * 1000.0);
    printf("║ Test 2: %7.0f msg/sec (high load)            ║\n", (25000.0 / test2_time) * 1000.0);
    printf("║ Test 3: %7.0f ops/sec (arena reset)          ║\n", (1000000.0 / test3_time) * 1000.0);
    printf("╚════════════════════════════════════════════════╝\n\n");
    
    printf("Profiler running for 10 more seconds...\n");
    printf("Check dashboard for detailed metrics!\n\n");
    
    for (int i = 10; i > 0; i--) {
        printf("\r  Time remaining: %2d seconds... ", i);
        fflush(stdout);
        sleep(1);
    }
    
    printf("\n\n");
    
    // Cleanup
    for (int i = 0; i < 100; i++) {
        arena_destroy(actors1[i]->arena);
        free(actors1[i]);
    }
    free(actors1);
    
    for (int i = 0; i < 5; i++) {
        arena_destroy(actors2[i]->arena);
        free(actors2[i]);
    }
    free(actors2);
    
    profiler_shutdown();
    
    printf("Stress test complete!\n");
    return 0;
}

