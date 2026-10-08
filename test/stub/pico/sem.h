// Host-side stub of <pico/sem.h>. The acquire calls return at once, so the receive loop keeps
// testing the queue itself and the suite retains its polling behaviour; releases are counted so a
// test can see the handler signal a waiting receive().
#ifndef _STUB_PICO_SEM_H
#define _STUB_PICO_SEM_H

#include <stdbool.h>
#include <stdint.h>

typedef struct semaphore {
    int16_t permits;
    int16_t max_permits;
} semaphore_t;

/// Releases counted across the run, so a test can watch for them.
extern int g_sem_releases;

static inline void sem_init(semaphore_t *sem, int16_t initial_permits, int16_t max_permits) {
    sem->permits = initial_permits;
    sem->max_permits = max_permits;
}
static inline void sem_reset(semaphore_t *sem, int16_t permits) { sem->permits = permits; }
static inline void sem_acquire_blocking(semaphore_t *) {}
static inline bool sem_acquire_timeout_us(semaphore_t *, uint32_t) { return true; }

void sem_release(semaphore_t *sem);

#endif
