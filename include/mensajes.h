#ifndef MENSAJES_H
#define MENSAJES_H

#include "claves.h" // Necesario para struct Paquete [cite: 4]

/*
 * PROTOCOLO DE COMUNICACIÓN (RPC Distribuido)
 * Este archivo define los formatos de intercambio de datos entre el Proxy
 * y el Servidor utilizando colas de mensajes POSIX.
 */

// =========================================================
// CONSTANTES ARQUITECTÓNICAS
// =========================================================
#define SERVER_QUEUE "/claves_server_req"
#define MAX_QUEUE_NAME 64

// Límites estrictos definidos por el enunciado
#define MAX_KEY_LEN 256  // 255 caracteres + '\0' [cite: 8]
#define MAX_VAL1_LEN 256 // 255 caracteres + '\0' [cite: 10]
#define MAX_VEC_LEN 32   // Máximo de elementos en vector float [cite: 11, 13]

// =========================================================
// CÓDIGOS DE OPERACIÓN
// =========================================================
typedef enum {
    OP_INIT = 0, // destroy()
    OP_SET,      // set_value()
    OP_GET,      // get_value()
    OP_MODIFY,   // modify_value()
    OP_DELETE,   // delete_key()
    OP_EXIST     // exist()
} Operacion;

// =========================================================
// PAYLOADS DE LA PETICIÓN (Estrategia de Unión)
// =========================================================
// Usado por OP_SET y OP_MODIFY
typedef struct {
    char key[MAX_KEY_LEN];
    char value1[MAX_VAL1_LEN];
    int N_value2;
    float V_value2[MAX_VEC_LEN];
    struct Paquete value3;       // Estructura cliente [cite: 14]
} PayloadEscribir;

// Usado por OP_GET, OP_DELETE y OP_EXIST
typedef struct {
    char key[MAX_KEY_LEN];
} PayloadLeer;

/* * 2. ESTRUCTURA DEL MENSAJE DE PETICIÓN (Cliente -> Servidor)
 */
typedef struct {
    Operacion op_code;                      // Qué función ejecutar
    unsigned int id_correlacion;            // ID único para evitar cruce de mensajes
    char q_name[MAX_QUEUE_NAME];            // Cola de respuesta (/claves_resp_PID_TID)
    
    // La unión fuerza a que la estructura mida siempre lo mismo (seguro para POSIX)
    // pero aísla conceptualmente los datos.
    union {
        PayloadEscribir escritura;
        PayloadLeer lectura;
    } payload;
} Peticion;

/* * 3. ESTRUCTURA DEL MENSAJE DE RESPUESTA (Servidor -> Cliente)
 */
typedef struct {
    unsigned int id_correlacion;    // El servidor debe devolver el mismo ID recibido
    int result;                     // 0 (éxito), -1 (error tupla), -2 (error coms) [cite: 119]
    
    // Datos de retorno (obligatorio llenarlos solo para OP_GET) [cite: 34, 35, 36]
    char value1[MAX_VAL1_LEN];
    int N_value2;
    float V_value2[MAX_VEC_LEN];
    struct Paquete value3;
} Respuesta;

#endif // MENSAJES_H