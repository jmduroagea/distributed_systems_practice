/**
 * @file claves.c
 * @brief Concurrent Hopscotch Hash Table con Hazard Pointers y Lock Striping.
 * Arquitectura:
 * - Resolución de colisiones: Hopscotch-variant (vecindad de 64 ranuras) para
 *   O(1).
 * - Concurrencia de datos: Lock Striping (1024 Seqlocks atómicos segmentados).
 * - Concurrencia de memoria: Hazard Pointers para recolección de basura lock-free.
 * - Rendimiento: Separación Skinny/Fat para maximizar el uso de la caché L1/L2.
 */

#define _GNU_SOURCE

#include "../include/claves.h"
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
 * CONSTANTES ARQUITECTÓNICAS Y LÍMITES DEL SISTEMA
 * ========================================================================= */
#define INITIAL_CAPACITY 65536
#define HOPSCOTCH_NEIGHBORHOOD 64
#define NUM_SEGMENTS 1024
#define STASH_SIZE 256
#define LOAD_FACTOR_THRESHOLD 0.9f

// Límites definidos por la especificación de la API
#define MAX_STR_LEN 255
#define MAX_FLOATS 32

// Constantes del Subsistema de Hazard Pointers
#define HP_MAX_THREADS 256
#define HP_RETIRE_THRESHOLD 512

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
struct HashEntry {
  uint64_t hop_info;
  uint32_t hash_cache;
  _Atomic(struct Tuple *) data;
};

struct ConcurrentHashTable {
  struct HashEntry *entries;
  uint32_t capacity;
  atomic_int total_items;

  // CORRECCIÓN FALSE SHARING: Alineamos cada lock individualmente a 64 bytes
  // para que ocupen su propia línea de caché L1 exclusiva.
  struct {
    alignas(64) atomic_uint val;
  } segment_locks[NUM_SEGMENTS];

  struct HashEntry stash[STASH_SIZE];
  atomic_int stash_count;
  pthread_mutex_t stash_lock;
};

// Estructura global para registrar los Hazard Pointers de todos los hilos
struct HPRecord {
  atomic_bool active;
  _Atomic(void *) pointer;
} __attribute__((aligned(64)));

/* ========================================================================= *
 * ESTADO GLOBAL Y VARIABLES LOCALES DE HILO (TLS)
 * ========================================================================= */
struct ConcurrentHashTable *global_table = NULL;
atomic_int servicio_iniciado = ATOMIC_VAR_INIT(0);
pthread_rwlock_t global_stw_rwlock = PTHREAD_RWLOCK_INITIALIZER;

// Estado del subsistema HP
struct HPRecord hp_records[HP_MAX_THREADS];
_Thread_local int my_hp_id = -1;
_Thread_local void *retire_list[HP_RETIRE_THRESHOLD];
_Thread_local int retire_count = 0;

/* ========================================================================= *
 * FUNCIONES AUXILIARES: HAZARD POINTERS (Gestión de Memoria Lock-Free)
 * ========================================================================= */
static void hp_init_thread() {
  if (my_hp_id != -1)
    return;
  for (int i = 0; i < HP_MAX_THREADS; i++) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&hp_records[i].active, &expected,
                                       true)) {
      my_hp_id = i;
      atomic_store(&hp_records[i].pointer, NULL);
      return;
    }
  }
  fprintf(stderr, "Fatal: Límite de hilos HP (%d) alcanzado.\n",
          HP_MAX_THREADS);
  exit(1);
}

static void hp_set(void *ptr) {
  if (my_hp_id == -1)
    hp_init_thread();
  atomic_store_explicit(&hp_records[my_hp_id].pointer, ptr,
                        memory_order_release);
}

static void hp_clear() {
  atomic_store_explicit(&hp_records[my_hp_id].pointer, NULL,
                        memory_order_release);
}

static void hp_scan() {
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
      if (retire_count < HP_RETIRE_THRESHOLD) {
        retire_list[retire_count++] = ptr;
      } else {
        fprintf(stderr,
                "[hp_scan] WARN: retire_list llena, puntero %p no liberado "
                "(memory leak).\n",
                ptr);
      }
    }
  }
}

static void hp_retire(void *ptr) {
  if (my_hp_id == -1)
    hp_init_thread();
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

// CORRECCIÓN BUG MATEMÁTICO: Block Striping en lugar de Interleaved
static inline int get_segment(uint32_t index, uint32_t capacity) {
  uint32_t chunk_size = capacity / NUM_SEGMENTS;
  return (int)((index / chunk_size) % NUM_SEGMENTS);
}

static inline unsigned read_begin(atomic_uint *lock) {
  unsigned seq;
  while (true) {
    seq = atomic_load_explicit(lock, memory_order_acquire);
    if (seq % 2 == 0)
      return seq;
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

/* -------------------------------------------------------------------------
 * neighborhood_write_lock / neighborhood_write_unlock
 * ------------------------------------------------------------------------- */
static inline void neighborhood_write_lock(uint32_t ideal_idx,
                                           uint32_t capacity, int *seg1_out,
                                           int *seg2_out) {
  // Pasamos capacity a get_segment
  int seg1 = get_segment(ideal_idx, capacity);
  uint32_t last_idx = (ideal_idx + HOPSCOTCH_NEIGHBORHOOD - 1) % capacity;
  int seg2 = get_segment(last_idx, capacity);

  if (seg1 == seg2) {
    /* Caso común: toda la vecindad cae en un único segmento */
    write_lock(&global_table->segment_locks[seg1].val);
    *seg1_out = seg1;
    *seg2_out = -1;
  } else {
    /* Caso de cruce: adquirimos en orden ascendente para evitar deadlock */
    int first = (seg1 < seg2) ? seg1 : seg2;
    int second = (seg1 < seg2) ? seg2 : seg1;
    write_lock(&global_table->segment_locks[first].val);
    write_lock(&global_table->segment_locks[second].val);
    *seg1_out = first;
    *seg2_out = second;
  }
}

static inline void neighborhood_write_unlock(int seg1, int seg2) {
  write_unlock(&global_table->segment_locks[seg1].val);
  if (seg2 != -1)
    write_unlock(&global_table->segment_locks[seg2].val);
}

static inline bool neighborhood_read_retry(uint32_t ideal_idx,
                                           uint32_t capacity, unsigned seq1,
                                           unsigned seq2) {
  atomic_thread_fence(memory_order_acquire);
  int s1 = get_segment(ideal_idx, capacity);
  if (atomic_load_explicit(&global_table->segment_locks[s1].val,
                           memory_order_relaxed) != seq1)
    return true;

  uint32_t last_idx = (ideal_idx + HOPSCOTCH_NEIGHBORHOOD - 1) % capacity;
  int s2 = get_segment(last_idx, capacity);
  if (s2 != s1) {
    if (atomic_load_explicit(&global_table->segment_locks[s2].val,
                             memory_order_relaxed) != seq2)
      return true;
  }
  return false;
}

/* -------------------------------------------------------------------------
 * Lógica de Redimensionamiento (Resize Workers)
 * ------------------------------------------------------------------------- */
struct ResizeArgs {
  struct ConcurrentHashTable *old_table;
  struct ConcurrentHashTable *new_table;
  atomic_int *chunk_counter;
  int total_chunks;
  pthread_barrier_t *barrier;
};

static void migrate_entry_to_new_table(struct ConcurrentHashTable *new_table,
                                       struct Tuple *t, uint64_t h) {
  uint32_t short_hash = (uint32_t)h;
  uint32_t ideal_idx = (uint32_t)(h % new_table->capacity);

  for (int offset = 0; offset < HOPSCOTCH_NEIGHBORHOOD; offset++) {
    uint32_t candidate = (ideal_idx + (uint32_t)offset) % new_table->capacity;

    struct Tuple *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(
            &new_table->entries[candidate].data, &expected, t,
            memory_order_release, memory_order_relaxed)) {
      new_table->entries[candidate].hash_cache = short_hash;

      uint64_t old_hop, new_hop;
      do {
        old_hop = atomic_load_explicit(
            (_Atomic uint64_t *)&new_table->entries[ideal_idx].hop_info,
            memory_order_relaxed);
        new_hop = old_hop | (1ULL << offset);
      } while (!atomic_compare_exchange_weak_explicit(
          (_Atomic uint64_t *)&new_table->entries[ideal_idx].hop_info, &old_hop,
          new_hop, memory_order_release, memory_order_relaxed));

      return;
    }
  }

  pthread_mutex_lock(&new_table->stash_lock);
  for (int i = 0; i < STASH_SIZE; i++) {
    struct Tuple *expected = NULL;
    if (atomic_compare_exchange_strong_explicit(
            &new_table->stash[i].data, &expected, t, memory_order_release,
            memory_order_relaxed)) {
      new_table->stash[i].hash_cache = short_hash;
      atomic_fetch_add_explicit(&new_table->stash_count, 1,
                                memory_order_relaxed);
      pthread_mutex_unlock(&new_table->stash_lock);
      return;
    }
  }
  pthread_mutex_unlock(&new_table->stash_lock);

  fprintf(stderr,
          "[resize] FATAL: no se pudo migrar la tupla '%s'. "
          "Stash de new_table agotado. Memoria perdida.\n",
          t->key);
}

void *resize_worker(void *args) {
  struct ResizeArgs *r = (struct ResizeArgs *)args;

  int chunk_size = (int)r->old_table->capacity / r->total_chunks;
  int remainder = (int)r->old_table->capacity % r->total_chunks;

  while (true) {
    int my_chunk =
        atomic_fetch_add_explicit(r->chunk_counter, 1, memory_order_relaxed);
    if (my_chunk >= r->total_chunks)
      break;

    int start_idx = my_chunk * chunk_size;
    int end_idx = start_idx + chunk_size +
                  (my_chunk == r->total_chunks - 1 ? remainder : 0);

    for (int i = start_idx; i < end_idx; i++) {
      struct Tuple *t = atomic_load_explicit(&r->old_table->entries[i].data,
                                             memory_order_relaxed);
      if (t == NULL)
        continue;

      uint64_t h = hash_function(t->key);
      migrate_entry_to_new_table(r->new_table, t, h);
    }
  }

  pthread_barrier_wait(r->barrier);
  return NULL;
}

void trigger_parallel_resize(void) {
  pthread_rwlock_wrlock(&global_stw_rwlock);

  float load = (float)atomic_load_explicit(&global_table->total_items,
                                           memory_order_relaxed) /
               (float)global_table->capacity;

  if (load < LOAD_FACTOR_THRESHOLD &&
      atomic_load_explicit(&global_table->stash_count, memory_order_relaxed) <
          STASH_SIZE / 2) {
    pthread_rwlock_unlock(&global_stw_rwlock);
    return;
  }

  struct ConcurrentHashTable *new_table =
      calloc(1, sizeof(struct ConcurrentHashTable));
  if (!new_table) {
    fprintf(stderr,
            "[resize] OOM: no se pudo asignar new_table. Resize abortado.\n");
    pthread_rwlock_unlock(&global_stw_rwlock);
    return;
  }

  new_table->capacity = global_table->capacity * 2;
  new_table->entries = calloc(new_table->capacity, sizeof(struct HashEntry));
  if (!new_table->entries) {
    fprintf(stderr,
            "[resize] OOM: no se pudo asignar entries. Resize abortado.\n");
    free(new_table);
    pthread_rwlock_unlock(&global_stw_rwlock);
    return;
  }

  pthread_mutex_init(&new_table->stash_lock, NULL);
  atomic_store_explicit(
      &new_table->total_items,
      atomic_load_explicit(&global_table->total_items, memory_order_relaxed),
      memory_order_relaxed);

  long cores = sysconf(_SC_NPROCESSORS_ONLN);
  int num_threads = (cores > 0) ? (int)cores : 2;
  if (num_threads > 32)
    num_threads = 32;

  pthread_t *workers = malloc((size_t)num_threads * sizeof(pthread_t));
  pthread_barrier_t barrier;

  if (!workers) {
    fprintf(stderr, "[resize] OOM: no se pudo asignar array de workers.\n");
    free(new_table->entries);
    free(new_table);
    pthread_rwlock_unlock(&global_stw_rwlock);
    return;
  }

  pthread_barrier_init(&barrier, NULL, (unsigned)(num_threads + 1));
  atomic_int chunk_counter = ATOMIC_VAR_INIT(0);

  int total_chunks_raw = num_threads * 16;
  int total_chunks = 1;
  while (total_chunks < total_chunks_raw)
    total_chunks <<= 1;

  struct ResizeArgs resize_args = {
      .old_table = global_table,
      .new_table = new_table,
      .chunk_counter = &chunk_counter,
      .total_chunks = total_chunks,
      .barrier = &barrier,
  };

  for (int i = 0; i < num_threads; i++) {
    if (pthread_create(&workers[i], NULL, resize_worker, &resize_args) != 0) {
      fprintf(stderr, "[resize] pthread_create falló en hilo %d.\n", i);
      num_threads = i;
      pthread_barrier_destroy(&barrier);
      pthread_barrier_init(&barrier, NULL, (unsigned)(num_threads + 1));
      break;
    }
  }

  for (int i = 0; i < STASH_SIZE; i++) {
    struct Tuple *t = atomic_load_explicit(&global_table->stash[i].data,
                                           memory_order_relaxed);
    if (t == NULL)
      continue;

    uint64_t h = hash_function(t->key);
    migrate_entry_to_new_table(new_table, t, h);
  }

  pthread_barrier_wait(&barrier);

  for (int i = 0; i < num_threads; i++) {
    pthread_join(workers[i], NULL);
  }
  pthread_barrier_destroy(&barrier);
  free(workers);

  struct ConcurrentHashTable *old_table = global_table;
  global_table = new_table;

  pthread_rwlock_unlock(&global_stw_rwlock);

  free(old_table->entries);
  pthread_mutex_destroy(&old_table->stash_lock);
  free(old_table);
}

void *background_free(void *arg) {
  struct ConcurrentHashTable *old_table = (struct ConcurrentHashTable *)arg;

  for (uint32_t i = 0; i < old_table->capacity; i++) {
    struct Tuple *t = atomic_load(&old_table->entries[i].data);
    if (t != NULL) {
      hp_retire(t);
    }
  }
  for (int i = 0; i < STASH_SIZE; i++) {
    struct Tuple *t = atomic_load(&old_table->stash[i].data);
    if (t != NULL) {
      hp_retire(t);
    }
  }
  hp_scan();

  free(old_table->entries);
  pthread_mutex_destroy(&old_table->stash_lock);
  free(old_table);
  return NULL;
}

/* ========================================================================= *
 * IMPLEMENTACIÓN DE LA API (CLIENTE)
 * ========================================================================= */

int destroy(void) {
  struct ConcurrentHashTable *new_table =
      calloc(1, sizeof(struct ConcurrentHashTable));
  if (!new_table)
    return -1;

  new_table->capacity = INITIAL_CAPACITY;
  new_table->entries = calloc(new_table->capacity, sizeof(struct HashEntry));
  if (!new_table->entries) {
    free(new_table);
    return -1;
  }
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
}

int set_value(char *key, char *value1, int N_value2, float *V_value2,
              struct Paquete value3) {
  if (!atomic_load(&servicio_iniciado) || key == NULL || value1 == NULL ||
      V_value2 == NULL)
    return -1;
  if (N_value2 < 1 || N_value2 > 32)
    return -1;
  if (strnlen(key, MAX_STR_LEN + 1) > MAX_STR_LEN ||
      strnlen(value1, MAX_STR_LEN + 1) > MAX_STR_LEN)
    return -1;

  if (exist(key) == 1)
    return -1;

  struct Tuple *new_tuple = malloc(sizeof(struct Tuple));
  if (!new_tuple)
    return -1;

  strcpy(new_tuple->key, key);
  strcpy(new_tuple->value1, value1);
  new_tuple->N_value2 = N_value2;
  memcpy(new_tuple->V_value2, V_value2, N_value2 * sizeof(float));
  new_tuple->value3 = value3;

  uint64_t full_hash = hash_function(key);
  uint32_t short_hash = (uint32_t)full_hash;

  int retries = 0;
  const int MAX_RETRIES = 3;

  while (retries < MAX_RETRIES) {
    pthread_rwlock_rdlock(&global_stw_rwlock);

    uint32_t ideal_idx = full_hash % global_table->capacity;
    int seg_idx, seg_idx2;
    neighborhood_write_lock(ideal_idx, global_table->capacity, &seg_idx,
                            &seg_idx2);

    uint64_t hop_info_check = global_table->entries[ideal_idx].hop_info;
    bool already_exists = false;

    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
      if ((hop_info_check & (1ULL << i)) != 0) {
        uint32_t ci = (ideal_idx + i) % global_table->capacity;
        struct Tuple *ex = atomic_load(&global_table->entries[ci].data);
        if (ex != NULL && strcmp(ex->key, key) == 0) {
          already_exists = true;
          break;
        }
      }
    }

    // CORRECCIÓN DOBLE-CHECK STASH: Comprobar también el stash bajo el lock de
    // la vecindad
    if (!already_exists) {
      pthread_mutex_lock(&global_table->stash_lock);
      for (int i = 0; i < STASH_SIZE; i++) {
        struct Tuple *st = atomic_load(&global_table->stash[i].data);
        if (st != NULL && global_table->stash[i].hash_cache == short_hash &&
            strcmp(st->key, key) == 0) {
          already_exists = true;
          break;
        }
      }
      pthread_mutex_unlock(&global_table->stash_lock);
    }

    if (already_exists) {
      neighborhood_write_unlock(seg_idx, seg_idx2);
      pthread_rwlock_unlock(&global_stw_rwlock);
      free(new_tuple);
      return -1;
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

    neighborhood_write_unlock(seg_idx, seg_idx2);

    if (inserted) {
      pthread_rwlock_unlock(&global_stw_rwlock);

      float load = (float)atomic_load(&global_table->total_items) /
                   global_table->capacity;
      if (load > LOAD_FACTOR_THRESHOLD) {
        trigger_parallel_resize();
      }
      return 0;
    }

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

    pthread_rwlock_unlock(&global_stw_rwlock);

    if (stash_idx != -1) {
      float load = (float)atomic_load(&global_table->total_items) /
                   global_table->capacity;
      if (load > LOAD_FACTOR_THRESHOLD ||
          atomic_load(&global_table->stash_count) >= STASH_SIZE) {
        trigger_parallel_resize();
      }
      return 0;
    }

    trigger_parallel_resize();
    retries++;
  }

  free(new_tuple);
  return -1;
}

int get_value(char *key, char *value1, int *N_value2, float *V_value2,
              struct Paquete *value3) {
  if (!atomic_load(&servicio_iniciado) || key == NULL)
    return -1;
  if (strnlen(key, MAX_STR_LEN + 1) > MAX_STR_LEN)
    return -1;

  uint64_t full_hash = hash_function(key);
  uint32_t short_hash = (uint32_t)full_hash;

  pthread_rwlock_rdlock(&global_stw_rwlock);

  uint32_t ideal_idx = full_hash % global_table->capacity;
  int seg_idx = get_segment(ideal_idx, global_table->capacity);
  uint32_t last_idx =
      (ideal_idx + HOPSCOTCH_NEIGHBORHOOD - 1) % global_table->capacity;
  int seg_idx2 = get_segment(last_idx, global_table->capacity);
  unsigned seq1, seq2;
  bool found = false;

  do {
    seq1 = read_begin(&global_table->segment_locks[seg_idx].val);
    seq2 = (seg_idx2 != seg_idx)
               ? read_begin(&global_table->segment_locks[seg_idx2].val)
               : 0;

    uint64_t hop_info = global_table->entries[ideal_idx].hop_info;

    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
      if ((hop_info & (1ULL << i)) != 0) {
        uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
        if (global_table->entries[check_idx].hash_cache == short_hash) {

          struct Tuple *t = atomic_load(&global_table->entries[check_idx].data);
          if (t != NULL) {
            hp_set((void *)t);

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
            hp_clear();
          }
        }
      }
    }
  } while (
      neighborhood_read_retry(ideal_idx, global_table->capacity, seq1, seq2));

  if (!found) {
    pthread_mutex_lock(&global_table->stash_lock);
    for (int i = 0; i < STASH_SIZE; i++) {
      struct Tuple *t = atomic_load(&global_table->stash[i].data);
      if (t != NULL && global_table->stash[i].hash_cache == short_hash &&
          strcmp(t->key, key) == 0) {
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

int modify_value(char *key, char *value1, int N_value2, float *V_value2,
                 struct Paquete value3) {
  if (!atomic_load(&servicio_iniciado) || key == NULL || value1 == NULL ||
      V_value2 == NULL)
    return -1;
  if (N_value2 < 1 || N_value2 > 32)
    return -1;
  if (strnlen(key, MAX_STR_LEN + 1) > MAX_STR_LEN ||
      strnlen(value1, MAX_STR_LEN + 1) > MAX_STR_LEN)
    return -1;

  uint64_t full_hash = hash_function(key);
  uint32_t short_hash = (uint32_t)full_hash;

  pthread_rwlock_rdlock(&global_stw_rwlock);

  uint32_t ideal_idx = full_hash % global_table->capacity;
  int seg_idx, seg_idx2;
  neighborhood_write_lock(ideal_idx, global_table->capacity, &seg_idx,
                          &seg_idx2);
  bool modified = false;

  uint64_t hop_info = global_table->entries[ideal_idx].hop_info;
  for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
    if ((hop_info & (1ULL << i)) != 0) {
      uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
      if (global_table->entries[check_idx].hash_cache == short_hash) {

        struct Tuple *t = atomic_load(&global_table->entries[check_idx].data);
        if (t != NULL && strcmp(t->key, key) == 0) {

          struct Tuple *new_tuple = malloc(sizeof(struct Tuple));
          if (!new_tuple) {
            neighborhood_write_unlock(seg_idx, seg_idx2);
            pthread_rwlock_unlock(&global_stw_rwlock);
            return -1;
          }
          *new_tuple = *t;

          strcpy(new_tuple->value1, value1);
          new_tuple->N_value2 = N_value2;
          memcpy(new_tuple->V_value2, V_value2, N_value2 * sizeof(float));
          new_tuple->value3 = value3;

          atomic_store(&global_table->entries[check_idx].data, new_tuple);
          hp_retire(t);

          modified = true;
          break;
        }
      }
    }
  }
  neighborhood_write_unlock(seg_idx, seg_idx2);

  if (!modified) {
    pthread_mutex_lock(&global_table->stash_lock);
    for (int i = 0; i < STASH_SIZE; i++) {
      struct Tuple *t = atomic_load(&global_table->stash[i].data);
      if (t != NULL && global_table->stash[i].hash_cache == short_hash &&
          strcmp(t->key, key) == 0) {

        struct Tuple *new_tuple = malloc(sizeof(struct Tuple));
        if (!new_tuple) {
          pthread_mutex_unlock(&global_table->stash_lock);
          pthread_rwlock_unlock(&global_stw_rwlock);
          return -1;
        }
        *new_tuple = *t;

        strcpy(new_tuple->value1, value1);
        new_tuple->N_value2 = N_value2;
        memcpy(new_tuple->V_value2, V_value2, N_value2 * sizeof(float));
        new_tuple->value3 = value3;

        atomic_store(&global_table->stash[i].data, new_tuple);
        hp_retire(t);

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
  if (!atomic_load(&servicio_iniciado) || key == NULL)
    return -1;
  if (strnlen(key, MAX_STR_LEN + 1) > MAX_STR_LEN)
    return -1;

  uint64_t full_hash = hash_function(key);
  uint32_t short_hash = (uint32_t)full_hash;

  pthread_rwlock_rdlock(&global_stw_rwlock);

  uint32_t ideal_idx = full_hash % global_table->capacity;
  int seg_idx, seg_idx2;
  neighborhood_write_lock(ideal_idx, global_table->capacity, &seg_idx,
                          &seg_idx2);
  bool deleted = false;

  uint64_t hop_info = global_table->entries[ideal_idx].hop_info;
  for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
    if ((hop_info & (1ULL << i)) != 0) {
      uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
      if (global_table->entries[check_idx].hash_cache == short_hash) {
        struct Tuple *t = atomic_load(&global_table->entries[check_idx].data);
        if (t != NULL && strcmp(t->key, key) == 0) {

          atomic_store(&global_table->entries[check_idx].data, NULL);
          global_table->entries[check_idx].hash_cache = 0;
          global_table->entries[ideal_idx].hop_info &= ~(1ULL << i);

          atomic_fetch_sub(&global_table->total_items, 1);
          hp_retire(t);

          deleted = true;
          break;
        }
      }
    }
  }
  neighborhood_write_unlock(seg_idx, seg_idx2);

  if (!deleted) {
    pthread_mutex_lock(&global_table->stash_lock);
    for (int i = 0; i < STASH_SIZE; i++) {
      struct Tuple *t = atomic_load(&global_table->stash[i].data);
      if (t != NULL && global_table->stash[i].hash_cache == short_hash &&
          strcmp(t->key, key) == 0) {
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
  if (!atomic_load(&servicio_iniciado) || key == NULL)
    return -1;
  if (strnlen(key, MAX_STR_LEN + 1) > MAX_STR_LEN)
    return -1;

  uint64_t full_hash = hash_function(key);
  uint32_t short_hash = (uint32_t)full_hash;

  pthread_rwlock_rdlock(&global_stw_rwlock);

  uint32_t ideal_idx = full_hash % global_table->capacity;
  int seg_idx = get_segment(ideal_idx, global_table->capacity);
  uint32_t last_idx =
      (ideal_idx + HOPSCOTCH_NEIGHBORHOOD - 1) % global_table->capacity;
  int seg_idx2 = get_segment(last_idx, global_table->capacity);
  unsigned seq1, seq2;
  bool found = false;

  do {
    seq1 = read_begin(&global_table->segment_locks[seg_idx].val);
    seq2 = (seg_idx2 != seg_idx)
               ? read_begin(&global_table->segment_locks[seg_idx2].val)
               : 0;

    uint64_t hop_info = global_table->entries[ideal_idx].hop_info;

    for (int i = 0; i < HOPSCOTCH_NEIGHBORHOOD; i++) {
      if ((hop_info & (1ULL << i)) != 0) {
        uint32_t check_idx = (ideal_idx + i) % global_table->capacity;
        if (global_table->entries[check_idx].hash_cache == short_hash) {

          struct Tuple *t = atomic_load(&global_table->entries[check_idx].data);
          if (t != NULL) {
            hp_set((void *)t);
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
  } while (
      neighborhood_read_retry(ideal_idx, global_table->capacity, seq1, seq2));

  if (!found) {
    pthread_mutex_lock(&global_table->stash_lock);
    for (int i = 0; i < STASH_SIZE; i++) {
      struct Tuple *t = atomic_load(&global_table->stash[i].data);
      if (t != NULL && global_table->stash[i].hash_cache == short_hash &&
          strcmp(t->key, key) == 0) {
        found = true;
        break;
      }
    }
    pthread_mutex_unlock(&global_table->stash_lock);
  }

  pthread_rwlock_unlock(&global_stw_rwlock);
  return found ? 1 : 0;
}