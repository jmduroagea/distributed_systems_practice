#include "../../include/hash-table.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Mantenemos el nombre (hash-table.c) para evitar problemas con las dependencias de otros archivos, pero en este fichero solo se implementa una lista enlazada simple, que hace uso de un mutex global, nada fancy */


/* ESTRUCTURAS INTERNAS */
 
/* Nodo de la lista enlazada: contiene la tupla y un puntero al siguiente. */
struct Nodo {
    struct Tuple datos;
    struct Nodo *nodo_siguiente;
};
 
/* Mantenemos esto para evitar hacer cambios también sobre hash-table.h. De esta manera, mantenemos aisladas las implementaciones de cada archivo */
struct ConcurrentHashTable {
    struct Nodo    *head;
    pthread_mutex_t mutex;
};
 
/* HELPERS INTERNOS (asumen que el mutex ya está adquirido) */
 
/* Busca un nodo para cada clave. Devuelve NULL si no existe. */
static struct Nodo *find_node(struct ConcurrentHashTable *table, const char *key) {
    for (struct Nodo *n = table->head; n != NULL; n = n->nodo_siguiente) {
        if (strcmp(n->datos.key, key) == 0) {
            return n;
        }
    }
    return NULL;
}
 
/* Libera todos los nodos de la lista y deja al head en NULL. */
static void free_all_nodes(struct ConcurrentHashTable *table) {
    struct Nodo *n = table->head;
    while (n != NULL) {
        struct Nodo *nodo_siguiente = n->nodo_siguiente;
        free(n);
        n = nodo_siguiente;
    }
    table->head = NULL;
}
 
/* API PÚBLICA */
 
ConcurrentHashTable *ht_create(void) {
    struct ConcurrentHashTable *table = malloc(sizeof(*table));
    if (table == NULL) {
        return NULL;
    }
    table->head = NULL;
    if (pthread_mutex_init(&table->mutex, NULL) != 0) {
        free(table);
        return NULL;
    }
    return table;
}
 
int ht_clear(ConcurrentHashTable *table) {
    if (table == NULL) return -1;
    pthread_mutex_lock(&table->mutex);
    free_all_nodes(table);
    pthread_mutex_unlock(&table->mutex);
    return 0;
}
 
int ht_insert(ConcurrentHashTable *table, const struct Tuple *datos) {
    if (table == NULL || datos == NULL) return -1;
    pthread_mutex_lock(&table->mutex);
    if (find_node(table, datos->key) != NULL) {      /* Evitamos guardar claves duplicadas */
        pthread_mutex_unlock(&table->mutex);
        return -1;
    }
 
    struct Nodo *new_node = malloc(sizeof(*new_node));
    if (new_node == NULL) {
        pthread_mutex_unlock(&table->mutex);
        return -1;
    }
 
    /* Copia por cada valor */
    new_node->datos = *datos;

    /* Inserción en el inicio de la lista */
    new_node->nodo_siguiente = table->head;
    table->head    = new_node;
 
    pthread_mutex_unlock(&table->mutex);
    return 0;
}
 
int ht_get(ConcurrentHashTable *table, const char *key, struct Tuple *out_datos) {
    if (table == NULL || key == NULL || out_datos == NULL) return -1;
 
    pthread_mutex_lock(&table->mutex);
 
    struct Nodo *n = find_node(table, key);
    if (n == NULL) {
        pthread_mutex_unlock(&table->mutex);
        return -1;
    }

    *out_datos = n->datos;
 
    pthread_mutex_unlock(&table->mutex);
    return 0;
}
 
int ht_modify(ConcurrentHashTable *table, const struct Tuple *datos) {
    if (table == NULL || datos == NULL) return -1;
 
    pthread_mutex_lock(&table->mutex);
 
    struct Nodo *n = find_node(table, datos->key);
    if (n == NULL) {
        pthread_mutex_unlock(&table->mutex);
        return -1;
    }

    n->datos = *datos;    /* Aquí sobreescribimos la clave sin cambiar la posición en la lista, lo que sería muy poco óptimo */

    pthread_mutex_unlock(&table->mutex);
    return 0;
}

int ht_remove(ConcurrentHashTable *table, const char *key) {
    if (table == NULL || key == NULL) return -1;

    pthread_mutex_lock(&table->mutex);

    struct Nodo *prev = NULL;
    struct Nodo *nodo_actual = table->head;
    while (nodo_actual != NULL) {
        if (strcmp(nodo_actual->datos.key, key) == 0) {
            if (prev == NULL) {
                table->head = nodo_actual->nodo_siguiente;
            } else {
                prev->nodo_siguiente = nodo_actual->nodo_siguiente;
            }
            free(nodo_actual);
            pthread_mutex_unlock(&table->mutex);
            return 0;
        }
        prev = nodo_actual;
        nodo_actual = nodo_actual->nodo_siguiente;
    }

    pthread_mutex_unlock(&table->mutex);
    return -1; /* No existía. */
}

bool ht_exists(ConcurrentHashTable *table, const char *key) {
    if (table == NULL || key == NULL) return false;
    
    pthread_mutex_lock(&table->mutex);
    bool found = (find_node(table, key) != NULL);
    pthread_mutex_unlock(&table->mutex);
    return found;
}