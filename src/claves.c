#include "claves.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/*
 * IMPLEMENTACIÓN LOCAL DE LA LÓGICA DE NEGOCIO.
 * Este archivo contiene la estructura de datos real (Lista Enlazada) y el Mutex.
 * Se compilará como 'libclaves.so'.
 */

/* 
 * 1. DEFINICIÓN DE ESTRUCTURAS INTERNAS
 * Se definirá aquí el 'struct Node' para la lista enlazada.
 * Contendrá: key, value1, N_value2, v_value2, value3, y struct Node *next.
 */

/* 
 * 2. VARIABLES GLOBALES
 * - Puntero 'head' al inicio de la lista.
 * - Mutex global (pthread_mutex_t) para proteger el acceso concurrente a la lista
 *   (necesario porque el servidor lanzará hilos concurrentes que usarán esta librería).
 */

/* 
 * 3. IMPLEMENTACIÓN DE LAS FUNCIONES DE LA API (init, set_value, get_value...)
 * Cada función seguirá este patrón:
 *    a. Bloquear el mutex (pthread_mutex_lock).
 *    b. Realizar la operación en la lista (recorrer, insertar, copiar datos, borrar).
 *       Nota: Usar strncpy/memcpy para guardar copias profundas de los datos, 
 *       no guardar solo los punteros recibidos.
 *    c. Desbloquear el mutex (pthread_mutex_unlock).
 *    d. Devolver el resultado (0, 1, o -1).
 */