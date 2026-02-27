#ifndef MENSAJES_H
#define MENSAJES_H

#include "claves.h"

/*
 * PROTOCOLO DE COMUNICACIÓN (Parte B)
 * Este archivo define los formatos de intercambio de datos entre el Cliente (Proxy)
 * y el Servidor a través de las colas de mensajes POSIX.
 */

/* 1. CÓDIGOS DE OPERACIÓN */
/* Identifican qué función de la API se desea ejecutar */
#define OP_INIT    0
#define OP_SET     1
#define OP_GET     2
#define OP_MODIFY  3
#define OP_DELETE  4
#define OP_EXIST   5

/* Definiciones de límites para las estructuras de mensajería */
#define MAX_KEY_LEN 256
#define MAX_VAL1_LEN 256
#define MAX_VEC_LEN 32

/* 
 * 2. ESTRUCTURA DEL MENSAJE DE PETICIÓN (Cliente -> Servidor)
 * Contiene la unión de todos los argumentos posibles de las funciones de la API.
 */
typedef struct {
    int op_code;                    // Código de operación (OP_SET, OP_GET, etc.)
    char q_name[MAX_KEY_LEN];       // Nombre de la cola de respuesta del cliente (/cola_pid)
    
    // Datos de la tupla (usados según la operación)
    char key[MAX_KEY_LEN];          
    char value1[MAX_VAL1_LEN];
    int N_value2;                   
    float V_value2[MAX_VEC_LEN];    // Vector estático [32]
    struct Paquete value3;
} Peticion;

/* 
 * 3. ESTRUCTURA DEL MENSAJE DE RESPUESTA (Servidor -> Cliente)
 * Contiene el resultado de la operación y los datos de salida si procede.
 */
typedef struct {
    int result;                     // 0 (éxito), -1 (error lógico), -2 (error coms - gestionado por proxy)
    
    // Datos de retorno (solo para OP_GET)
    char value1[MAX_VAL1_LEN];
    int N_value2;
    float V_value2[MAX_VEC_LEN];
    struct Paquete value3;
} Respuesta;

#endif