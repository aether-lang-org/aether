/* Copyright (c) 2026 Aether Developers. */
#ifndef AETHER_TRACKING_H
#define AETHER_TRACKING_H

#include "../alloc/aether_alloc.h"

/* std.tracking (#1049): a leak-detecting allocator wrapper. */

AetherAllocator* aether_tracking_wrap(AetherAllocator* inner);
int  aether_tracking_count(AetherAllocator* t);
long aether_tracking_bytes(AetherAllocator* t);
int  aether_tracking_report(AetherAllocator* t);
void aether_tracking_destroy(AetherAllocator* t);

#endif
