#ifndef MENSAJES_H
#define MENSAJES_H

#include "claves.h"

/*
 * ESTE ARCHIVO DEFINE EL PROTOCOLO DE COMUNICACIÓN.
 * Será incluido tanto por el Proxy (Cliente) como por el Servidor.
 */

/* 
 * 1. DEFINICIÓN DE CÓDIGOS DE OPERACIÓN
 * Se definirán constantes para identificar qué función se está solicitando.
 * Ejemplo: 
 * #define OP_INIT    0
 * #define OP_SET     1
 * #define OP_GET     2
 * ...
 */

/* 
 * 2. ESTRUCTURA DEL MENSAJE DE PETICIÓN (CLIENTE -> SERVIDOR)
 * Esta estructura debe contener todos los campos necesarios para realizar cualquier operación.
 * Campos necesarios:
 * - Código de operación (int).
 * - Nombre de la cola del cliente (char array) para que el servidor sepa dónde responder.
 * - Clave (char array).
 * - Valor1 (char array).
 * - N_value2 (int).
 * - Vector value2 (float array[32]).
 * - Estructura value3 (struct Paquete).
 *
 * Nota: Dado que mq_receive requiere un buffer fijo, esta estructura debe ser
 * la unión de todos los parámetros posibles. Si una operación (ej. delete) 
 * no usa 'valor1', ese campo simplemente se ignorará en el servidor.
 */

/* 
 * 3. ESTRUCTURA DEL MENSAJE DE RESPUESTA (SERVIDOR -> CLIENTE)
 * Esta estructura contiene el resultado de la operación.
 * Campos necesarios:
 * - Código de retorno (int): 0 para éxito, -1 para error lógica, -2 error comms.
 * - Valor1 (char array) -> Solo útil para get_value.
 * - N_value2 (int) -> Solo útil para get_value.
 * - Vector value2 (float array[32]) -> Solo útil para get_value.
 * - Estructura value3 (struct Paquete) -> Solo útil para get_value.
 */

#endif