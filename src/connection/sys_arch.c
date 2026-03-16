// lwIP NO_SYS port: provides sys_now() required when NO_SYS=1 and LWIP_TIMERS=1.
// timeouts.c calls sys_now() to get the current time in milliseconds.

#include "lwip/sys.h"

#ifdef _WIN32
#include <windows.h>
u32_t sys_now(void) {
    return (u32_t)GetTickCount();
}
#else
#include <time.h>
u32_t sys_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u32_t)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
}
#endif
