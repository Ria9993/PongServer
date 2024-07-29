#pragma once

#define PORT 9180
#define MAX_SESSION 1000
#define NUM_SESSION_WORKER_THREAD 6 // Typically, twice the number of CPU cores
#define CACHE_LINE 64
#define SERVER_TICK_RATE 20 // Per Sec

// Only support x86 or x86_64 architecture
#if !defined(__x86_64__) && !defined(__i386__)
    // #error "Only support x86 or x86_64 architecture"
#endif
