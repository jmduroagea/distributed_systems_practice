/**
  * @file mensajes.h
  * @brief Definición de estructuras y constantes para el protocolo de
  *        comunicación entre el Proxy y el Servidor usando Colas de Mensajes POSIX.
  *        Incluye códigos de operación, formatos de petición y respuesta, y límites
  *        estrictos según el enunciado.
  **/

#ifndef MENSAJES_H
#define MENSAJES_H

#include "../include/claves.h"
#include <stddef.h>

// =========================================================
// CONSTANTES ARQUITECTÓNICAS
// =========================================================
#define SERVER_QUEUE "/claves_server_req"
#define MAX_QUEUE_NAME 64

// Límites definidos por el enunciado
#define MAX_KEY_LEN 256  // 255 caracteres + '\0'
#define MAX_VAL1_LEN 256 // 255 caracteres + '\0'
#define MAX_VEC_LEN 32   // Máximo de elementos en vector float

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

typedef struct {
  char key[MAX_KEY_LEN];
  char value1[MAX_VAL1_LEN];
  int N_value2;
  float V_value2[MAX_VEC_LEN];
  struct Paquete value3;
} PayloadEscribir;

typedef struct {
  char key[MAX_KEY_LEN];
} PayloadLeer;

// =========================================================
// PETICIÓN (Cliente -> Servidor)
// =========================================================

typedef struct {
  Operacion op_code;

  char q_name[MAX_QUEUE_NAME]; // Cola de respuesta por cliente

  union {
    PayloadEscribir escritura; // OP_SET, OP_MODIFY
    PayloadLeer lectura;       // OP_GET, OP_DELETE, OP_EXIST
  } payload;
} Peticion;

// Tamaños por operación — evita enviar bytes innecesarios
#define PETICION_HEADER_SIZE (offsetof(Peticion, payload))
#define PETICION_SIZE_LEER (PETICION_HEADER_SIZE + sizeof(PayloadLeer))
#define PETICION_SIZE_ESCRIB (PETICION_HEADER_SIZE + sizeof(PayloadEscribir))
#define PETICION_SIZE_INIT (PETICION_HEADER_SIZE)

// Macro que devuelve el tamaño correcto según la operación
#define PETICION_SIZE(op)                                                      \
  (size_t)(((op) == OP_SET || (op) == OP_MODIFY) ? PETICION_SIZE_ESCRIB        \
           : ((op) == OP_INIT)                   ? PETICION_SIZE_INIT          \
                                                 : PETICION_SIZE_LEER)

// =========================================================
// RESPUESTAS (Servidor -> Cliente)
// =========================================================

// Respuesta simple: destroy, set, modify, delete, exist — solo 4 bytes
typedef struct {
  int result;
} RespSimple;

// Respuesta completa: solo get
typedef struct {
  int result;
  char value1[MAX_VAL1_LEN];
  int N_value2;
  float V_value2[MAX_VEC_LEN];
  struct Paquete value3;
} RespGet;

// Tamaño máximo para mq_attr.mq_msgsize
#define RESPUESTA_MAX_SIZE (sizeof(RespGet))

#endif // MENSAJES_H