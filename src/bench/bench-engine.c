/**
 * @file bench-engine.c
 * @brief In-process throughput benchmark for the storage engine (libclaves.so).
 *
 * Calls the claves.h API directly, with no network in between, so the number
 * it reports depends only on the engine compiled into libclaves.so
 * (make ENGINE=hopscotch | ENGINE=list).
 *
 * Workload (typical read-heavy key-value cache):
 *   1. Pre-load N keys.
 *   2. For each thread count T, run T threads for a fixed time doing:
 *        45% get_value on a random pre-loaded key
 *        45% exist     on a random pre-loaded key
 *         5% modify_value on a random pre-loaded key
 *         5% set_value + delete_key on a key private to the thread
 *      The set/delete pair keeps the number of keys constant during the run.
 *   3. Print one CSV line per T: engine,threads,ops_per_sec,errors
 *
 * Every call checks its result (pre-loaded keys must always be found), so the
 * benchmark doubles as a concurrency test. Exit code is 1 if any error occurs.
 */
#include "../../include/claves.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ===================== DEFAULTS AND LIMITS ===================== */

#define DEFAULT_KEYS        10000
#define DEFAULT_SECONDS     3
#define DEFAULT_THREADS     "1,2,4,8"
#define DEFAULT_ENGINE      "unknown"

#define MAX_THREAD_COUNTS   16   /* Max entries in the -t list */
#define MAX_THREADS         128  /* Below HP_MAX_THREADS (256) in hash-table.c */
#define KEY_BUF_LEN         32   /* "key_<n>" / "priv_<n>" always fit */
#define VALUE1_BUF_LEN      256  /* claves.h: value1 needs room for 256 bytes */
#define V2_MAX_LEN          32   /* claves.h: V_value2 holds up to 32 floats */
#define V2_LEN              4    /* Floats written per tuple in this workload */

/* Operation mix, as cumulative percentages over [0, 100). */
#define PCT_GET             45
#define PCT_EXIST           (PCT_GET + 45)
#define PCT_MODIFY          (PCT_EXIST + 5)
/* Remaining 5%: set_value + delete_key */
#define PCT_TOTAL           100

#define NSEC_PER_SEC        1000000000.0

/* ===================== SHARED STATE ===================== */

struct BenchConfig {
    int         num_keys;
    int         seconds;
    int         thread_counts[MAX_THREAD_COUNTS];
    int         num_thread_counts;
    const char *engine;
};

/* Pre-formatted keys, so the hot loop never calls snprintf. */
static char (*keys)[KEY_BUF_LEN] = NULL;
static int  num_keys = 0;

static atomic_bool stop = false;

/* Results of one worker. Padded to a cache line so that counters of different
 * threads never share a line (no false sharing while counting). */
struct WorkerResult {
    uint64_t ops;
    uint64_t errors;
} __attribute__((aligned(64)));

struct WorkerArgs {
    int                  id;
    pthread_barrier_t   *start_barrier;
    struct WorkerResult *result;
};

/* ===================== RANDOM NUMBERS ===================== */

/* xorshift64*: tiny per-thread generator. rand() is not used because it keeps
 * hidden shared state, which would act as a lock and distort the measurement. */
static inline uint64_t next_random(uint64_t *state) {
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ===================== WORKLOAD ===================== */

static void fill_tuple(float *v2, struct Paquete *v3, int seed) {
    for (int i = 0; i < V2_LEN; i++) {
        v2[i] = (float)(seed + i);
    }
    *v3 = (struct Paquete){seed, seed, seed};
}

static int preload_keys(int count) {
    keys = malloc((size_t)count * sizeof(*keys));
    if (keys == NULL) {
        perror("malloc");
        return -1;
    }
    num_keys = count;

    if (destroy() != 0) {
        fprintf(stderr, "Error: destroy() falló\n");
        return -1;
    }

    float v2[V2_LEN];
    struct Paquete v3;
    for (int i = 0; i < count; i++) {
        snprintf(keys[i], KEY_BUF_LEN, "key_%d", i);
        fill_tuple(v2, &v3, i);
        if (set_value(keys[i], "initial", V2_LEN, v2, v3) != 0) {
            fprintf(stderr, "Error: no se pudo precargar %s\n", keys[i]);
            return -1;
        }
    }
    return 0;
}

static void *worker(void *arg) {
    struct WorkerArgs *args = arg;
    uint64_t rng = 0x9E3779B97F4A7C15ULL * (uint64_t)(args->id + 1);
    uint64_t ops = 0, errors = 0;

    char private_key[KEY_BUF_LEN];
    snprintf(private_key, KEY_BUF_LEN, "priv_%d", args->id);

    float v2[V2_LEN];
    struct Paquete v3;
    fill_tuple(v2, &v3, args->id);

    char out_value1[VALUE1_BUF_LEN];
    float out_v2[V2_MAX_LEN];
    int out_n;
    struct Paquete out_v3;

    pthread_barrier_wait(args->start_barrier);

    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        uint64_t r = next_random(&rng);
        int op = (int)(r % PCT_TOTAL);
        char *key = keys[(r / PCT_TOTAL) % (uint64_t)num_keys];

        if (op < PCT_GET) {
            errors += get_value(key, out_value1, &out_n, out_v2, &out_v3) != 0;
            ops++;
        } else if (op < PCT_EXIST) {
            errors += exist(key) != 1;
            ops++;
        } else if (op < PCT_MODIFY) {
            errors += modify_value(key, "modified", V2_LEN, v2, v3) != 0;
            ops++;
        } else {
            errors += set_value(private_key, "private", V2_LEN, v2, v3) != 0;
            errors += delete_key(private_key) != 0;
            ops += 2;
        }
    }

    args->result->ops = ops;
    args->result->errors = errors;
    return NULL;
}

/* ===================== ONE MEASUREMENT ===================== */

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / NSEC_PER_SEC;
}

/* Runs the workload with num_threads threads and prints one CSV line.
 * Returns the number of errors. Exits if a thread cannot be created, since a
 * measurement with fewer threads than requested would be misleading. */
static long run_with_threads(int num_threads, int seconds, const char *engine) {
    pthread_t threads[MAX_THREADS];
    struct WorkerArgs args[MAX_THREADS];
    struct WorkerResult results[MAX_THREADS];
    pthread_barrier_t start_barrier;

    /* +1: the main thread joins the barrier so the clock starts only when
     * every worker is ready (thread creation is not measured). */
    pthread_barrier_init(&start_barrier, NULL, (unsigned)num_threads + 1);
    atomic_store(&stop, false);

    for (int i = 0; i < num_threads; i++) {
        args[i] = (struct WorkerArgs){i, &start_barrier, &results[i]};
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "Error: no se pudo crear el hilo %d\n", i);
            exit(EXIT_FAILURE);
        }
    }

    struct timespec start, end;
    pthread_barrier_wait(&start_barrier);
    clock_gettime(CLOCK_MONOTONIC, &start);

    sleep((unsigned)seconds);
    atomic_store(&stop, true);

    uint64_t total_ops = 0, total_errors = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_ops += results[i].ops;
        total_errors += results[i].errors;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    pthread_barrier_destroy(&start_barrier);

    double ops_per_sec = (double)total_ops / elapsed_seconds(&start, &end);
    printf("%s,%d,%.0f,%llu\n", engine, num_threads, ops_per_sec,
           (unsigned long long)total_errors);
    fflush(stdout);
    return (long)total_errors;
}

/* ===================== COMMAND LINE ===================== */

static void usage(const char *prog) {
    fprintf(stderr,
            "Uso: %s [-k claves] [-d segundos] [-t lista_hilos] [-e motor]\n"
            "  -k  claves precargadas            (por defecto %d)\n"
            "  -d  segundos por medición         (por defecto %d)\n"
            "  -t  número de hilos, separados por comas (por defecto %s, máx %d)\n"
            "  -e  etiqueta del motor en la salida (por defecto %s)\n"
            "Salida CSV: engine,threads,ops_per_sec,errors\n",
            prog, DEFAULT_KEYS, DEFAULT_SECONDS, DEFAULT_THREADS, MAX_THREADS,
            DEFAULT_ENGINE);
}

/* Parses a strictly positive integer in [1, max]. Returns -1 if invalid. */
static int parse_positive(const char *text, int max) {
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > max) {
        return -1;
    }
    return (int)value;
}

static int parse_thread_list(const char *text, struct BenchConfig *config) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s", text);

    config->num_thread_counts = 0;
    char *saveptr;
    for (char *tok = strtok_r(buffer, ",", &saveptr); tok != NULL;
         tok = strtok_r(NULL, ",", &saveptr)) {
        if (config->num_thread_counts == MAX_THREAD_COUNTS) {
            return -1;
        }
        int n = parse_positive(tok, MAX_THREADS);
        if (n < 0) {
            return -1;
        }
        config->thread_counts[config->num_thread_counts++] = n;
    }
    return config->num_thread_counts > 0 ? 0 : -1;
}

static int parse_args(int argc, char **argv, struct BenchConfig *config) {
    config->num_keys = DEFAULT_KEYS;
    config->seconds = DEFAULT_SECONDS;
    config->engine = DEFAULT_ENGINE;
    if (parse_thread_list(DEFAULT_THREADS, config) != 0) {
        return -1;
    }

    int opt;
    while ((opt = getopt(argc, argv, "k:d:t:e:h")) != -1) {
        switch (opt) {
        case 'k':
            if ((config->num_keys = parse_positive(optarg, INT32_MAX)) < 0)
                return -1;
            break;
        case 'd':
            if ((config->seconds = parse_positive(optarg, INT32_MAX)) < 0)
                return -1;
            break;
        case 't':
            if (parse_thread_list(optarg, config) != 0)
                return -1;
            break;
        case 'e':
            config->engine = optarg;
            break;
        default:
            return -1;
        }
    }
    return 0;
}

/* ===================== MAIN ===================== */

int main(int argc, char **argv) {
    struct BenchConfig config;
    if (parse_args(argc, argv, &config) != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (preload_keys(config.num_keys) != 0) {
        return EXIT_FAILURE;
    }

    long total_errors = 0;
    for (int i = 0; i < config.num_thread_counts; i++) {
        total_errors += run_with_threads(config.thread_counts[i],
                                         config.seconds, config.engine);
    }

    free(keys);
    return total_errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
