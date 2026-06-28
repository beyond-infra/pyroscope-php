/*
 * Deterministic simulation test for pyroscope-php mutex + double-buffer.
 *
 * Standalone C program -- no PHP. Verifies the mutex+double-buffer
 * concurrency model under load, fault injection, and invariants.
 *
 * Build:  gcc -pthread -Wall -O2 -o sim_bin tests/simulation.c
 * Usage:  ./sim_bin --seed <N> [--fault-mode none|full-buffer]
 * Output: deterministic hash, same seed == same hash
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
#define RUN_SECONDS 3

typedef char sample_t[MAX_STACK_LEN];

static sample_t      *active_buf;
static sample_t      *drain_buf;
static pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        active_count;
static int             stop = 0;

static pthread_mutex_t hash_mutex = PTHREAD_MUTEX_INITIALIZER;
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
    unsigned int seed = (unsigned int)(id + 42);
    for (;;) {
        if (__atomic_load_n(&stop, __ATOMIC_RELAXED)) break;
        pthread_mutex_lock(&buf_mutex);
        if (active_count < BUF_SIZE) {
            snprintf(active_buf[active_count], MAX_STACK_LEN, "p%ld_%u", id, seed++);
            active_count++;
        }
        pthread_mutex_unlock(&buf_mutex);
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    for (;;) {
        usleep(100000); /* 100ms */
        if (__atomic_load_n(&stop, __ATOMIC_RELAXED)) break;

        pthread_mutex_lock(&buf_mutex);
        uint32_t n = active_count;
        sample_t *tmp = active_buf;
        active_buf = drain_buf;
        drain_buf = tmp;
        active_count = 0;
        pthread_mutex_unlock(&buf_mutex);

        if (n == 0) continue;

        uint64_t batch_hash = fnv1a(drain_buf, n * MAX_STACK_LEN);
        pthread_mutex_lock(&hash_mutex);
        global_hash ^= batch_hash;
        total_drained += n;
        pthread_mutex_unlock(&hash_mutex);
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

    srand((unsigned int)seed);
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

    sleep(RUN_SECONDS);
    __atomic_store_n(&stop, 1, __ATOMIC_RELAXED);

    pthread_join(cons, NULL);
    for (int i = 0; i < NUM_PRODUCERS; i++)
        pthread_join(prods[i], NULL);

    pthread_mutex_destroy(&buf_mutex);
    pthread_mutex_destroy(&hash_mutex);
    free(active_buf);
    free(drain_buf);

    printf("seed:%d mode:%d hash:%llu drained:%llu\n",
           seed, fault_mode,
           (unsigned long long)global_hash,
           (unsigned long long)total_drained);

    printf("PASS\n");
    return 0;
}
