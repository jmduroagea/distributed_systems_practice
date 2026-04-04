/**
 * @file servidor-mq.c
 * @brief Servidor concurrente usando Colas de Mensajes POSIX y un Pool de Hilos
 *        Simétrico.
 */

#include <errno.h>
#include <mqueue.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../include/claves.h"
#include "../../include/mensajes.h"

#define DEBUG_MODE 0 // Set to 1 to enable logging

#if DEBUG_MODE
#define LOG_INFO(...)                                                          \
  {                                                                            \
    printf("[INFO] " __VA_ARGS__);                                             \
    fflush(stdout);                                                            \
  }
#define LOG_ERR(...)                                                           \
  {                                                                            \
    fprintf(stderr, "[ERROR] " __VA_ARGS__);                                   \
    fflush(stderr);                                                            \
  }
#else
#define LOG_INFO(...)
#define LOG_ERR(...)
#endif

mqd_t q_servidor = (mqd_t)-1;
atomic_int server_running = 1;

/* ========================================================================= *
 * SEÑALES
 * ========================================================================= */
void handle_sigint(int sig) {
  (void)sig;
  server_running = 0;
  if (q_servidor != (mqd_t)-1) {
    mq_close(q_servidor);
    mq_unlink(SERVER_QUEUE);
    q_servidor = (mqd_t)-1;
  }
  exit(0);
}

/* ========================================================================= *
 * HELPERS
 * ========================================================================= */
long get_max_queue_depth() {
  FILE *f = fopen("/proc/sys/fs/mqueue/msg_max", "r");
  long max_msg = 10;
  if (f) {
    fscanf(f, "%ld", &max_msg);
    fclose(f);
  }
  return max_msg;
}

int send_response(mqd_t q, const void *response, size_t response_size) {
  struct timespec timeout;
  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_nsec += 50000000; // 50ms timeout
  if (timeout.tv_nsec >= 1000000000) {
    timeout.tv_sec++;
    timeout.tv_nsec -= 1000000000;
  }

  if (mq_timedsend(q, (const char *)response, response_size, 0, &timeout) ==
      -1) {
    perror("send_response: mq_timedsend");
    return -1;
  }
  return 0;
}

// Envía RespSimple (4 bytes) — para destroy, set, modify, delete, exist
static void responder_simple(const char *q_name, int result) {
  mqd_t q = mq_open(q_name, O_WRONLY);
  if (q == (mqd_t)-1) {
    LOG_ERR("Cola cliente %s no existe.\n", q_name);
    return;
  }
  RespSimple r = {.result = result};

  if (send_response(q, &r, sizeof(r)) == -1)
    LOG_ERR("Failed to send RespSimple to %s\n", q_name);

  mq_close(q);
}

// Envía RespGet (404 bytes) — solo para get_value
static void responder_get(const char *q_name, RespGet *r) {
  mqd_t q = mq_open(q_name, O_WRONLY);
  if (q == (mqd_t)-1) {
    LOG_ERR("Cola cliente %s no existe.\n", q_name);
    return;
  }

  if (send_response(q, r, sizeof(RespGet)) == -1)
    LOG_ERR("Failed to send RespGet to %s\n", q_name);

  mq_close(q);
}

/* ========================================================================= *
 * HILO TRABAJADOR
 * ========================================================================= */
void *worker_thread(void *arg) {
  int thread_id = *(int *)arg;
  free(arg); // Liberar memoria del ID
  (void)thread_id;

  // Buffer de recepción del tamaño máximo posible
  char buf[sizeof(Peticion)];
  unsigned int prio;

  LOG_INFO("Worker %d iniciado.\n", thread_id);

  while (server_running) {
    // Esperar por una petición (bloqueante)
    ssize_t bytes = mq_receive(q_servidor, buf, sizeof(Peticion), &prio);

    if (bytes == -1) {
      if (errno == EBADF || errno == EINVAL)
        break; // Cola cerrada o inválida, salir del loop
      continue;
    }

    // Obtener la petición del buffer
    Peticion *req = (Peticion *)buf;

    // Procesar la petición según su op_code
    switch (req->op_code) {

    case OP_INIT: {
      int res =
          destroy(); // Reutilizamos destroy() para limpiar la tabla al iniciar
      LOG_INFO("Worker %d: destroy() = %d\n", thread_id, res);
      responder_simple(req->q_name, res);
      break;
    }

    case OP_SET: {
      int res = set_value(
          req->payload.escritura.key, req->payload.escritura.value1,
          req->payload.escritura.N_value2, req->payload.escritura.V_value2,
          req->payload.escritura.value3); // Rellenamos el payload de escritura
      LOG_INFO("Worker %d: set_value('%s') = %d\n", thread_id,
               req->payload.escritura.key, res);
      responder_simple(req->q_name, res);
      break;
    }

    case OP_GET: {
      RespGet r; // Estructura para la respuesta del get_value
      // Pasamos las variables por referencia para que get_value las rellene
      r.result = get_value(req->payload.lectura.key, r.value1, &r.N_value2,
                           r.V_value2, &r.value3);
      LOG_INFO("Worker %d: get_value('%s') = %d\n", thread_id,
               req->payload.lectura.key, r.result);
      responder_get(req->q_name, &r);
      break;
    }

    case OP_MODIFY: {
      int res = modify_value(
          req->payload.escritura.key, req->payload.escritura.value1,
          req->payload.escritura.N_value2, req->payload.escritura.V_value2,
          req->payload.escritura.value3); // Rellenamos el payload de escritura
      LOG_INFO("Worker %d: modify_value('%s') = %d\n", thread_id,
               req->payload.escritura.key, res);
      responder_simple(req->q_name, res);
      break;
    }

    case OP_DELETE: {
      int res = delete_key(req->payload.lectura.key); // Borra la clave
      LOG_INFO("Worker %d: delete_key('%s') = %d\n", thread_id,
               req->payload.lectura.key, res);
      responder_simple(req->q_name, res);
      break;
    }

    case OP_EXIST: {
      int res = exist(req->payload.lectura.key); // Determina si la clave existe
      LOG_INFO("Worker %d: exist('%s') = %d\n", thread_id,
               req->payload.lectura.key, res);
      responder_simple(req->q_name, res);
      break;
    }

    default:
      // Código de operación desconocido
      LOG_ERR("Worker %d: op_code desconocido %d\n", thread_id, req->op_code);
      responder_simple(req->q_name, -1);
      break;
    }
  }

  LOG_INFO("Worker %d finalizando.\n", thread_id);
  return NULL;
}

/* ========================================================================= *
 * MAIN
 * ========================================================================= */
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  // 1. Señales
  struct sigaction sa;
  sa.sa_handler = handle_sigint;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  // 2. Limpieza preventiva (por si el servidor crasheó previamente)
  mq_unlink(SERVER_QUEUE);

  // 3. Cola del servidor
  struct mq_attr attr = {.mq_flags = 0,
                         .mq_maxmsg = 1,
                         .mq_msgsize =
                             sizeof(Peticion), // máximo posible de entrada
                         .mq_curmsgs = 0};

  // Crear la cola del servidor
  q_servidor = mq_open(SERVER_QUEUE, O_CREAT | O_RDONLY, 0666, &attr);

  if (q_servidor == (mqd_t)-1) {
    perror("Error al crear la cola del servidor");
    exit(EXIT_FAILURE);
  }
  LOG_INFO("Cola servidor creada: %s (maxmsg=%ld, msgsize=%zu)\n", SERVER_QUEUE,
           max_msgs, sizeof(Peticion));

  // 4. Inicializar tabla de datos
  if (destroy() == -1)
    LOG_ERR("Fallo al inicializar la estructura de datos.\n");

  // 5. Symmetric Thread pool
  long num_cores =
      sysconf(_SC_NPROCESSORS_ONLN); // Inspecta núcleos disponibles
  if (num_cores < 1)
    // Fallback a 4 si no se pudo determinar el número de núcleos
    num_cores = 4;
  LOG_INFO("Iniciando %ld workers...\n", num_cores);

  // Crear un pool de hilos igual al número de núcleos disponibles
  pthread_t workers[num_cores];
  for (int i = 0; i < num_cores; i++) {
    int *id = malloc(sizeof(int));
    *id = i + 1;
    if (pthread_create(&workers[i], NULL, worker_thread, id) != 0)
      perror("Error al crear hilo trabajador");
  }

  LOG_INFO("Servidor listo (Ctrl+C para apagar)\n");

  while (server_running)
    // El proceso principal solo espera señales para apagar el servidor
    pause();

  // Esperar a que los workers terminen
  for (int i = 0; i < num_cores; i++)
    pthread_join(workers[i], NULL);

  return 0;
}