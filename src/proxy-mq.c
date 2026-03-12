#include "../include/claves.h"
#include "../include/mensajes.h"
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
mqd_t server_queue = -1;
mqd_t client_queue = -1;
char client_queue_name[256];

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

static void cleanup() {
  if (client_queue != -1) {
    mq_close(client_queue);
    mq_unlink(client_queue_name);
  }
  if (server_queue != -1) {
    mq_close(server_queue);
  }
}

// ======================== INICIALIZACIÓN DEL PROXY ========================

static int init_proxy() {
  /**
      @brief Incializa la cola del cliente y la cola del servidor. Devuelve 0 en
   caso de éxito y -2 en caso de error.
      @return int Devuelve 0 en caso de éxito y -2 en caso de error
   **/
  atexit(cleanup);
  if (client_queue != -1) { // Ya inicializado
    return 0;
  }

  // Crear nombre de cola único para el cliente
  snprintf(client_queue_name, sizeof(client_queue_name), "/client_queue_%d",
           getpid());

  // Atributos de la cola del cliente
  struct mq_attr attr = {.mq_flags = 0,
                         .mq_maxmsg = 10,
                         .mq_msgsize = sizeof(Respuesta),
                         .mq_curmsgs = 0};

  // Crear la cola del cliente
  client_queue = mq_open(client_queue_name, O_CREAT | O_RDONLY, 0666, &attr);

  if (client_queue == -1) {
    perror("init_proxy: mq_open cliente");
    return -2;
  }

  // Abrir la cola del servidor
  server_queue = mq_open(SERVER_QUEUE, O_WRONLY);

  if (server_queue == -1) {
    perror("init_proxy: mq_open servidor");
    mq_close(client_queue);
    mq_unlink(client_queue_name);
    return -2;
  }

  return 0;
}

// ======================== FUNCIONES DE ENVÍO Y RECEPCIÓN
// ========================

static int enviar_y_recibir(Peticion *p, Respuesta *r) {
  if (init_proxy() != 0) { // Asegurar que las colas están inicializadas
    return -2;
  }

  // Copiar el nombre de la cola del cliente en la petición
  strncpy(p->q_name, client_queue_name, sizeof(p->q_name) - 1);
  p->q_name[sizeof(p->q_name) - 1] = '\0'; // Asegurar null-termination

  if (mq_send(server_queue, (char *)p, sizeof(Peticion), 0) == -1) {
    perror("enviar_y_recibir: mq_send");
    return -2;
  }

  if (mq_receive(client_queue, (char *)r, sizeof(Respuesta), NULL) == -1) {
    perror("enviar_y_recibir: mq_receive");
    return -2;
  }

  return 0;
}

// ======================== FUNCIONES DE LA API PROXY ========================

int destroy(void) {
  Peticion p = {0};
  Respuesta r = {0};
  p.op_code = OP_INIT;

  if (enviar_y_recibir(&p, &r) != 0)
    return -2;
  return r.result;
}

int set_value(char *key, char *value1, int N_value2, float *V_value2,
              struct Paquete value3) {

  if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
    return -1;

  Peticion p = {0};
  Respuesta r = {0};
  p.op_code = OP_SET;
  strncpy(p.payload.escritura.key, key, sizeof(p.payload.escritura.key) - 1);
  strncpy(p.payload.escritura.value1, value1,
          sizeof(p.payload.escritura.value1) - 1);
  p.payload.escritura.N_value2 = N_value2;
  memcpy(p.payload.escritura.V_value2, V_value2, N_value2 * sizeof(float));
  p.payload.escritura.value3 = value3;

  if (enviar_y_recibir(&p, &r) != 0)
    return -2;
  return r.result;
}

int get_value(char *key, char *value1, int *N_value2, float *V_value2,
              struct Paquete *value3) {

  Peticion p = {0};
  Respuesta r = {0};
  p.op_code = OP_GET;
  strncpy(p.payload.lectura.key, key, sizeof(p.payload.lectura.key) - 1);

  if (enviar_y_recibir(&p, &r) != 0)
    return -2;

  if (r.result == 0) {
    strncpy(value1, r.value1, 255);
    *N_value2 = r.N_value2;
    memcpy(V_value2, r.V_value2, r.N_value2 * sizeof(float));
    *value3 = r.value3;
  }

  return r.result;
}

int modify_value(char *key, char *value1, int N_value2, float *V_value2,
                 struct Paquete value3) {

  if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
    return -1;

  Peticion p = {0};
  Respuesta r = {0};
  p.op_code = OP_MODIFY;
  strncpy(p.payload.escritura.key, key, sizeof(p.payload.escritura.key) - 1);
  strncpy(p.payload.escritura.value1, value1,
          sizeof(p.payload.escritura.value1) - 1);
  p.payload.escritura.N_value2 = N_value2;
  memcpy(p.payload.escritura.V_value2, V_value2, N_value2 * sizeof(float));
  p.payload.escritura.value3 = value3;

  if (enviar_y_recibir(&p, &r) != 0)
    return -2;
  return r.result;
}

int delete_key(char *key) {
  Peticion p = {0};
  Respuesta r = {0};
  p.op_code = OP_DELETE;
  strncpy(p.payload.lectura.key, key, sizeof(p.payload.lectura.key) - 1);

  if (enviar_y_recibir(&p, &r) != 0)
    return -2;
  return r.result;
}

int exist(char *key) {
  Peticion p = {0};
  Respuesta r = {0};
  p.op_code = OP_EXIST;
  strncpy(p.payload.lectura.key, key, sizeof(p.payload.lectura.key) - 1);

  if (enviar_y_recibir(&p, &r) != 0)
    return -2;
  return r.result;
}