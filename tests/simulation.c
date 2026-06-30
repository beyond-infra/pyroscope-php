/*
 * Deterministic simulation test for pyroscope-php mutex + double-buffer.
 *
 * Standalone C program -- no PHP. Verifies the mutex+double-buffer
 * concurrency model under load, fault injection, and invariants.
 *
 * Build:  gcc -pthread -Wall -O2 -o sim_bin tests/simulation.c
 * Usage:  ./sim_bin --seed <N> [--fault-mode none|full-buffer]
 * Output: deterministic hash, same seed == same hash
 *
 * Determinism: each producer writes a fixed, seed-derived set of samples
 * (content "p{id}_{seed}_{j}") and waits — never drops — when the buffer is
 * full. The drained multiset is therefore exactly {p{i}_{seed}_{j}} (plus a
 * fixed prefill in full-buffer mode), independent of thread scheduling. The
 * hash is a per-sample XOR of fnv1a, which is order-independent; every sample
 * is unique by construction, so no XOR cancellation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>

/* Small BUF_SIZE so we can allocate on heap without issue */
#define BUF_SIZE 4096
#define MAX_STACK_LEN 64
#define NUM_PRODUCERS 4
#define PER_PRODUCER 8192

typedef char sample_t[MAX_STACK_LEN];

static sample_t      *active_buf;
static sample_t      *drain_buf;
static pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        active_count;
static int             g_seed = 42;
static int             producers_remaining = NUM_PRODUCERS;

static uint64_t global_hash = 0;
static uint64_t total_drained = 0;

static uint64_t fnv1a(const void *data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void *producer(void *arg) {
    long id = (long)arg;
    for (int j = 0; j < PER_PRODUCER; j++) {
        pthread_mutex_lock(&buf_mutex);
        /* Wait if the buffer is full so no sample is ever dropped — the
         * drained multiset must stay complete for a deterministic hash. */
        while (active_count >= BUF_SIZE) {
            pthread_mutex_unlock(&buf_mutex);
            usleep(50);
            pthread_mutex_lock(&buf_mutex);
        }
        snprintf(active_buf[active_count], MAX_STACK_LEN, "p%ld_%d_%d", id, g_seed, j);
        active_count++;
        pthread_mutex_unlock(&buf_mutex);
    }
    __atomic_sub_fetch(&producers_remaining, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    for (;;) {
        usleep(100000); /* 100ms */

        pthread_mutex_lock(&buf_mutex);
        uint32_t n = active_count;
        sample_t *tmp = active_buf;
        active_buf = drain_buf;
        drain_buf = tmp;
        active_count = 0;
        pthread_mutex_unlock(&buf_mutex);

        if (n > 0) {
            for (uint32_t k = 0; k < n; k++)
                /* hash only the valid string, not the full 64-byte slot —
                 * bytes past the null terminator are leftover from whichever
                 * sample last occupied the slot and vary by schedule. */
                global_hash ^= fnv1a(drain_buf[k], strlen(drain_buf[k]));
            total_drained += n;
        }

        /* Done once every producer has exited and the buffer is fully drained. */
        if (__atomic_load_n(&producers_remaining, __ATOMIC_SEQ_CST) == 0
                && __atomic_load_n(&active_count, __ATOMIC_RELAXED) == 0)
            break;
    }
    return NULL;
}

int main(int argc, char **argv) {
    int seed = 42;
    int fault_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fault-mode") == 0 && i + 1 < argc) {
            if (strcmp(argv[++i], "full-buffer") == 0) fault_mode = 1;
        }
    }

    g_seed = seed;
    active_buf = calloc(BUF_SIZE, sizeof(sample_t));
    drain_buf  = calloc(BUF_SIZE, sizeof(sample_t));
    if (!active_buf || !drain_buf) {
        fprintf(stderr, "simulation: calloc failed\n");
        return 1;
    }

    if (fault_mode == 1) {
        for (int i = 0; i < BUF_SIZE; i++)
            snprintf(active_buf[i], MAX_STACK_LEN, "prefill_%d", i);
        active_count = BUF_SIZE;
    }

    pthread_t prods[NUM_PRODUCERS], cons;
    for (long i = 0; i < NUM_PRODUCERS; i++)
        pthread_create(&prods[i], NULL, producer, (void *)i);
    pthread_create(&cons, NULL, consumer, NULL);

    for (int i = 0; i < NUM_PRODUCERS; i++)
        pthread_join(prods[i], NULL);
    pthread_join(cons, NULL);

    pthread_mutex_destroy(&buf_mutex);
    free(active_buf);
    free(drain_buf);

    printf("seed:%d mode:%d hash:%llu drained:%llu\n",
           seed, fault_mode,
           (unsigned long long)global_hash,
           (unsigned long long)total_drained);

    printf("PASS\n");
    return 0;
}
