/**
 * @file proxy-mq.c
 * @brief Proxy del cliente que usa Colas de Mensajes POSIX para comunicarse con
 *el servidor. El proxy implementa la API pública y traduce las llamadas a
 *peticiones que envía al servidor a través de colas de mensajes. Cada cliente
 *tiene dos colas de respuesta: una para respuestas simples (set, delete,
 *modify, exist) que devuelven 4B y otra para respuestas de get (que incluyen
 *datos).
 **/

#include "../../include/claves.h"
#include "../../include/mensajes.h"
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>

// =========================================================
// ESTADO GLOBAL DEL PROXY (Inicializado una vez por cliente)
// =========================================================
static mqd_t server_queue = (mqd_t)-1;
static mqd_t client_queue = (mqd_t)-1;
static char client_queue_name[64];
static int proxy_ready = 0;

// =========================================================
// LIMPIEZA DE RECURSOS AL FINALIZAR
// =========================================================
static void cleanup() {
  // Cerrar y eliminar colas de mensajes si están abiertas
  if (client_queue != (mqd_t)-1) {
    mq_close(client_queue);
    mq_unlink(client_queue_name);
    client_queue = (mqd_t)-1;
  }
  if (server_queue != (mqd_t)-1) {
    mq_close(server_queue);
    server_queue = (mqd_t)-1;
  }
}

// =========================================================
// INICIALIZACIÓN DEL PROXY (Se realiza una vez por cliente)
// =========================================================
static int init_proxy() {
  // Si el proxy ya está listo, no hacer nada
  if (proxy_ready)
    return 0;

  atexit(cleanup); // Registrar función de limpieza al salir

  snprintf(client_queue_name, sizeof(client_queue_name),
           "/cq_%d", getpid());

  // Crear la cola del cliente con atributos adecuados
  struct mq_attr attr_get = {
      .mq_flags = 0,
      .mq_maxmsg = 1,
      .mq_msgsize = RESPUESTA_MAX_SIZE, // solo el tamaño máximo real
      .mq_curmsgs = 0};

  // Crear la cola del cliente
  int tries = 0;
  // Intentar abrir la cola del cliente, con reintentos en caso de EMFILE/ENFILE
  while (tries < 10) {
    client_queue = mq_open(client_queue_name, O_CREAT | O_RDONLY, 0666, &attr_get);
    if (client_queue != (mqd_t)-1)
      break; // Éxito
    if (errno == EMFILE || errno == ENFILE) {
      // Demasiados archivos abiertos, esperar un poco y reintentar
      srand(time(0) ^ getpid());
      usleep(rand() % 1000000); // Jitter sleep entre 0 y 1 segundo
      tries++;
    } else {
      perror("init_proxy: mq_open cliente");
      return -2;
    }
  }

  if(tries == 5) {
    fprintf(stderr, "[%ld] Fallo en intento %d: errno=%d\n", 
        time(NULL), tries, errno);
    return -2;
  }

  // Abrir la cola del servidor para enviar peticiones
  server_queue = mq_open(SERVER_QUEUE, O_WRONLY);
  if (server_queue == (mqd_t)-1) {
    perror("init_proxy: mq_open servidor");
    mq_close(client_queue);
    mq_unlink(client_queue_name);
    client_queue = (mqd_t)-1;
    return -2;
  }

  proxy_ready = 1;
  return 0;
}

__attribute__((constructor)) static void proxy_constructor() { init_proxy(); }

// =========================================================
// HELPERS PARA ENVIAR Y RECIBIR RESPUESTAS
// =========================================================
static int enviar_y_recibir(Peticion *p, void *recv_buf, size_t recv_size) {
  // Asegurar que el proxy esté inicializado
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  // send_size: solo los bytes necesarios según la operación
  // Asi eliminamos la necesidad de zeroing Peticion antes de enviar
  size_t send_size = PETICION_SIZE(p->op_code);

  // Enviar la petición al servidor
  if (mq_send(server_queue, (const char *)p, send_size, 0) == -1) {
    perror("enviar_y_recibir: mq_send");
    return -2;
  }

  // Esperar por la respuesta en la cola del cliente
  if (mq_receive(client_queue, (char *)recv_buf, RESPUESTA_MAX_SIZE,
                   NULL) == -1) {
      perror("enviar_y_recibir: mq_receive_get");
      return -2;
    }

  return 0;
}

// Macro para rellenar el q_name sin llamar a strncpy cada vez
#define SET_QNAME(p)                                                           \
  memcpy((p)->q_name, client_queue_name, sizeof(client_queue_name))

// =========================================================
// API ENDPOINTS (Implementación de las funciones públicas)
// =========================================================

int destroy(void) {
  // Solo necesitamos enviar la petición, no hay payload
  Peticion p;
  p.op_code = OP_INIT;
  SET_QNAME(&p);
  RespSimple r;
  // Enviar la petición y esperar la respuesta
  if (enviar_y_recibir(&p, &r, sizeof(r)) != 0)
    // Si hubo un error en la comunicación, devolver -2
    return -2;
  return r.result;
}

int set_value(char *key, char *value1, int N_value2, float *V_value2,
              struct Paquete value3) {
  // Validar los parámetros antes de enviar la petición
  if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
    return -1;

  Peticion p;
  p.op_code = OP_SET;
  SET_QNAME(&p);
  // Solo copiar los bytes de la key que realmente tiene contenido
  strncpy(p.payload.escritura.key, key, MAX_KEY_LEN - 1);
  strncpy(p.payload.escritura.value1, value1, MAX_VAL1_LEN - 1);
  p.payload.escritura.N_value2 = N_value2;
  memcpy(p.payload.escritura.V_value2, V_value2, N_value2 * sizeof(float));
  p.payload.escritura.value3 = value3;

  RespSimple r;
  // Enviar la petición y esperar la respuesta
  if (enviar_y_recibir(&p, &r, sizeof(r)) != 0)
    return -2;
  return r.result;
}

int get_value(char *key, char *value1, int *N_value2, float *V_value2,
              struct Paquete *value3) {
  Peticion p;
  p.op_code = OP_GET;
  SET_QNAME(&p);

  strncpy(p.payload.lectura.key, key, MAX_KEY_LEN - 1);

  RespGet r;

  // Enviar la petición y esperar la respuesta
  if (enviar_y_recibir(&p, &r, sizeof(r)) != 0)
    return -2;

  if (r.result == 0) {
    // Copiar solo los bytes necesarios según el tamaño real de value1 y
    // N_value2
    strncpy(value1, r.value1, MAX_VAL1_LEN - 1);
    *N_value2 = r.N_value2;
    memcpy(V_value2, r.V_value2, r.N_value2 * sizeof(float)); //
    *value3 = r.value3;
  }
  return r.result;
}

int modify_value(char *key, char *value1, int N_value2, float *V_value2,
                 struct Paquete value3) {
  // Validar los parámetros antes de enviar la petición
  if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
    return -1;

  Peticion p;
  p.op_code = OP_MODIFY;
  SET_QNAME(&p);
  strncpy(p.payload.escritura.key, key, MAX_KEY_LEN - 1);
  strncpy(p.payload.escritura.value1, value1, MAX_VAL1_LEN - 1);
  p.payload.escritura.N_value2 = N_value2;
  memcpy(p.payload.escritura.V_value2, V_value2, N_value2 * sizeof(float));
  p.payload.escritura.value3 = value3;

  RespSimple r;

  // Enviar la petición y esperar la respuesta
  if (enviar_y_recibir(&p, &r, sizeof(r)) != 0)
    return -2;
  return r.result;
}

int delete_key(char *key) {
  Peticion p;
  p.op_code = OP_DELETE;
  SET_QNAME(&p);
  strncpy(p.payload.lectura.key, key, MAX_KEY_LEN - 1);

  RespSimple r;
  if (enviar_y_recibir(&p, &r, sizeof(r)) != 0)
    return -2;
  return r.result;
}

int exist(char *key) {
  // Solo necesitamos enviar la clave para verificar su existencia
  Peticion p;
  p.op_code = OP_EXIST;
  SET_QNAME(&p);
  strncpy(p.payload.lectura.key, key, MAX_KEY_LEN - 1);

  RespSimple r;

  // Enviar la petición y esperar la respuesta
  if (enviar_y_recibir(&p, &r, sizeof(r)) != 0)
    return -2;
  return r.result;
}