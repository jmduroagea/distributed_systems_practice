#include "claves.h"
#include "mensajes.h"
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
// ... otros includes (string.h, unistd.h para getpid, etc.)

/*
 * IMPLEMENTACIÓN DEL CLIENTE DISTRIBUIDO (PROXY).
 * Este archivo implementa la misma API que 'claves.c' pero NO guarda datos.
 * Su función es empaquetar peticiones y enviarlas al servidor.
 * Se compilará como 'libproxyclaves.so'.
 */

/* 
 * 1. VARIABLES GLOBALES (Privadas al proxy)
 * - Descriptor de la cola del servidor (mqd_t).
 * - Descriptor de la cola de respuesta del cliente (mqd_t).
 * - Nombre de la cola del cliente (basado en PID).
 */

/*
 * 2. FUNCIONES AUXILIARES
 * - Una función para inicializar las colas si es la primera vez que se llama a la API.
 *   Debe generar un nombre único (/cola_cliente_<pid>), crearla y abrir la del servidor.
 */

/* 
 * 3. IMPLEMENTACIÓN DE LAS FUNCIONES DE LA API
 * Ejemplo para set_value(...):
 *    a. Asegurar que las colas están abiertas.
 *    b. Crear una instancia de 'struct Peticion'.
 *    c. Rellenar 'op_code' = OP_SET.
 *    d. Copiar los parámetros (key, values...) en la estructura.
 *    e. Enviar mensaje (mq_send) a la cola del servidor.
 *    f. Esperar respuesta (mq_receive) en la cola del cliente.
 *    g. Analizar 'struct Respuesta' y devolver el código de retorno.
 *    h. Manejar errores: Si falla el envío/recepción, devolver -2.
 */