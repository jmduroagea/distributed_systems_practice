/**
 * @file claves.c
 * @brief Concurrent Hopscotch Hash Table with Overflow Stash and Parallel Resizing.
 * * Architecture Overview:
 * - Collision Resolution: 64-slot bounded linear probing (Hopscotch-variant) to ensure O(1) searches.
 * - Concurrency: Lock Striping via 1024 atomic Seqlocks. Reads are entirely lock-free.
 * - Cache Optimization: "Skinny" array entries (24 bytes) separated from "Fat" data payloads.
 * - Resizing: Parallel Stop-The-World (STW) migration to avoid latency spikes.
 */

#define _GNU_SOURCE // Required for POSIX pthread_barrier_t on some Linux systems

#include "../include/claves.h"
#include "../xxhash/xxhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdbool.h>

/* ========================================================================= *
 * ARCHITECTURAL CONSTANTS
 * ========================================================================= */

// Starting at 2^20 slots (~25MB RAM) delays the first highly-expensive resize operation.
#define INITIAL_CAPACITY 1048576 

// 64 slots map perfectly to a single uint64_t register for 1-cycle bitwise operations.
#define HOPSCOTCH_NEIGHBORHOOD 64

// 1024 lock segments minimize lock contention (Amdahl's Law) while consuming only ~65KB.
#define NUM_SEGMENTS 1024

// The Stash absorbs statistically unlucky clusters, pushing safe load factors higher.
#define STASH_SIZE 64

// Threshold to trigger a resize before Hopscotch cascading shifts become too expensive.
#define LOAD_FACTOR_THRESHOLD 0.85f

// System constraints defined by the assignment API.
#define MAX_STR_LEN 255
#define MAX_FLOATS 32

/* ========================================================================= *
 * DATA STRUCTURES
 * ========================================================================= */

/**
 * The "Fat" Tuple Payload
 * Allocated exactly once per insertion to eliminate malloc() contention.
 */
struct Tuple {
    char key[MAX_STR_LEN + 1];    // Max 255 chars + null terminator [cite: 8]
    char value1[MAX_STR_LEN + 1]; // Max 255 chars + null terminator [cite: 10]
    int N_value2;                 // Must be between 1 and 32 [cite: 13]
    float V_value2[MAX_FLOATS];   // Array of up to 32 floats [cite: 11]
    struct Paquete value3;        // Client-defined struct [cite: 14]
};

/**
 * The "Skinny" Hash Entry
 * Fits cleanly into a 64-byte CPU cache line to make neighborhood scans lightning-fast.
 */
struct HashEntry {
    uint64_t hop_info;      // Bitmap tracking which of the next 64 slots belong to this hash index
    uint32_t hash_cache;    // 32-bit hash signature to avoid pointer dereferences on cache misses
    struct Tuple *data;     // Pointer to the heavy payload
};

/**
 * The Master Table Structure
 */
struct ConcurrentHashTable {
    struct HashEntry *entries;
    uint32_t capacity;
    atomic_int total_items;
    
    // Segmented Seqlocks. 
    // alignas(64) pads each lock to a full cache line, preventing False Sharing between threads.
    alignas(64) atomic_uint segment_locks[NUM_SEGMENTS]; 
    
    // Overflow Stash for the 0.01% of keys that overflow the 64-slot Hopscotch neighborhood.
    struct HashEntry stash[STASH_SIZE];
    atomic_int stash_count;
    pthread_mutex_t stash_lock;
};

/* ========================================================================= *
 * GLOBAL STATE & LOCKS
 * ========================================================================= */
struct ConcurrentHashTable *global_table = NULL;
atomic_int servicio_iniciado = ATOMIC_VAR_INIT(0);

/**
 * Global Stop-The-World (STW) Lock.
 * Readers = Normal threaded operations (set, get, exist). 
 * Writer = Parallel STW Resize or destroy() pointer swaps.
 * Elevating this to a global level prevents segmentation faults during destroy().
 */
pthread_rwlock_t global_stw_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* ========================================================================= *
 * HELPER FUNCTIONS: Hashing & Concurrency
 * ========================================================================= */

// XXH3 operates at RAM-speed limits, utilizing CPU vector instructions for short strings.
static inline uint64_t hash_function(const char *key) {
    return XXH3_64bits(key, strlen(key));
}

static inline int get_segment(uint32_t index, uint32_t capacity) {
    return (index / (capacity / NUM_SEGMENTS)) % NUM_SEGMENTS;
}

/**
 * Seqlock Primitives: 
 * These use acquire/release memory semantics to prevent the CPU and compiler 
 * from reordering instructions across the lock boundaries.
 * Even sequence = unlocked. Odd sequence = currently locked by a writer.
 */
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
        // Spin until even, then atomically increment to odd
        if (expected % 2 == 0 && atomic_compare_exchange_weak_explicit(
                lock, &expected, expected + 1, 
                memory_order_acquire, memory_order_relaxed)) {
            break;
        }
    }
}

static inline void write_unlock(atomic_uint *lock) {
    // Release the lock by incrementing back to an even number
    atomic_fetch_add_explicit(lock, 1, memory_order_release);
}

/* ========================================================================= *
 * PARALLEL RESIZING LOGIC
 * ========================================================================= */
struct ResizeArgs {
    struct ConcurrentHashTable *old_table;
    struct ConcurrentHashTable *new_table;
    atomic_int *chunk_counter;
    int total_chunks;
    pthread_barrier_t *barrier;
};

// Worker thread for parallel Stop-The-World (STW) migration
void* resize_worker(void* args) {
    struct ResizeArgs *r = (struct ResizeArgs *)args;
    int chunk_size = r->old_table->capacity / r->total_chunks;
    
    while (true) {
        // Atomic work-stealing: grab the next available chunk of the old table
        int my_chunk = atomic_fetch_add(r->chunk_counter, 1);
        if (my_chunk >= r->total_chunks) break;
        
        int start_idx = my_chunk * chunk_size;
        int end_idx = start_idx + chunk_size;
        
        for (int i = start_idx; i < end_idx; i++) {
            if (r->old_table->entries[i].data != NULL) {
                // Since the world is stopped, we can insert into the new table lock-free
                struct Tuple *t = r->old_table->entries[i].data;
                uint64_t h = hash_function(t->key);
                uint32_t idx = h % r->new_table->capacity;
                
                // Linear probe for the new table
                while (r->new_table->entries[idx].data != NULL) {
                    idx = (idx + 1) % r->new_table->capacity;
                }
                r->new_table->entries[idx].data = t;
                r->new_table->entries[idx].hash_cache = (uint32_t)h;
            }
        }
    }
    // Wait for all worker threads to finish their chunks
    pthread_barrier_wait(r->barrier);
    return NULL;
}

void trigger_parallel_resize() {
    // 1. STW Phase: Acquiring the global Write Lock freezes all incoming client operations.
    pthread_rwlock_wrlock(&global_stw_rwlock);
    
    // Double check triggers in case a concurrent thread already completed the resize
    float load = (float)atomic_load(&global_table->total_items) / global_table->capacity;
    if (load < LOAD_FACTOR_THRESHOLD && atomic_load(&global_table->stash_count) < STASH_SIZE) {
        pthread_rwlock_unlock(&global_stw_rwlock);
        return;
    }

    // 2. Allocation Phase: Double the capacity
    struct ConcurrentHashTable *new_table = calloc(1, sizeof(struct ConcurrentHashTable));
    new_table->capacity = global_table->capacity * 2;
    new_table->entries = calloc(new_table->capacity, sizeof(struct HashEntry));
    pthread_mutex_init(&new_table->stash_lock, NULL);
    atomic_store(&new_table->total_items, atomic_load(&global_table->total_items));

    // 3. Parallel Migration Phase
    int num_threads = 4;
    pthread_t workers[num_threads];
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, num_threads + 1);
    atomic_int chunk_counter = ATOMIC_VAR_INIT(0);
    
    struct ResizeArgs args = {global_table, new_table, &chunk_counter, 64, &barrier};
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&workers[i], NULL, resize_worker, &args);
    }
    
    // The main thread handles migrating the overflow stash
    for (int i = 0; i < STASH_SIZE; i++) {
        if (global_table->stash[i].data != NULL) {
            struct Tuple *t = global_table->stash[i].data;
            uint32_t idx = hash_function(t->key) % new_table->capacity;
            while (new_table->entries[idx].data != NULL) idx = (idx + 1) % new_table->capacity;
            new_table->entries[idx].data = t;
        }
    }
    
    // 4. Swap and Cleanup Phase
    pthread_barrier_wait(&barrier);
    pthread_barrier_destroy(&barrier);
    
    struct ConcurrentHashTable *old_table = global_table;
    global_table = new_table;
    
    pthread_rwlock_unlock(&global_stw_rwlock);
    
    // Free the old arrays. Do NOT free the internal Tuples, as they were moved to the new table!
    free(old_table->entries);
    free(old_table);
}

/* ========================================================================= *
 * API IMPLEMENTATION
 * ========================================================================= */

// Background routine for O(1) destroy latency
void* background_free(void* arg) {
    struct ConcurrentHashTable *old_table = (struct ConcurrentHashTable *)arg;
    for (uint32_t i = 0; i < old_table->capacity; i++) {
        if (old_table->entries[i].data != NULL) free(old_table->entries[i].data);
    }
    for (int i = 0; i < STASH_SIZE; i++) {
        if (old_table->stash[i].data != NULL) free(old_table->stash[i].data);
    }
    free(old_table->entries);
    free(old_table);
    return NULL;
}

int destroy(void) { 
    // Allocate the fresh table synchronously
    struct ConcurrentHashTable *new_table = calloc(1, sizeof(struct ConcurrentHashTable));
    if (!new_table) return -1; // [cite: 23]
    
    new_table->capacity = INITIAL_CAPACITY;
    new_table->entries = calloc(new_table->capacity, sizeof(struct HashEntry));
    pthread_mutex_init(&new_table->stash_lock, NULL);
    
    // "Swap and Defer" pattern: Acquire global write lock to safely swap pointers 
    // while ensuring no active readers are using the old table.
    pthread_rwlock_wrlock(&global_stw_rwlock);
    struct ConcurrentHashTable *old_table = global_table;
    global_table = new_table;
    atomic_store(&servicio_iniciado, 1);
    pthread_rwlock_unlock(&global_stw_rwlock);
    
    // Offload the heavy O(N) memory freeing to a detached background thread.
    if (old_table != NULL) {
        pthread_t cleanup_thread;
        pthread_create(&cleanup_thread, NULL, background_free, old_table);
        pthread_detach(cleanup_thread);
    }
    return 0; // Initialization successful [cite: 23]
}

int set_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3) { 
    if (!atomic_load(&servicio_iniciado) || key == NULL || value1 == NULL || V_value2 == NULL) return -1;
    if (N_value2 < 1 || N_value2 > 32) return -1; // Constraint violation [cite: 31]
    if (strlen(key) > MAX_STR_LEN || strlen(value1) > MAX_STR_LEN) return -1; // Exceeds limits [cite: 8, 10]

    // Allocate the "Fat" Tuple once.
    struct Tuple *new_tuple = malloc(sizeof(struct Tuple));
    if (!new_tuple) return -1;
    strcpy(new_tuple->key, key);
    strcpy(new_tuple->value1, value1);
    new_tuple->N_value2 = N_value2;
    memcpy(new_tuple->V_value2, V_value2, N_value2 * sizeof(float));
    new_tuple->value3 = value3;

    uint64_t full_hash = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;

    // Grab a Read Lock to ensure we don't insert while a STW resize is happening
    pthread_rwlock_rdlock(&global_stw_rwlock);
    
    uint32_t ideal_idx = full_hash % global_table->capacity;
    int seg_idx = get_segment(ideal_idx, global_table->capacity);
    
    write_lock(&global_table->segment_locks[seg_idx]);

    // Fast-path duplicate check
    if (global_table->entries[ideal_idx].data != NULL && 
        strcmp(global_table->entries[ideal_idx].data->key, key) == 0) {
        write_unlock(&global_table->segment_locks[seg_idx]);
        pthread_rwlock_unlock(&global_stw_rwlock);
        free(new_tuple);
        return -1; // Inserting an existing key is an error [cite: 31]
    }

    bool inserted = false;
    
    // 1. Primary Attempt: Scan the 64-slot neighborhood for an empty space
    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
        uint32_t curr_idx = (ideal_idx + i) % global_table->capacity;
        if (global_table->entries[curr_idx].data == NULL) {
            global_table->entries[curr_idx].data = new_tuple;
            global_table->entries[curr_idx].hash_cache = short_hash;
            global_table->entries[ideal_idx].hop_info |= (1ULL << i); // Update neighborhood mask
            inserted = true;
            atomic_fetch_add(&global_table->total_items, 1);
            break;
        }
    }

    write_unlock(&global_table->segment_locks[seg_idx]);

    // 2. Secondary Attempt: Fallback to the Stash if the neighborhood is heavily clustered
    if (!inserted) {
        pthread_mutex_lock(&global_table->stash_lock);
        int stash_idx = -1;
        for (int i = 0; i < STASH_SIZE; i++) {
            if (global_table->stash[i].data == NULL) {
                global_table->stash[i].data = new_tuple;
                global_table->stash[i].hash_cache = short_hash;
                atomic_fetch_add(&global_table->stash_count, 1);
                stash_idx = i;
                break;
            }
        }
        pthread_mutex_unlock(&global_table->stash_lock);
        
        // 3. Emergency state: The Stash is full. 
        // Transparent Resize Retry guarantees we never impose arbitrary capacity limits.
        if (stash_idx == -1) {
            pthread_rwlock_unlock(&global_stw_rwlock);
            free(new_tuple);
            trigger_parallel_resize();
            return set_value(key, value1, N_value2, V_value2, value3); // Recursive transparent retry
        }
    }
    
    pthread_rwlock_unlock(&global_stw_rwlock);

    // Validate table health post-insertion
    float load = (float)atomic_load(&global_table->total_items) / global_table->capacity;
    if (load > LOAD_FACTOR_THRESHOLD || atomic_load(&global_table->stash_count) >= STASH_SIZE) {
        trigger_parallel_resize();
    }

    return 0; // Insertion successful [cite: 30]
}

int get_value(char *key, char *value1, int *N_value2, float *V_value2, struct Paquete *value3) { 
    if (!atomic_load(&servicio_iniciado) || key == NULL) return -1;

    uint64_t full_hash = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;
    
    pthread_rwlock_rdlock(&global_stw_rwlock);
    
    uint32_t ideal_idx = full_hash % global_table->capacity;
    int seg_idx = get_segment(ideal_idx, global_table->capacity);
    unsigned seq;
    bool found = false;

    // Lock-Free Optimistic Read via Seqlocks
    do {
        seq = read_begin(&global_table->segment_locks[seg_idx]);
        uint64_t hop_info = global_table->entries[ideal_idx].hop_info;
        
        for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
            // Check only the slots that officially belong to this neighborhood
            if ((hop_info & (1ULL << i)) != 0) {
                uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
                
                // Compare the hash_cache integer before doing an expensive pointer dereference
                if (global_table->entries[check_idx].hash_cache == short_hash) {
                    struct Tuple *t = global_table->entries[check_idx].data;
                    
                    // Final validation via strcmp to rule out hash collisions
                    if (t != NULL && strcmp(t->key, key) == 0) {
                        strcpy(value1, t->value1); // Extract value1 [cite: 34]
                        *N_value2 = t->N_value2;   // Extract N [cite: 35]
                        memcpy(V_value2, t->V_value2, t->N_value2 * sizeof(float)); // Extract vector [cite: 35]
                        *value3 = t->value3;       // Extract struct [cite: 36]
                        found = true;
                        break;
                    }
                }
            }
        }
    // If a writer modified this segment while we were reading, spin and try again
    } while (read_retry(&global_table->segment_locks[seg_idx], seq));

    // Fallback: Check the Overflow Stash
    if (!found) {
        pthread_mutex_lock(&global_table->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = global_table->stash[i].data;
            if (t != NULL && global_table->stash[i].hash_cache == short_hash && strcmp(t->key, key) == 0) {
                strcpy(value1, t->value1);
                *N_value2 = t->N_value2;
                memcpy(V_value2, t->V_value2, t->N_value2 * sizeof(float));
                *value3 = t->value3;
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&global_table->stash_lock);
    }

    pthread_rwlock_unlock(&global_stw_rwlock);
    return found ? 0 : -1; // 0 on success, -1 if key does not exist [cite: 37]
}

int modify_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3) { 
    if (!atomic_load(&servicio_iniciado) || key == NULL || value1 == NULL || V_value2 == NULL) return -1;
    if (N_value2 < 1 || N_value2 > 32) return -1; // [cite: 41]
    if (strlen(value1) > MAX_STR_LEN) return -1;

    uint64_t full_hash = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;

    pthread_rwlock_rdlock(&global_stw_rwlock);
    
    uint32_t ideal_idx = full_hash % global_table->capacity;
    int seg_idx = get_segment(ideal_idx, global_table->capacity);
    bool modified = false;

    // Modification requires exclusive Write access to the segment
    write_lock(&global_table->segment_locks[seg_idx]);
    
    uint64_t hop_info = global_table->entries[ideal_idx].hop_info;
    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
        if ((hop_info & (1ULL << i)) != 0) {
            uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
            if (global_table->entries[check_idx].hash_cache == short_hash) {
                struct Tuple *t = global_table->entries[check_idx].data;
                if (t != NULL && strcmp(t->key, key) == 0) {
                    strcpy(t->value1, value1);
                    t->N_value2 = N_value2;
                    memcpy(t->V_value2, V_value2, N_value2 * sizeof(float));
                    t->value3 = value3;
                    modified = true;
                    break;
                }
            }
        }
    }
    write_unlock(&global_table->segment_locks[seg_idx]);

    if (!modified) {
        pthread_mutex_lock(&global_table->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = global_table->stash[i].data;
            if (t != NULL && global_table->stash[i].hash_cache == short_hash && strcmp(t->key, key) == 0) {
                strcpy(t->value1, value1);
                t->N_value2 = N_value2;
                memcpy(t->V_value2, V_value2, N_value2 * sizeof(float));
                t->value3 = value3;
                modified = true;
                break;
            }
        }
        pthread_mutex_unlock(&global_table->stash_lock);
    }

    pthread_rwlock_unlock(&global_stw_rwlock);
    return modified ? 0 : -1; // 0 on success, -1 if key does not exist [cite: 40]
}

int delete_key(char *key) { 
    if (!atomic_load(&servicio_iniciado) || key == NULL) return -1;

    uint64_t full_hash = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;

    pthread_rwlock_rdlock(&global_stw_rwlock);
    
    uint32_t ideal_idx = full_hash % global_table->capacity;
    int seg_idx = get_segment(ideal_idx, global_table->capacity);
    bool deleted = false;

    write_lock(&global_table->segment_locks[seg_idx]);
    
    uint64_t hop_info = global_table->entries[ideal_idx].hop_info;
    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
        if ((hop_info & (1ULL << i)) != 0) {
            uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
            if (global_table->entries[check_idx].hash_cache == short_hash) {
                struct Tuple *t = global_table->entries[check_idx].data;
                if (t != NULL && strcmp(t->key, key) == 0) {
                    free(t); // Return Fat Tuple memory to OS
                    global_table->entries[check_idx].data = NULL;
                    global_table->entries[check_idx].hash_cache = 0;
                    
                    // Clear the bit from the original hash index's neighborhood mask
                    global_table->entries[ideal_idx].hop_info &= ~(1ULL << i); 
                    
                    atomic_fetch_sub(&global_table->total_items, 1);
                    deleted = true;
                    break;
                }
            }
        }
    }
    write_unlock(&global_table->segment_locks[seg_idx]);

    if (!deleted) {
        pthread_mutex_lock(&global_table->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = global_table->stash[i].data;
            if (t != NULL && global_table->stash[i].hash_cache == short_hash && strcmp(t->key, key) == 0) {
                free(t);
                global_table->stash[i].data = NULL;
                global_table->stash[i].hash_cache = 0;
                atomic_fetch_sub(&global_table->stash_count, 1);
                deleted = true;
                break;
            }
        }
        pthread_mutex_unlock(&global_table->stash_lock);
    }

    pthread_rwlock_unlock(&global_stw_rwlock);
    return deleted ? 0 : -1; // 0 on success, -1 if key does not exist [cite: 43, 44]
}

int exist(char *key) { 
    if (!atomic_load(&servicio_iniciado) || key == NULL) return -1; // Returns -1 on error [cite: 46]

    uint64_t full_hash = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;
    
    pthread_rwlock_rdlock(&global_stw_rwlock); 
    
    uint32_t ideal_idx = full_hash % global_table->capacity;
    int seg_idx = get_segment(ideal_idx, global_table->capacity);
    unsigned seq;
    bool found = false;

    // Lock-Free Optimistic Read via Seqlocks
    do {
        seq = read_begin(&global_table->segment_locks[seg_idx]);
        uint64_t hop_info = global_table->entries[ideal_idx].hop_info;
        
        for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
            if ((hop_info & (1ULL << i)) != 0) {
                uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
                if (global_table->entries[check_idx].hash_cache == short_hash) {
                    struct Tuple *t = global_table->entries[check_idx].data;
                    if (t != NULL && strcmp(t->key, key) == 0) {
                        found = true;
                        break;
                    }
                }
            }
        }
    } while (read_retry(&global_table->segment_locks[seg_idx], seq));

    if (!found) {
        pthread_mutex_lock(&global_table->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = global_table->stash[i].data;
            if (t != NULL && global_table->stash[i].hash_cache == short_hash && strcmp(t->key, key) == 0) {
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&global_table->stash_lock);
    }

    pthread_rwlock_unlock(&global_stw_rwlock);
    return found ? 1 : 0; // Returns 1 if key exists, 0 otherwise [cite: 46]
}