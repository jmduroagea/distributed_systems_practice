#include "claves.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/*
 * IMPLEMENTACIÓN DEL SERVICIO (Parte A y Lógica del Servidor)
 * Genera la librería: libclaves.so
 *
 * DECISIONES DE DISEÑO:
 * 1. Estructura de Datos: TABLA HASH con encadenamiento para colisiones.
 * 2. Gestión de Memoria: COPIA PROFUNDA (Deep Copy). Se duplican las cadenas
 *    (key, value1) usando memoria dinámica.
 * 3. Vector V2: Estático (float v[32]) dentro del nodo para optimizar
 *    acceso y reducir fragmentación, dado que el límite es bajo.
 * 4. Concurrencia: Uso de MUTEX global para proteger la tabla hash.
 */

// Tamaño de la tabla hash (número de buckets)
#define HASH_SIZE 1024

/* Estructura del Nodo para la Tabla Hash */
typedef struct Node {
    char *key;                  // Copia dinámica
    char *value1;               // Copia dinámica
    int N_value2;
    float V_value2[32];         // Almacenamiento estático
    struct Paquete value3;
    struct Node *next;          // Puntero para colisiones
} Node;

// Tabla Hash Global
Node *tabla_hash[HASH_SIZE];

// Mutex para exclusión mutua (Thread-Safety)
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Flag de inicialización
int inicializado = 0;

/* FUNCIONES INTERNAS */
// unsigned int hash_function(const char *str);

/* 
 * IMPLEMENTACIÓN DE LA API (init, set_value, get_value, etc.)
 * Todas las funciones siguen el esquema:
 * Lock -> Operación en Tabla Hash -> Unlock -> Return
 */