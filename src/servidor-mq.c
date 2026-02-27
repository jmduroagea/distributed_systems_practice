#include "claves.h"     // Enlaza con libclaves.so (Parte A)
#include "mensajes.h"
#include <mqueue.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * EJECUTABLE DEL SERVIDOR (Parte B)
 * Nombre del ejecutable: servidor-mq
 *
 * FUNCIONAMIENTO:
 * - Servidor Concurrente: Hilos bajo demanda.
 * - Comunicación: Colas de mensajes POSIX.
 * - Lógica: Delega en libclaves.so.
 */

#define SERVER_QUEUE_NAME "/COLA_SERVIDOR"

/*
 * Hilo Trabajador (Worker Thread)
 * 1. Recibe 'struct Peticion' como argumento.
 * 2. Abre la cola del cliente (q_name).
 * 3. Switch(op_code):
 *    - Llama a la función real (ej: set_value) de claves.c.
 * 4. Prepara 'struct Respuesta' con el resultado.
 * 5. Envía respuesta.
 * 6. Cierra cola cliente y termina.
 */

/*
 * Main
 * 1. Crea la cola del servidor.
 * 2. Bucle infinito:
 *    - mq_receive (Bloqueante).
 *    - pthread_create (Pasa el mensaje al hilo).
 *    - pthread_detach (Para liberar recursos al terminar el hilo).
 */