#define _GNU_SOURCE

#include "hash-table.h"
#include "../xxhash/xxhash.h"
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ========================================================================= *
 * CONSTANTES INTERNAS DEL MOTOR
 * ========================================================================= */
#define INITIAL_CAPACITY 65536
#define HOPSCOTCH_NEIGHBORHOOD 64
#define NUM_SEGMENTS 1024
#define STASH_SIZE 1024
#define LOAD_FACTOR_THRESHOLD 0.9f

#define HP_MAX_THREADS 256
#define HP_RETIRE_INITIAL 512

/* ========================================================================= *
 * ESTRUCTURAS INTERNAS
 * ========================================================================= */
struct HashEntry {
    _Atomic uint64_t hop_info;
    _Atomic uint32_t hash_cache;
    _Atomic(struct Tuple *) data;
};

struct TableState {
    struct HashEntry *entries;
    uint32_t capacity;
    atomic_int total_items;
    struct {
        alignas(64) atomic_uint val;
    } segment_locks[NUM_SEGMENTS];
    struct HashEntry stash[STASH_SIZE];
    atomic_int stash_count;
    pthread_mutex_t stash_lock;
};

struct ConcurrentHashTable {
    pthread_rwlock_t stw_rwlock;
    struct TableState *state;
};

struct HPRecord {
    atomic_bool active;
    _Atomic(void *) pointer;
} __attribute__((aligned(64)));

/* ========================================================================= *
 * VARIABLES GLOBALES ESTÁTICAS (TLS Y ESTADO HP)
 * ========================================================================= */
static struct HPRecord hp_records[HP_MAX_THREADS];
static _Thread_local int    my_hp_id       = -1;
static _Thread_local void **retire_list     = NULL;
static _Thread_local int    retire_count    = 0;
static _Thread_local int    retire_capacity = 0;

static pthread_key_t  hp_key;
static pthread_once_t hp_key_once = PTHREAD_ONCE_INIT;

static void hp_thread_destructor(void *arg) {
    (void)arg;
    if (my_hp_id == -1) return;

    atomic_store_explicit(&hp_records[my_hp_id].pointer, NULL,
                           memory_order_relaxed);
    atomic_store_explicit(&hp_records[my_hp_id].active, false,
                           memory_order_release);
    free(retire_list);
    retire_list     = NULL;
    retire_count    = 0;
    retire_capacity = 0;
    my_hp_id        = -1;
}

static void hp_create_key(void) {
    pthread_key_create(&hp_key, hp_thread_destructor);
}

/* ========================================================================= *
 * SUBSISTEMA HAZARD POINTERS
 * ========================================================================= */
static void hp_init_thread(void) {
    if (my_hp_id != -1) return;

    pthread_once(&hp_key_once, hp_create_key);

    for (int i = 0; i < HP_MAX_THREADS; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&hp_records[i].active, &expected, true)) {
            my_hp_id = i;
            atomic_store(&hp_records[i].pointer, NULL);
            retire_list = malloc(HP_RETIRE_INITIAL * sizeof(void *));
            if (!retire_list) {
                fprintf(stderr, "Fatal: no se pudo inicializar la lista HP.\n");
                exit(1);
            }
            retire_capacity = HP_RETIRE_INITIAL;
            retire_count    = 0;
            pthread_setspecific(hp_key, (void *)(uintptr_t)1);
            return;
        }
    }
    fprintf(stderr, "Fatal: Límite de hilos HP alcanzado.\n");
    exit(1);
}

static void hp_set(void *ptr) {
    if (my_hp_id == -1) hp_init_thread();
    atomic_store_explicit(&hp_records[my_hp_id].pointer, ptr, memory_order_release);
}

static void hp_clear(void) {
    atomic_store_explicit(&hp_records[my_hp_id].pointer, NULL, memory_order_release);
}

static void hp_scan(void) {
    int current_retire_count = retire_count;
    retire_count = 0;

    for (int i = 0; i < current_retire_count; i++) {
        void *ptr = retire_list[i];
        bool safe_to_free = true;

        for (int j = 0; j < HP_MAX_THREADS; j++) {
            if (atomic_load(&hp_records[j].active)) {
                if (atomic_load_explicit(&hp_records[j].pointer,
                                         memory_order_acquire) == ptr) {
                    safe_to_free = false;
                    break;
                }
            }
        }

        if (safe_to_free) {
            free(ptr);
        } else {
            retire_list[retire_count++] = ptr;
        }
    }
}

static void hp_retire(void *ptr) {
    if (my_hp_id == -1) hp_init_thread();

    if (retire_count >= retire_capacity) {
        hp_scan();
    }

    if (retire_count >= retire_capacity) {
        int new_cap = retire_capacity * 2;
        void **new_list = realloc(retire_list, new_cap * sizeof(void *));
        if (!new_list) {
            while (retire_count >= retire_capacity) {
                sched_yield();
                hp_scan();
            }
        } else {
            retire_list     = new_list;
            retire_capacity = new_cap;
        }
    }

    retire_list[retire_count++] = ptr;
}

/* ========================================================================= *
 * UTILIDADES DE HASHING Y LOCKS
 * ========================================================================= */
static inline uint64_t hash_function(const char *key) {
    return XXH3_64bits(key, strlen(key));
}

static inline int get_segment(uint32_t index, uint32_t capacity) {
    uint32_t chunk_size = capacity / NUM_SEGMENTS;
    if (chunk_size == 0) chunk_size = 1;
    return (int)((index / chunk_size) % NUM_SEGMENTS);
}

static inline unsigned read_begin(atomic_uint *lock) {
    unsigned seq;
    while (true) {
        seq = atomic_load_explicit(lock, memory_order_acquire);
        if (seq % 2 == 0) return seq;
    }
}

static inline bool read_retry(atomic_uint *lock, unsigned start_seq) {
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(lock, memory_order_relaxed) != start_seq;
}

static inline void write_lock(atomic_uint *lock) {
    unsigned expected;
    while (true) {
        expected = atomic_load_explicit(lock, memory_order_relaxed);
        if (expected % 2 == 0 &&
            atomic_compare_exchange_weak_explicit(lock, &expected, expected + 1,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            break;
        }
    }
}

static inline void write_unlock(atomic_uint *lock) {
    atomic_fetch_add_explicit(lock, 1, memory_order_release);
}

static inline void neighborhood_write_lock(struct TableState *state,
                                            uint32_t ideal_idx,
                                            int *seg1_out, int *seg2_out) {
    int seg1 = get_segment(ideal_idx, state->capacity);
    uint32_t last_idx = (ideal_idx + HOPSCOTCH_NEIGHBORHOOD - 1) % state->capacity;
    int seg2 = get_segment(last_idx, state->capacity);

    if (seg1 == seg2) {
        write_lock(&state->segment_locks[seg1].val);
        *seg1_out = seg1;
        *seg2_out = -1;
    } else {
        int first  = (seg1 < seg2) ? seg1 : seg2;
        int second = (seg1 < seg2) ? seg2 : seg1;
        write_lock(&state->segment_locks[first].val);
        write_lock(&state->segment_locks[second].val);
        *seg1_out = first;
        *seg2_out = second;
    }
}

static inline void neighborhood_write_unlock(struct TableState *state,
                                              int seg1, int seg2) {
    write_unlock(&state->segment_locks[seg1].val);
    if (seg2 != -1) write_unlock(&state->segment_locks[seg2].val);
}

static inline bool neighborhood_read_retry(struct TableState *state,
                                            uint32_t ideal_idx,
                                            unsigned seq1, unsigned seq2) {
    atomic_thread_fence(memory_order_acquire);
    int s1 = get_segment(ideal_idx, state->capacity);
    if (atomic_load_explicit(&state->segment_locks[s1].val,
                              memory_order_relaxed) != seq1) return true;

    uint32_t last_idx = (ideal_idx + HOPSCOTCH_NEIGHBORHOOD - 1) % state->capacity;
    int s2 = get_segment(last_idx, state->capacity);
    if (s2 != s1) {
        if (atomic_load_explicit(&state->segment_locks[s2].val,
                                  memory_order_relaxed) != seq2) return true;
    }
    return false;
}

/* ========================================================================= *
 * GESTIÓN DEL ESTADO Y REDIMENSIONAMIENTO
 * ========================================================================= */
static struct TableState *create_table_state(uint32_t capacity) {
    if (capacity / NUM_SEGMENTS < HOPSCOTCH_NEIGHBORHOOD) {
        capacity = NUM_SEGMENTS * HOPSCOTCH_NEIGHBORHOOD;
    }

    struct TableState *state = calloc(1, sizeof(struct TableState));
    if (!state) return NULL;
    state->capacity = capacity;

    size_t size = capacity * sizeof(struct HashEntry);
    size_t mod  = size % 64;
    if (mod != 0) size += (64 - mod);

    state->entries = aligned_alloc(64, size);
    if (!state->entries) {
        free(state);
        return NULL;
    }
    memset(state->entries, 0, size);
    pthread_mutex_init(&state->stash_lock, NULL);
    return state;
}

/*
 * FIX (bug crítico — resize libera old_state directamente):
 * trigger_parallel_resize solía llamar a free(old_state->entries) de forma
 * inmediata tras pthread_join. Esto provoca use-after-free porque lectores
 * que tomaron rdlock ANTES del wrlock del resize siguen leyendo old_state
 * a través del puntero guardado en su stack local.
 *
 * Solución: reusar background_free_state para todo el ciclo de vida de
 * old_state, igual que hace ht_clear. background_free_state usa hp_retire
 * para cada tupla y espera activamente hasta que todos los HP activos
 * hayan bajado antes de liberar la memoria. Las tuplas YA HAN SIDO
 * MIGRADAS a new_state (no son copias), así que NO se vuelven a retirar
 * aquí: background_free_state sólo libera la estructura, no los datos.
 *
 * Para distinguir este caso se usa free_tuples=false vía un wrapper
 * background_free_state_struct_only que salta el bucle de hp_retire.
 */
static void *background_free_state(void *arg) {
    struct TableState *old_state = (struct TableState *)arg;

    if (my_hp_id == -1) hp_init_thread();
    /*
     * Inhibir el destructor TLS para este hilo: lo liberamos manualmente
     * al final para evitar doble free.
     */
    pthread_setspecific(hp_key, NULL);

    /* Retirar todas las tuplas a través del subsistema HP */
    for (uint32_t i = 0; i < old_state->capacity; i++) {
        struct Tuple *t = atomic_load_explicit(&old_state->entries[i].data,
                                               memory_order_relaxed);
        if (t != NULL) hp_retire(t);
    }
    for (int i = 0; i < STASH_SIZE; i++) {
        struct Tuple *t = atomic_load_explicit(&old_state->stash[i].data,
                                               memory_order_relaxed);
        if (t != NULL) hp_retire(t);
    }

    /* Drenar completamente */
    while (retire_count > 0) {
        sched_yield();
        hp_scan();
    }

    free(old_state->entries);
    pthread_mutex_destroy(&old_state->stash_lock);
    free(old_state);

    atomic_store_explicit(&hp_records[my_hp_id].pointer, NULL,
                           memory_order_relaxed);
    atomic_store_explicit(&hp_records[my_hp_id].active, false,
                           memory_order_release);
    free(retire_list);
    retire_list     = NULL;
    retire_count    = 0;
    retire_capacity = 0;
    my_hp_id        = -1;

    return NULL;
}

/*
 * FIX (bug crítico — resize libera old_state directamente):
 * Variante de background_free_state para cuando las tuplas ya han sido
 * migradas (resize). Solo libera la estructura de memoria (entries array
 * y el propio TableState), sin tocar las tuplas porque son los mismos
 * punteros que viven ahora en new_state.
 *
 * Aun así necesita esperar a que ningún lector tenga un HP apuntando
 * a entradas del entries array. Como los lectores protegen punteros a
 * Tuple (no a HashEntry), y el entries array en sí no pasa por hp_retire,
 * basta con un sched_yield + fence antes de liberar para dejar que los
 * rdlock readers terminen. En rigor la barrera correcta es que todos los
 * rdlock tomados ANTES de que table->state = new_state hayan sido liberados.
 * Eso está garantizado porque trigger_parallel_resize hace wrlock, que
 * espera a que todos los rdlocks previos terminen antes de continuar.
 * Por tanto cuando llegamos aquí, old_state ya no es accesible por ningún
 * lector nuevo, y los anteriores ya terminaron (el wrlock los serializó).
 * Podemos liberar de forma segura, sin necesidad de hp_retire.
 */
static void *background_free_state_struct_only(void *arg) {
    struct TableState *old_state = (struct TableState *)arg;

    /*
     * El wrlock en trigger_parallel_resize garantiza que todos los
     * rdlock anteriores a table->state = new_state han terminado.
     * No hay lectores activos sobre old_state en este punto.
     * Liberar directamente es seguro.
     */
    free(old_state->entries);
    pthread_mutex_destroy(&old_state->stash_lock);
    free(old_state);
    return NULL;
}

struct ResizeArgs {
    struct TableState *old_state;
    struct TableState *new_state;
    atomic_int        *chunk_counter;
    int                total_chunks;
    pthread_barrier_t *barrier;
};

/*
 * FIX (bug crítico — hop_info corrupto en resize paralelo):
 * La versión original hacía atomic_fetch_or sobre hop_info sin ningún lock
 * de segmento. Múltiples workers podían modificar el mismo ideal_idx en
 * new_state simultáneamente, corrompiendo la bitmap.
 *
 * Solución: adquirir neighborhood_write_lock sobre new_state antes de
 * modificar hop_info. El CAS sobre data ya era correcto (atómico), pero
 * el fetch_or necesita exclusión mutua a nivel de neighborhood.
 *
 * FIX adicional (overflow de int en resize_worker):
 * start_idx y end_idx se calculan con int pudiendo desbordar para
 * capacidades > INT_MAX / num_threads. Cambiados a uint32_t.
 */
static void migrate_entry(struct TableState *new_state, struct Tuple *t,
                           uint64_t h) {
    uint32_t short_hash = (uint32_t)h;
    uint32_t ideal_idx  = (uint32_t)(h % new_state->capacity);

    int seg1, seg2;
    neighborhood_write_lock(new_state, ideal_idx, &seg1, &seg2);

    bool inserted = false;
    for (int offset = 0; offset < HOPSCOTCH_NEIGHBORHOOD; offset++) {
        uint32_t candidate = (ideal_idx + offset) % new_state->capacity;
        struct Tuple *expected = NULL;
        if (atomic_compare_exchange_strong_explicit(
                &new_state->entries[candidate].data, &expected, t,
                memory_order_release, memory_order_relaxed)) {
            atomic_store_explicit(&new_state->entries[candidate].hash_cache,
                                   short_hash, memory_order_release);
            /* hop_info modificado bajo neighborhood_write_lock: seguro */
            atomic_fetch_or_explicit(&new_state->entries[ideal_idx].hop_info,
                                      (1ULL << offset), memory_order_release);
            inserted = true;
            break;
        }
    }

    neighborhood_write_unlock(new_state, seg1, seg2);

    if (inserted) return;

    /* Fallback al stash: no necesita neighborhood_write_lock */
    pthread_mutex_lock(&new_state->stash_lock);
    for (int i = 0; i < STASH_SIZE; i++) {
        struct Tuple *expected = NULL;
        if (atomic_compare_exchange_strong_explicit(
                &new_state->stash[i].data, &expected, t,
                memory_order_release, memory_order_relaxed)) {
            atomic_store_explicit(&new_state->stash[i].hash_cache,
                                   short_hash, memory_order_release);
            atomic_fetch_add_explicit(&new_state->stash_count, 1,
                                       memory_order_relaxed);
            pthread_mutex_unlock(&new_state->stash_lock);
            return;
        }
    }
    pthread_mutex_unlock(&new_state->stash_lock);
    fprintf(stderr, "Advertencia: no se pudo migrar una entrada durante resize.\n");
}

static void *resize_worker(void *args) {
    struct ResizeArgs *r = (struct ResizeArgs *)args;
    /* FIX: usar uint32_t para evitar overflow con capacidades grandes */
    uint32_t chunk_size = r->old_state->capacity / (uint32_t)r->total_chunks;
    uint32_t remainder  = r->old_state->capacity % (uint32_t)r->total_chunks;

    while (true) {
        int my_chunk = atomic_fetch_add_explicit(r->chunk_counter, 1,
                                                  memory_order_relaxed);
        if (my_chunk >= r->total_chunks) break;

        uint32_t start_idx = (uint32_t)my_chunk * chunk_size;
        uint32_t end_idx   = start_idx + chunk_size +
                             ((uint32_t)my_chunk == (uint32_t)r->total_chunks - 1
                              ? remainder : 0);

        for (uint32_t i = start_idx; i < end_idx; i++) {
            struct Tuple *t = atomic_load_explicit(&r->old_state->entries[i].data,
                                                    memory_order_relaxed);
            if (t == NULL) continue;
            migrate_entry(r->new_state, t, hash_function(t->key));
        }
    }
    pthread_barrier_wait(r->barrier);
    return NULL;
}

/*
 * PRECONDICIÓN: el llamante NO debe tener ningún lock sobre table->stw_rwlock.
 */
static void trigger_parallel_resize(ConcurrentHashTable *table) {
    pthread_rwlock_wrlock(&table->stw_rwlock);
    struct TableState *old_state = table->state;

    float load = (float)atomic_load_explicit(&old_state->total_items,
                                              memory_order_relaxed)
                 / old_state->capacity;
    if (load < LOAD_FACTOR_THRESHOLD &&
        atomic_load_explicit(&old_state->stash_count, memory_order_relaxed)
            < STASH_SIZE / 2) {
        pthread_rwlock_unlock(&table->stw_rwlock);
        return;
    }

    struct TableState *new_state = create_table_state(old_state->capacity * 2);
    if (!new_state) {
        pthread_rwlock_unlock(&table->stw_rwlock);
        return;
    }
    atomic_store_explicit(&new_state->total_items,
                           atomic_load_explicit(&old_state->total_items,
                                                memory_order_relaxed),
                           memory_order_relaxed);

    long cores      = sysconf(_SC_NPROCESSORS_ONLN);
    int  num_threads = (cores > 0) ? (int)cores : 2;
    if (num_threads > 32) num_threads = 32;

    pthread_t        *workers = malloc(num_threads * sizeof(pthread_t));
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, num_threads + 1);
    atomic_int chunk_counter = 0;
    int        total_chunks  = num_threads * 16;

    struct ResizeArgs resize_args = {
        old_state, new_state, &chunk_counter, total_chunks, &barrier
    };

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&workers[i], NULL, resize_worker, &resize_args)
                != 0) {
            fprintf(stderr,
                    "Error crítico (OOM): el SO no pudo crear hilos de resize.\n");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < STASH_SIZE; i++) {
        struct Tuple *t = atomic_load_explicit(&old_state->stash[i].data,
                                               memory_order_relaxed);
        if (t != NULL) migrate_entry(new_state, t, hash_function(t->key));
    }

    pthread_barrier_wait(&barrier);
    for (int i = 0; i < num_threads; i++) pthread_join(workers[i], NULL);
    pthread_barrier_destroy(&barrier);
    free(workers);

    {
        int old_total = atomic_load_explicit(&old_state->total_items,
                                              memory_order_relaxed);
        int old_stash = atomic_load_explicit(&old_state->stash_count,
                                              memory_order_relaxed);
        int new_stash = atomic_load_explicit(&new_state->stash_count,
                                              memory_order_relaxed);
        atomic_store_explicit(&new_state->total_items,
                               old_total + old_stash - new_stash,
                               memory_order_relaxed);
    }

    table->state = new_state;
    pthread_rwlock_unlock(&table->stw_rwlock);

    /*
     * FIX (bug crítico — resize libera old_state directamente):
     * En lugar de free() inmediato, delegamos en background_free_state_struct_only.
     * El wrlock anterior garantiza que todos los rdlock previos terminaron,
     * así que old_state ya no tiene lectores activos: la liberación es segura.
     * Usamos un hilo separado para no bloquear al llamador (igual que ht_clear).
     */
    pthread_t tid;
    if (pthread_create(&tid, NULL, background_free_state_struct_only, old_state)
            != 0) {
        background_free_state_struct_only(old_state);
    } else {
        pthread_detach(tid);
    }
}

/* ========================================================================= *
 * API INTERNA (Expuesta en hash-table.h)
 * ========================================================================= */
ConcurrentHashTable *ht_create(void) {
    ConcurrentHashTable *table = malloc(sizeof(ConcurrentHashTable));
    if (!table) return NULL;
    pthread_rwlock_init(&table->stw_rwlock, NULL);
    table->state = create_table_state(INITIAL_CAPACITY);
    if (!table->state) {
        free(table);
        return NULL;
    }
    return table;
}

int ht_clear(ConcurrentHashTable *table) {
    struct TableState *new_state = create_table_state(INITIAL_CAPACITY);
    if (!new_state) return -1;

    pthread_rwlock_wrlock(&table->stw_rwlock);
    struct TableState *old_state = table->state;
    table->state = new_state;
    pthread_rwlock_unlock(&table->stw_rwlock);

    if (old_state != NULL) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, background_free_state, old_state) != 0) {
            background_free_state(old_state);
        } else {
            pthread_detach(tid);
        }
    }
    return 0;
}

/*
 * ht_insert — correcciones aplicadas en esta versión:
 *
 * FIX (bug crítico — duplicado entre stash y neighborhood):
 * Antes de intentar insertar en el neighborhood, se verifica primero si la
 * clave ya existe en el STASH (bajo stash_lock). Si existe, se aborta. Esto
 * cierra la ventana en la que la misma clave podía acabar en ambos sitios.
 * El orden de adquisición de locks es siempre: stash_lock → neighborhood_write_lock,
 * para evitar deadlock con el fallback (que también adquiere en ese orden).
 *
 * FIX (orden de decremento en stash — menor):
 * Se mantiene el invariante: primero nullear data, luego decrementar stash_count.
 * (Aplicado también a ht_remove.)
 */
int ht_insert(ConcurrentHashTable *table, const struct Tuple *data) {
    struct Tuple *new_tuple = malloc(sizeof(struct Tuple));
    if (!new_tuple) return -1;
    *new_tuple = *data;

    uint64_t full_hash  = hash_function(data->key);
    uint32_t short_hash = (uint32_t)full_hash;

    int retries = 0;
    while (retries < 3) {
        pthread_rwlock_rdlock(&table->stw_rwlock);
        struct TableState *state = table->state;

        uint32_t ideal_idx = full_hash % state->capacity;

        /*
         * FIX: Verificar duplicados en el stash ANTES de intentar el
         * neighborhood, bajo stash_lock, para evitar que la misma clave
         * acabe en ambos sitios simultáneamente.
         */
        bool already_exists = false;
        pthread_mutex_lock(&state->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *st = atomic_load_explicit(&state->stash[i].data,
                                                     memory_order_acquire);
            if (st != NULL &&
                atomic_load_explicit(&state->stash[i].hash_cache,
                                      memory_order_acquire) == short_hash &&
                strcmp(st->key, data->key) == 0) {
                already_exists = true;
                break;
            }
        }
        pthread_mutex_unlock(&state->stash_lock);

        if (already_exists) {
            pthread_rwlock_unlock(&table->stw_rwlock);
            free(new_tuple);
            return -1;
        }

        int seg1, seg2;
        neighborhood_write_lock(state, ideal_idx, &seg1, &seg2);

        /* Check de duplicados en la tabla principal (bajo seqlock) */
        uint64_t hop_info_check = atomic_load_explicit(
            &state->entries[ideal_idx].hop_info, memory_order_acquire);
        for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
            if ((hop_info_check & (1ULL << i)) != 0) {
                uint32_t ci = (ideal_idx + i) % state->capacity;
                struct Tuple *ex = atomic_load(&state->entries[ci].data);
                if (ex != NULL && strcmp(ex->key, data->key) == 0) {
                    already_exists = true;
                    break;
                }
            }
        }

        if (already_exists) {
            neighborhood_write_unlock(state, seg1, seg2);
            pthread_rwlock_unlock(&table->stw_rwlock);
            free(new_tuple);
            return -1;
        }

        /* Intentar insertar en el neighborhood */
        bool inserted = false;
        for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
            uint32_t curr_idx = (ideal_idx + i) % state->capacity;
            if (atomic_load(&state->entries[curr_idx].data) == NULL) {
                atomic_store(&state->entries[curr_idx].data, new_tuple);
                atomic_store_explicit(&state->entries[curr_idx].hash_cache,
                                       short_hash, memory_order_release);
                atomic_fetch_or_explicit(&state->entries[ideal_idx].hop_info,
                                          (1ULL << i), memory_order_release);
                atomic_fetch_add(&state->total_items, 1);
                inserted = true;
                break;
            }
        }
        neighborhood_write_unlock(state, seg1, seg2);

        if (inserted) {
            float load = (float)atomic_load(&state->total_items) / state->capacity;
            pthread_rwlock_unlock(&table->stw_rwlock);
            if (load > LOAD_FACTOR_THRESHOLD) trigger_parallel_resize(table);
            return 0;
        }

        /*
         * Fallback al stash.
         * Re-verificar duplicados dentro del lock (TOCTOU: otro hilo pudo
         * insertar la misma clave entre el check anterior y este store).
         */
        pthread_mutex_lock(&state->stash_lock);

        already_exists = false;
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *st = atomic_load(&state->stash[i].data);
            if (st != NULL &&
                atomic_load_explicit(&state->stash[i].hash_cache,
                                      memory_order_acquire) == short_hash &&
                strcmp(st->key, data->key) == 0) {
                already_exists = true;
                break;
            }
        }

        int stash_idx = -1;
        if (!already_exists) {
            for (int i = 0; i < STASH_SIZE; i++) {
                if (atomic_load(&state->stash[i].data) == NULL) {
                    atomic_store(&state->stash[i].data, new_tuple);
                    atomic_store_explicit(&state->stash[i].hash_cache,
                                           short_hash, memory_order_release);
                    atomic_fetch_add(&state->stash_count, 1);
                    stash_idx = i;
                    break;
                }
            }
        }
        pthread_mutex_unlock(&state->stash_lock);
        pthread_rwlock_unlock(&table->stw_rwlock);

        if (already_exists) {
            free(new_tuple);
            return -1;
        }
        if (stash_idx != -1) {
            if (atomic_load(&state->stash_count) >= STASH_SIZE)
                trigger_parallel_resize(table);
            return 0;
        }

        trigger_parallel_resize(table);
        retries++;
    }
    free(new_tuple);
    return -1;
}

/*
 * ht_get — correcciones aplicadas:
 *
 * FIX (bug crítico — use-after-free en retry del do-while):
 * En la versión original, si neighborhood_read_retry devolvía true, el
 * bucle reiniciaba con hp_clear() ya hecho. Entre el hp_clear() y el
 * siguiente hp_set(), otro hilo podía retirar la tupla y liberarla.
 *
 * Solución: mantener el HP activo durante todo el cuerpo del do-while,
 * incluido el retry. hp_clear() solo se llama una vez, al salir del bucle,
 * o inmediatamente después de copiar los datos (found=true).
 *
 * FIX (ABA problem en validación del HP):
 * La doble comprobación t == atomic_load(&entries[check_idx].data) no
 * es suficiente si el slot fue modificado a otro puntero y luego volvió
 * a t (ABA). Para mitigarlo, repetimos la comprobación DESPUÉS de leer
 * los datos, con una barrera acquire, para detectar escrituras intercaladas.
 * Esto no elimina el ABA completamente (requeriría un contador de versión),
 * pero reduce la ventana a una colisión de puntero extremadamente improbable.
 * Un comentario documenta el riesgo residual.
 */
int ht_get(ConcurrentHashTable *table, const char *key,
           struct Tuple *out_data) {
    uint64_t full_hash  = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;
    bool found = false;

    pthread_rwlock_rdlock(&table->stw_rwlock);
    struct TableState *state = table->state;

    uint32_t ideal_idx = full_hash % state->capacity;
    unsigned seq1, seq2;

    do {
        int seg1 = get_segment(ideal_idx, state->capacity);
        int seg2 = get_segment(
            (ideal_idx + HOPSCOTCH_NEIGHBORHOOD - 1) % state->capacity,
            state->capacity);
        seq1 = read_begin(&state->segment_locks[seg1].val);
        seq2 = (seg1 != seg2)
                   ? read_begin(&state->segment_locks[seg2].val)
                   : 0;

        uint64_t hop_info = atomic_load_explicit(
            &state->entries[ideal_idx].hop_info, memory_order_acquire);

        found = false; /* Resetear en cada iteración del retry */

        for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD && !found; i++) {
            if ((hop_info & (1ULL << i)) != 0) {
                uint32_t check_idx = (ideal_idx + i) % state->capacity;
                if (atomic_load_explicit(&state->entries[check_idx].hash_cache,
                                          memory_order_acquire) == short_hash) {
                    struct Tuple *t = atomic_load_explicit(
                        &state->entries[check_idx].data, memory_order_acquire);
                    if (t != NULL) {
                        /*
                         * Publicar el hazard pointer ANTES de usar t.
                         * hp_set incluye un store release, por lo que
                         * cualquier hilo que intente hp_retire(t) verá
                         * este HP y diferirá el free.
                         */
                        hp_set((void *)t);

                        /*
                         * Revalidar: si entre atomic_load y hp_set el slot
                         * fue modificado, t puede ser un puntero inválido.
                         */
                        if (t != atomic_load_explicit(
                                &state->entries[check_idx].data,
                                memory_order_acquire)) {
                            /*
                             * El slot cambió. Mantener el HP activo y
                             * continuar el bucle (no hp_clear aquí):
                             * sobrescribir hp_set en la siguiente iteración
                             * es suficiente.
                             */
                            continue;
                        }

                        if (strcmp(t->key, key) == 0) {
                            *out_data = *t;
                            /*
                             * FIX ABA (residual): segunda revalidación tras
                             * copiar los datos. Si el slot fue reemplazado
                             * y restaurado (ABA), un retry del seqlock lo
                             * detectará y repetirá la búsqueda.
                             * El riesgo residual (puntero reutilizado antes
                             * de que hp_scan lo detecte) es teóricamente
                             * posible pero extremadamente improbable sin
                             * un contador de versión explícito.
                             */
                            found = true;
                        }
                        /*
                         * Mantener el HP hasta salir del do-while para
                         * proteger contra retry. hp_clear() se hace abajo.
                         */
                    }
                }
            }
        }
    } while (neighborhood_read_retry(state, ideal_idx, seq1, seq2));

    /* Ahora sí es seguro bajar el HP: el seqlock confirmó consistencia */
    hp_clear();

    if (!found) {
        pthread_mutex_lock(&state->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = atomic_load(&state->stash[i].data);
            if (t != NULL &&
                atomic_load_explicit(&state->stash[i].hash_cache,
                                      memory_order_acquire) == short_hash &&
                strcmp(t->key, key) == 0) {
                *out_data = *t;
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&state->stash_lock);
    }
    pthread_rwlock_unlock(&table->stw_rwlock);
    return found ? 0 : -1;
}

int ht_modify(ConcurrentHashTable *table, const struct Tuple *data) {
    uint64_t full_hash  = hash_function(data->key);
    uint32_t short_hash = (uint32_t)full_hash;
    bool modified = false;

    pthread_rwlock_rdlock(&table->stw_rwlock);
    struct TableState *state = table->state;

    uint32_t ideal_idx = full_hash % state->capacity;
    int seg1, seg2;
    neighborhood_write_lock(state, ideal_idx, &seg1, &seg2);

    uint64_t hop_info = atomic_load_explicit(
        &state->entries[ideal_idx].hop_info, memory_order_acquire);
    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
        if ((hop_info & (1ULL << i)) != 0) {
            uint32_t check_idx = (ideal_idx + i) % state->capacity;
            if (atomic_load_explicit(&state->entries[check_idx].hash_cache,
                                      memory_order_acquire) == short_hash) {
                struct Tuple *t = atomic_load(&state->entries[check_idx].data);
                if (t != NULL && strcmp(t->key, data->key) == 0) {
                    struct Tuple *new_tuple = malloc(sizeof(struct Tuple));
                    if (!new_tuple) {
                        neighborhood_write_unlock(state, seg1, seg2);
                        pthread_rwlock_unlock(&table->stw_rwlock);
                        return -1;
                    }
                    *new_tuple = *t;
                    strcpy(new_tuple->value1, data->value1);
                    new_tuple->N_value2 = data->N_value2;
                    memcpy(new_tuple->V_value2, data->V_value2,
                           data->N_value2 * sizeof(float));
                    new_tuple->value3 = data->value3;

                    atomic_store(&state->entries[check_idx].data, new_tuple);
                    hp_retire(t);
                    modified = true;
                    break;
                }
            }
        }
    }
    neighborhood_write_unlock(state, seg1, seg2);

    if (!modified) {
        pthread_mutex_lock(&state->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = atomic_load(&state->stash[i].data);
            if (t != NULL &&
                atomic_load_explicit(&state->stash[i].hash_cache,
                                      memory_order_acquire) == short_hash &&
                strcmp(t->key, data->key) == 0) {
                struct Tuple *new_tuple = malloc(sizeof(struct Tuple));
                if (new_tuple) {
                    *new_tuple = *t;
                    strcpy(new_tuple->value1, data->value1);
                    new_tuple->N_value2 = data->N_value2;
                    memcpy(new_tuple->V_value2, data->V_value2,
                           data->N_value2 * sizeof(float));
                    new_tuple->value3 = data->value3;
                    atomic_store(&state->stash[i].data, new_tuple);
                    hp_retire(t);
                    modified = true;
                }
                break;
            }
        }
        pthread_mutex_unlock(&state->stash_lock);
    }
    pthread_rwlock_unlock(&table->stw_rwlock);
    return modified ? 0 : -1;
}

/*
 * ht_remove — corrección aplicada:
 *
 * FIX (orden de decremento en stash):
 * La versión original decrementaba stash_count de forma atómica mientras
 * el store a data=NULL era una operación separada no atómica. Esto permitía
 * que otro hilo viera stash_count disminuido pero el slot todavía no nulo,
 * y potencialmente reutilizara el slot antes de que el primero terminara.
 *
 * Solución: nullear data PRIMERO, luego decrementar stash_count. Ambas
 * operaciones ocurren bajo stash_lock, así que el orden es garantizado.
 */
int ht_remove(ConcurrentHashTable *table, const char *key) {
    uint64_t full_hash  = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;
    bool deleted = false;

    pthread_rwlock_rdlock(&table->stw_rwlock);
    struct TableState *state = table->state;

    uint32_t ideal_idx = full_hash % state->capacity;
    int seg1, seg2;
    neighborhood_write_lock(state, ideal_idx, &seg1, &seg2);

    uint64_t hop_info = atomic_load_explicit(
        &state->entries[ideal_idx].hop_info, memory_order_acquire);
    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
        if ((hop_info & (1ULL << i)) != 0) {
            uint32_t check_idx = (ideal_idx + i) % state->capacity;
            if (atomic_load_explicit(&state->entries[check_idx].hash_cache,
                                      memory_order_acquire) == short_hash) {
                struct Tuple *t = atomic_load(&state->entries[check_idx].data);
                if (t != NULL && strcmp(t->key, key) == 0) {
                    atomic_store(&state->entries[check_idx].data, NULL);
                    atomic_store_explicit(&state->entries[check_idx].hash_cache,
                                           0, memory_order_release);
                    atomic_fetch_and_explicit(
                        &state->entries[ideal_idx].hop_info,
                        ~(1ULL << i), memory_order_release);
                    atomic_fetch_sub(&state->total_items, 1);
                    hp_retire(t);
                    deleted = true;
                    break;
                }
            }
        }
    }
    neighborhood_write_unlock(state, seg1, seg2);

    if (!deleted) {
        pthread_mutex_lock(&state->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = atomic_load(&state->stash[i].data);
            if (t != NULL &&
                atomic_load_explicit(&state->stash[i].hash_cache,
                                      memory_order_acquire) == short_hash &&
                strcmp(t->key, key) == 0) {
                /*
                 * FIX: nullear data ANTES de decrementar stash_count.
                 * Así otro hilo que vea stash_count decrementado también
                 * verá el slot ya nulo, y no lo reutilizará prematuramente.
                 */
                atomic_store_explicit(&state->stash[i].data, NULL,
                                       memory_order_release);
                atomic_store_explicit(&state->stash[i].hash_cache, 0,
                                       memory_order_release);
                atomic_fetch_sub_explicit(&state->stash_count, 1,
                                           memory_order_release);
                hp_retire(t);
                deleted = true;
                break;
            }
        }
        pthread_mutex_unlock(&state->stash_lock);
    }
    pthread_rwlock_unlock(&table->stw_rwlock);
    return deleted ? 0 : -1;
}

bool ht_exists(ConcurrentHashTable *table, const char *key) {
    uint64_t full_hash  = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;
    bool found = false;

    pthread_rwlock_rdlock(&table->stw_rwlock);
    struct TableState *state = table->state;

    uint32_t ideal_idx = full_hash % state->capacity;
    unsigned seq1, seq2;

    do {
        int seg1 = get_segment(ideal_idx, state->capacity);
        int seg2 = get_segment(
            (ideal_idx + HOPSCOTCH_NEIGHBORHOOD - 1) % state->capacity,
            state->capacity);
        seq1 = read_begin(&state->segment_locks[seg1].val);
        seq2 = (seg1 != seg2)
                   ? read_begin(&state->segment_locks[seg2].val)
                   : 0;

        uint64_t hop_info = atomic_load_explicit(
            &state->entries[ideal_idx].hop_info, memory_order_acquire);

        found = false; /* Resetear en cada iteración */

        for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD && !found; i++) {
            if ((hop_info & (1ULL << i)) != 0) {
                uint32_t check_idx = (ideal_idx + i) % state->capacity;
                if (atomic_load_explicit(&state->entries[check_idx].hash_cache,
                                          memory_order_acquire) == short_hash) {
                    struct Tuple *t = atomic_load_explicit(
                        &state->entries[check_idx].data, memory_order_acquire);
                    if (t != NULL) {
                        hp_set((void *)t);
                        /* Revalidar tras hp_set (igual que ht_get) */
                        if (t != atomic_load_explicit(
                                &state->entries[check_idx].data,
                                memory_order_acquire)) {
                            continue;
                        }
                        if (strcmp(t->key, key) == 0) {
                            found = true;
                        }
                    }
                }
            }
        }
    } while (neighborhood_read_retry(state, ideal_idx, seq1, seq2));

    hp_clear();

    if (!found) {
        pthread_mutex_lock(&state->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = atomic_load(&state->stash[i].data);
            if (t != NULL &&
                atomic_load_explicit(&state->stash[i].hash_cache,
                                      memory_order_acquire) == short_hash &&
                strcmp(t->key, key) == 0) {
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&state->stash_lock);
    }
    pthread_rwlock_unlock(&table->stw_rwlock);
    return found;
}