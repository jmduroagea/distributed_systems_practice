#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stdbool.h>
#include <stdint.h>
#include "../include/claves.h" // Para struct Paquete

#define MAX_STR_LEN 255
#define MAX_FLOATS 32

// El contenedor de datos (Tuple) para poder moverlo entre capas
struct Tuple {
    char key[MAX_STR_LEN + 1];
    char value1[MAX_STR_LEN + 1];
    int N_value2;
    float V_value2[MAX_FLOATS];
    struct Paquete value3;
};

// Puntero opaco (Nadie fuera de hash-table.c sabrá cómo está construida por dentro)
typedef struct ConcurrentHashTable ConcurrentHashTable;

// API Interna del Motor (CRUD Limpio y Encapsulado)
ConcurrentHashTable* ht_create(void);
int ht_clear(ConcurrentHashTable *table);

int ht_insert(ConcurrentHashTable *table, const struct Tuple *data);
int ht_get(ConcurrentHashTable *table, const char *key, struct Tuple *out_data);
int ht_modify(ConcurrentHashTable *table, const struct Tuple *data);
int ht_remove(ConcurrentHashTable *table, const char *key);
bool ht_exists(ConcurrentHashTable *table, const char *key);

#endif