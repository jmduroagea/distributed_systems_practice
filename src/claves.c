/**
 * @file claves.c
 * @brief Concurrent Hopscotch Hash Table con Hazard Pointers y Lock Striping.
 * * Arquitectura:
 * - Resolución de colisiones: Hopscotch-variant (vecindad de 64 ranuras) para O(1).
 * - Concurrencia de datos: Lock Striping (1024 Seqlocks atómicos segmentados).
 * - Concurrencia de memoria: Hazard Pointers para recolección de basura lock-free.
 * - Rendimiento: Separación Skinny/Fat para maximizar el uso de la caché L1/L2.
 */

#define _GNU_SOURCE

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
 * CONSTANTES ARQUITECTÓNICAS Y LÍMITES DEL SISTEMA
 * ========================================================================= */
#define INITIAL_CAPACITY 4096
#define HOPSCOTCH_NEIGHBORHOOD 64
#define NUM_SEGMENTS 4096
#define STASH_SIZE 1024
#define LOAD_FACTOR_THRESHOLD 0.9f

// Límites definidos por la especificación de la API
#define MAX_STR_LEN 255 
#define MAX_FLOATS 32    

// Constantes del Subsistema de Hazard Pointers
#define HP_MAX_THREADS 128      
#define HP_RETIRE_THRESHOLD 256  

/* ========================================================================= *
 * ESTRUCTURAS DE DATOS
 * ========================================================================= */

// El "Fat Tuple" que contiene los datos pesados
struct Tuple {
    char key[MAX_STR_LEN + 1];    
    char value1[MAX_STR_LEN + 1]; 
    int N_value2;                 
    float V_value2[MAX_FLOATS];   
    struct Paquete value3;        
};

// El "Skinny Entry" alineado para la caché. 
// Nota: El puntero 'data' ahora es atómico para interactuar con los Hazard Pointers.
struct HashEntry {
    uint64_t hop_info;      
    uint32_t hash_cache;    
    _Atomic(struct Tuple *) data; 
};

struct ConcurrentHashTable {
    struct HashEntry *entries;
    uint32_t capacity;
    atomic_int total_items;
    
    alignas(64) atomic_uint segment_locks[NUM_SEGMENTS]; 
    
    struct HashEntry stash[STASH_SIZE];
    atomic_int stash_count;
    pthread_mutex_t stash_lock;
};

// Estructura global para registrar los Hazard Pointers de todos los hilos
struct HPRecord {
    atomic_bool active;
    _Atomic(void*) pointer; 
};

/* ========================================================================= *
 * ESTADO GLOBAL Y VARIABLES LOCALES DE HILO (TLS)
 * ========================================================================= */
struct ConcurrentHashTable *global_table = NULL;
atomic_int servicio_iniciado = ATOMIC_VAR_INIT(0);
pthread_rwlock_t global_stw_rwlock = PTHREAD_RWLOCK_INITIALIZER;

// Estado del subsistema HP
struct HPRecord hp_records[HP_MAX_THREADS];
_Thread_local int my_hp_id = -1; 
_Thread_local void* retire_list[HP_RETIRE_THRESHOLD];
_Thread_local int retire_count = 0;

/* ========================================================================= *
 * FUNCIONES AUXILIARES: HAZARD POINTERS (Gestión de Memoria Lock-Free)
 * ========================================================================= */
static void hp_init_thread() {
    if (my_hp_id != -1) return;
    for (int i = 0; i < HP_MAX_THREADS; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&hp_records[i].active, &expected, true)) {
            my_hp_id = i;
            atomic_store(&hp_records[i].pointer, NULL);
            return;
        }
    }
    fprintf(stderr, "Fatal: Límite de hilos HP (%d) alcanzado.\n", HP_MAX_THREADS);
    exit(1);
}

static void hp_set(void* ptr) {
    if (my_hp_id == -1) hp_init_thread();
    atomic_store_explicit(&hp_records[my_hp_id].pointer, ptr, memory_order_release);
}

static void hp_clear() {
    atomic_store_explicit(&hp_records[my_hp_id].pointer, NULL, memory_order_release);
}

static void hp_scan() {
    int current_retire_count = retire_count;
    retire_count = 0; 

    for (int i = 0; i < current_retire_count; i++) {
        void* ptr = retire_list[i];
        bool safe_to_free = true;

        for (int j = 0; j < HP_MAX_THREADS; j++) {
            if (atomic_load(&hp_records[j].active)) {
                if (atomic_load_explicit(&hp_records[j].pointer, memory_order_acquire) == ptr) {
                    safe_to_free = false;
                    break;
                }
            }
        }

        if (safe_to_free) {
            free(ptr); // Destrucción física segura
        } else {
            retire_list[retire_count++] = ptr; // Volvemos a encolar para el próximo ciclo
        }
    }
}

static void hp_retire(void* ptr) {
    if (my_hp_id == -1) hp_init_thread();
    retire_list[retire_count++] = ptr;
    if (retire_count >= HP_RETIRE_THRESHOLD) {
        hp_scan();
    }
}

/* ========================================================================= *
 * FUNCIONES AUXILIARES: Hashing y Seqlocks (Consistencia de Datos)
 * ========================================================================= */
static inline uint64_t hash_function(const char *key) {
    return XXH3_64bits(key, strlen(key));
}

static inline int get_segment(uint32_t index, uint32_t capacity) {
    return (index / (capacity / NUM_SEGMENTS)) % NUM_SEGMENTS;
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
        if (expected % 2 == 0 && atomic_compare_exchange_weak_explicit(
                lock, &expected, expected + 1, 
                memory_order_acquire, memory_order_relaxed)) {
            break;
        }
    }
}

static inline void write_unlock(atomic_uint *lock) {
    atomic_fetch_add_explicit(lock, 1, memory_order_release);
}

/* ========================================================================= *
 * LÓGICA DE REDIMENSIONAMIENTO PARALELO (STW)
 * ========================================================================= */
struct ResizeArgs {
    struct ConcurrentHashTable *old_table;
    struct ConcurrentHashTable *new_table;
    atomic_int *chunk_counter;
    int total_chunks;
    pthread_barrier_t *barrier;
};

void* resize_worker(void* args) {
    struct ResizeArgs *r = (struct ResizeArgs *)args;
    int chunk_size = r->old_table->capacity / r->total_chunks;
    
    while (true) {
        int my_chunk = atomic_fetch_add(r->chunk_counter, 1);
        if (my_chunk >= r->total_chunks) break;
        
        int start_idx = my_chunk * chunk_size;
        int end_idx = start_idx + chunk_size;
        
        for (int i = start_idx; i < end_idx; i++) {
            struct Tuple *t = atomic_load(&r->old_table->entries[i].data);
            if (t != NULL) {
                uint64_t h = hash_function(t->key);
                uint32_t idx = h % r->new_table->capacity;
                
                while (atomic_load(&r->new_table->entries[idx].data) != NULL) {
                    idx = (idx + 1) % r->new_table->capacity;
                }
                atomic_store(&r->new_table->entries[idx].data, t);
                r->new_table->entries[idx].hash_cache = (uint32_t)h;
            }
        }
    }
    pthread_barrier_wait(r->barrier);
    return NULL;
}

void trigger_parallel_resize() {
    pthread_rwlock_wrlock(&global_stw_rwlock);
    
    float load = (float)atomic_load(&global_table->total_items) / global_table->capacity;
    if (load < LOAD_FACTOR_THRESHOLD && atomic_load(&global_table->stash_count) < STASH_SIZE) {
        pthread_rwlock_unlock(&global_stw_rwlock);
        return;
    }

    struct ConcurrentHashTable *new_table = calloc(1, sizeof(struct ConcurrentHashTable));
    new_table->capacity = global_table->capacity * 2;
    new_table->entries = calloc(new_table->capacity, sizeof(struct HashEntry));
    pthread_mutex_init(&new_table->stash_lock, NULL);
    atomic_store(&new_table->total_items, atomic_load(&global_table->total_items));

    int num_threads = 4;
    pthread_t workers[num_threads];
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, num_threads + 1);
    atomic_int chunk_counter = ATOMIC_VAR_INIT(0);
    
    struct ResizeArgs args = {global_table, new_table, &chunk_counter, 64, &barrier};
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&workers[i], NULL, resize_worker, &args);
    }
    
    for (int i = 0; i < STASH_SIZE; i++) {
        struct Tuple *t = atomic_load(&global_table->stash[i].data);
        if (t != NULL) {
            uint32_t idx = hash_function(t->key) % new_table->capacity;
            while (atomic_load(&new_table->entries[idx].data) != NULL) {
                idx = (idx + 1) % new_table->capacity;
            }
            atomic_store(&new_table->entries[idx].data, t);
        }
    }
    
    pthread_barrier_wait(&barrier);
    pthread_barrier_destroy(&barrier);
    
    struct ConcurrentHashTable *old_table = global_table;
    global_table = new_table;
    
    pthread_rwlock_unlock(&global_stw_rwlock);
    free(old_table->entries);
    free(old_table);
}

void* background_free(void* arg) {
    struct ConcurrentHashTable *old_table = (struct ConcurrentHashTable *)arg;
    for (uint32_t i = 0; i < old_table->capacity; i++) {
        struct Tuple *t = atomic_load(&old_table->entries[i].data);
        if (t != NULL) free(t);
    }
    for (int i = 0; i < STASH_SIZE; i++) {
        struct Tuple *t = atomic_load(&old_table->stash[i].data);
        if (t != NULL) free(t);
    }
    free(old_table->entries);
    free(old_table);
    return NULL;
}

/* ========================================================================= *
 * IMPLEMENTACIÓN DE LA API (CLIENTE)
 * ========================================================================= */

int destroy(void) { 
    struct ConcurrentHashTable *new_table = calloc(1, sizeof(struct ConcurrentHashTable));
    if (!new_table) return -1; 
    
    new_table->capacity = INITIAL_CAPACITY;
    new_table->entries = calloc(new_table->capacity, sizeof(struct HashEntry));
    pthread_mutex_init(&new_table->stash_lock, NULL);
    
    pthread_rwlock_wrlock(&global_stw_rwlock);
    struct ConcurrentHashTable *old_table = global_table;
    global_table = new_table;
    atomic_store(&servicio_iniciado, 1);
    pthread_rwlock_unlock(&global_stw_rwlock);
    
    if (old_table != NULL) {
        pthread_t cleanup_thread;
        pthread_create(&cleanup_thread, NULL, background_free, old_table);
        pthread_detach(cleanup_thread);
    }
    return 0; 


int set_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3) { 
    if (!atomic_load(&servicio_iniciado) || key == NULL || value1 == NULL || V_value2 == NULL) return -1;
    if (N_value2 < 1 || N_value2 > 32) return -1; 
    if (strlen(key) > MAX_STR_LEN || strlen(value1) > MAX_STR_LEN) return -1; 

    struct Tuple *new_tuple = malloc(sizeof(struct Tuple));
    if (!new_tuple) return -1;
    strcpy(new_tuple->key, key);
    strcpy(new_tuple->value1, value1);
    new_tuple->N_value2 = N_value2;
    memcpy(new_tuple->V_value2, V_value2, N_value2 * sizeof(float));
    new_tuple->value3 = value3;

    uint64_t full_hash = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;

    pthread_rwlock_rdlock(&global_stw_rwlock);
    uint32_t ideal_idx = full_hash % global_table->capacity;
    int seg_idx = get_segment(ideal_idx, global_table->capacity);
    
    write_lock(&global_table->segment_locks[seg_idx]);

    struct Tuple *existing = atomic_load(&global_table->entries[ideal_idx].data);
    if (existing != NULL && strcmp(existing->key, key) == 0) {
        write_unlock(&global_table->segment_locks[seg_idx]);
        pthread_rwlock_unlock(&global_stw_rwlock);
        free(new_tuple);
        return -1; // Error al insertar clave que ya existe
    }

    bool inserted = false;
    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
        uint32_t curr_idx = (ideal_idx + i) % global_table->capacity;
        if (atomic_load(&global_table->entries[curr_idx].data) == NULL) {
            atomic_store(&global_table->entries[curr_idx].data, new_tuple);
            global_table->entries[curr_idx].hash_cache = short_hash;
            global_table->entries[ideal_idx].hop_info |= (1ULL << i); 
            inserted = true;
            atomic_fetch_add(&global_table->total_items, 1);
            break;
        }
    }

    write_unlock(&global_table->segment_locks[seg_idx]);

    if (!inserted) {
        pthread_mutex_lock(&global_table->stash_lock);
        int stash_idx = -1;
        for (int i = 0; i < STASH_SIZE; i++) {
            if (atomic_load(&global_table->stash[i].data) == NULL) {
                atomic_store(&global_table->stash[i].data, new_tuple);
                global_table->stash[i].hash_cache = short_hash;
                atomic_fetch_add(&global_table->stash_count, 1);
                stash_idx = i;
                break;
            }
        }
        pthread_mutex_unlock(&global_table->stash_lock);
        
        if (stash_idx == -1) {
            pthread_rwlock_unlock(&global_stw_rwlock);
            free(new_tuple);
            trigger_parallel_resize();
            return set_value(key, value1, N_value2, V_value2, value3);
        }
    }
    
    pthread_rwlock_unlock(&global_stw_rwlock);

    float load = (float)atomic_load(&global_table->total_items) / global_table->capacity;
    if (load > LOAD_FACTOR_THRESHOLD || atomic_load(&global_table->stash_count) >= STASH_SIZE) {
        trigger_parallel_resize();
    }

    return 0; // Inserción exitosa 
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

    do {
        seq = read_begin(&global_table->segment_locks[seg_idx]);
        uint64_t hop_info = global_table->entries[ideal_idx].hop_info;
        
        for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
            if ((hop_info & (1ULL << i)) != 0) {
                uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
                if (global_table->entries[check_idx].hash_cache == short_hash) {
                    
                    // CARGA ATÓMICA Y PROTECCIÓN CON HAZARD POINTERS
                    struct Tuple *t = atomic_load(&global_table->entries[check_idx].data);
                    if (t != NULL) {
                        hp_set((void*)t);
                        
                        // Validamos que no haya sido eliminado mientras levantábamos el escudo
                        if (t != atomic_load(&global_table->entries[check_idx].data)) {
                            hp_clear();
                            continue;
                        }
                        
                        if (strcmp(t->key, key) == 0) {
                            strcpy(value1, t->value1); 
                            *N_value2 = t->N_value2;   
                            memcpy(V_value2, t->V_value2, t->N_value2 * sizeof(float)); 
                            *value3 = t->value3;       
                            found = true;
                            hp_clear();
                            break;
                        }
                        hp_clear(); // Bajamos escudo si fue colisión de hash
                    }
                }
            }
        }
    } while (read_retry(&global_table->segment_locks[seg_idx], seq));

    if (!found) {
        pthread_mutex_lock(&global_table->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = atomic_load(&global_table->stash[i].data);
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
    return found ? 0 : -1; 
}

int modify_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3) { 
    if (!atomic_load(&servicio_iniciado) || key == NULL || value1 == NULL || V_value2 == NULL) return -1;
    if (N_value2 < 1 || N_value2 > 32) return -1; 
    if (strlen(value1) > MAX_STR_LEN) return -1;

    uint64_t full_hash = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;

    pthread_rwlock_rdlock(&global_stw_rwlock);
    
    uint32_t ideal_idx = full_hash % global_table->capacity;
    int seg_idx = get_segment(ideal_idx, global_table->capacity);
    bool modified = false;

    write_lock(&global_table->segment_locks[seg_idx]);
    
    uint64_t hop_info = global_table->entries[ideal_idx].hop_info;
    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
        if ((hop_info & (1ULL << i)) != 0) {
            uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
            if (global_table->entries[check_idx].hash_cache == short_hash) {
                struct Tuple *t = atomic_load(&global_table->entries[check_idx].data);
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
            struct Tuple *t = atomic_load(&global_table->stash[i].data);
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
    return modified ? 0 : -1; 
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
                struct Tuple *t = atomic_load(&global_table->entries[check_idx].data);
                if (t != NULL && strcmp(t->key, key) == 0) {
                    
                    // DESVINCULACIÓN ATÓMICA
                    atomic_store(&global_table->entries[check_idx].data, NULL);
                    global_table->entries[check_idx].hash_cache = 0;
                    global_table->entries[ideal_idx].hop_info &= ~(1ULL << i); 
                    
                    atomic_fetch_sub(&global_table->total_items, 1);
                    
                    // RECLAMACIÓN DE MEMORIA SEGURA
                    hp_retire(t); 
                    
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
            struct Tuple *t = atomic_load(&global_table->stash[i].data);
            if (t != NULL && global_table->stash[i].hash_cache == short_hash && strcmp(t->key, key) == 0) {
                atomic_store(&global_table->stash[i].data, NULL);
                global_table->stash[i].hash_cache = 0;
                atomic_fetch_sub(&global_table->stash_count, 1);
                hp_retire(t);
                deleted = true;
                break;
            }
        }
        pthread_mutex_unlock(&global_table->stash_lock);
    }

    pthread_rwlock_unlock(&global_stw_rwlock);
    return deleted ? 0 : -1; 
}

int exist(char *key) { 
    if (!atomic_load(&servicio_iniciado) || key == NULL) return -1; 

    uint64_t full_hash = hash_function(key);
    uint32_t short_hash = (uint32_t)full_hash;
    
    pthread_rwlock_rdlock(&global_stw_rwlock); 
    
    uint32_t ideal_idx = full_hash % global_table->capacity;
    int seg_idx = get_segment(ideal_idx, global_table->capacity);
    unsigned seq;
    bool found = false;

    do {
        seq = read_begin(&global_table->segment_locks[seg_idx]);
        uint64_t hop_info = global_table->entries[ideal_idx].hop_info;
        
        for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
            if ((hop_info & (1ULL << i)) != 0) {
                uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
                if (global_table->entries[check_idx].hash_cache == short_hash) {
                    
                    struct Tuple *t = atomic_load(&global_table->entries[check_idx].data);
                    if (t != NULL) {
                        hp_set((void*)t);
                        if (t != atomic_load(&global_table->entries[check_idx].data)) {
                            hp_clear();
                            continue;
                        }
                        
                        if (strcmp(t->key, key) == 0) {
                            found = true;
                            hp_clear();
                            break;
                        }
                        hp_clear();
                    }
                }
            }
        }
    } while (read_retry(&global_table->segment_locks[seg_idx], seq));

    if (!found) {
        pthread_mutex_lock(&global_table->stash_lock);
        for (int i = 0; i < STASH_SIZE; i++) {
            struct Tuple *t = atomic_load(&global_table->stash[i].data);
            if (t != NULL && global_table->stash[i].hash_cache == short_hash && strcmp(t->key, key) == 0) {
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&global_table->stash_lock);
    }

    pthread_rwlock_unlock(&global_stw_rwlock);
    return found ? 1 : 0; 
}