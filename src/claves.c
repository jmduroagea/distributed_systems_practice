#include "../include/claves.h"
#include "hash-table.h"
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stddef.h>

/* ========================================================================= *
 * ESTADO GLOBAL DEL SERVICIO
 * ========================================================================= */
static ConcurrentHashTable *global_table    = NULL;
static atomic_int            servicio_iniciado = ATOMIC_VAR_INIT(0);
static pthread_mutex_t       init_mutex     = PTHREAD_MUTEX_INITIALIZER;

/* ========================================================================= *
 * IMPLEMENTACIÓN DE LA API
 * ========================================================================= */

int destroy(void) {
    pthread_mutex_lock(&init_mutex);

    if (global_table == NULL) {
        /*
         * Bug 2 corregido: ht_create() no acepta parámetros. La llamada
         * original era ht_create(4096), incompatible con la firma real
         * ht_create(void), lo que causaba error de compilación o UB.
         */
        global_table = ht_create();
        if (global_table != NULL) {
            atomic_store(&servicio_iniciado, 1);
        }
    } else {
        /* Ya existía: vaciar de forma segura sin romper punteros */
        ht_clear(global_table);
    }

    pthread_mutex_unlock(&init_mutex);
    return (global_table != NULL) ? 0 : -1;
}

int set_value(char *key, char *value1, int N_value2, float *V_value2,
              struct Paquete value3) {
    if (!atomic_load(&servicio_iniciado) || key == NULL || value1 == NULL ||
        V_value2 == NULL)
        return -1;
    if (N_value2 < 1 || N_value2 > 32) return -1;
    if (strnlen(key,    MAX_STR_LEN + 1) > MAX_STR_LEN) return -1;
    if (strnlen(value1, MAX_STR_LEN + 1) > MAX_STR_LEN) return -1;

    struct Tuple input_data;
    strcpy(input_data.key,    key);
    strcpy(input_data.value1, value1);
    input_data.N_value2 = N_value2;
    memcpy(input_data.V_value2, V_value2, N_value2 * sizeof(float));
    input_data.value3 = value3;

    return ht_insert(global_table, &input_data);
}

int get_value(char *key, char *value1, int *N_value2, float *V_value2,
              struct Paquete *value3) {
    if (!atomic_load(&servicio_iniciado) || key == NULL) return -1;
    if (strnlen(key, MAX_STR_LEN + 1) > MAX_STR_LEN) return -1;

    struct Tuple out_data;
    if (ht_get(global_table, key, &out_data) == 0) {
        strcpy(value1, out_data.value1);
        *N_value2 = out_data.N_value2;
        memcpy(V_value2, out_data.V_value2, out_data.N_value2 * sizeof(float));
        *value3 = out_data.value3;
        return 0;
    }
    return -1;
}

int modify_value(char *key, char *value1, int N_value2, float *V_value2,
                 struct Paquete value3) {
    if (!atomic_load(&servicio_iniciado) || key == NULL || value1 == NULL ||
        V_value2 == NULL)
        return -1;
    if (N_value2 < 1 || N_value2 > 32) return -1;
    if (strnlen(key,    MAX_STR_LEN + 1) > MAX_STR_LEN) return -1;
    if (strnlen(value1, MAX_STR_LEN + 1) > MAX_STR_LEN) return -1;

    struct Tuple input_data;
    strcpy(input_data.key,    key);
    strcpy(input_data.value1, value1);
    input_data.N_value2 = N_value2;
    memcpy(input_data.V_value2, V_value2, N_value2 * sizeof(float));
    input_data.value3 = value3;

    return ht_modify(global_table, &input_data);
}

int delete_key(char *key) {
    if (!atomic_load(&servicio_iniciado) || key == NULL) return -1;
    if (strnlen(key, MAX_STR_LEN + 1) > MAX_STR_LEN) return -1;

    return ht_remove(global_table, key);
}

int exist(char *key) {
    if (!atomic_load(&servicio_iniciado) || key == NULL) return -1;
    if (strnlen(key, MAX_STR_LEN + 1) > MAX_STR_LEN) return -1;

    return ht_exists(global_table, key) ? 1 : 0;
}