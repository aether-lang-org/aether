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

// Message: Increment
typedef struct Increment {
    int _message_id;
    int amount;
} Increment;

// Message: Response
typedef struct Response {
    int _message_id;
    int value;
    int status;
} Response;

int main() {
    {
void msg = (Increment){.amount = 5};
void resp = (Response){.value = 10, .status = 1};
    }
    return 0;
}
