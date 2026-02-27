#include "claves.h"
#include "mensajes.h"
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
// ...

/*
 * IMPLEMENTACIÓN DEL CLIENTE (Parte B - Proxy)
 * Genera la librería: libproxyclaves.so
 *
 * RESPONSABILIDAD:
 * - Ofrece la misma API que claves.c.
 * - No almacena datos.
 * - Empaqueta los argumentos en struct Peticion.
 * - Envía la petición a la cola del servidor.
 * - Espera la respuesta en una cola privada única por proceso.
 */

/* Variables globales del proxy */
// mqd_t server_queue;
// mqd_t client_queue;
// char client_queue_name[256];

/*
 * init()
 * - Genera nombre de cola único usando getpid().
 * - Crea la cola del cliente (bloqueante).
 * - Abre la cola del servidor.
 * - Envía OP_INIT si es necesario.
 */

/*
 * set_value(), get_value(), etc.
 * - Serializan los parámetros en 'struct Peticion'.
 * - mq_send() -> Servidor.
 * - mq_receive() <- Servidor (bloqueante).
 * - Desempaquetan 'struct Respuesta' y retornan valores.
 * - Retornan -2 en caso de fallo de mq_send/mq_receive.
 */