#include "claves.h"     // Para llamar a la lógica real (libclaves.so)
#include "mensajes.h"   // Para entender los structs Peticion/Respuesta
#include <mqueue.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
// ... otros includes

/*
 * EJECUTABLE DEL SERVIDOR.
 * Encargado de recibir mensajes, lanzar hilos y coordinar la respuesta.
 */

/* 
 * 1. DEFINICIÓN DE CONSTANTES
 * - Nombre de la cola del servidor (ej: "/SERVIDOR_MQ").
 * - Tamaño de los mensajes y capacidad de la cola.
 */

/*
 * 2. FUNCIÓN DEL HILO TRABAJADOR (Worker Thread)
 * Esta función es la que ejecuta pthread_create.
 *    a. Recibe el mensaje (struct Peticion) como argumento.
 *    b. Abre la cola del cliente especificada en el mensaje.
 *    c. Según 'op_code', llama a la función correspondiente de 'claves.c' (init, set, get...).
 *    d. Prepara 'struct Respuesta' con el resultado y los datos de salida (si es un get).
 *    e. Envía la respuesta (mq_send) a la cola del cliente.
 *    f. Cierra la cola del cliente.
 *    g. Termina el hilo.
 */

/*
 * 3. FUNCIÓN MAIN
 *    a. Crear la cola del servidor (mq_open con O_CREAT). Configurar atributos attr.
 *    b. Bucle infinito (while 1):
 *       - mq_receive: Bloquearse esperando una petición.
 *       - Al recibir, reservar memoria para el mensaje o pasarlo por valor.
 *       - pthread_create: Lanzar un hilo trabajador pasándole el mensaje.
 *       - pthread_detach: Para no tener que esperar a que el hilo termine.
 *    c. Código de limpieza (cerrar colas) en caso de señal de terminación (opcional).
 */