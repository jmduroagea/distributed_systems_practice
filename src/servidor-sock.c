#include "../include/claves.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define OP_DESTROY 0x01
#define OP_SET_VALUE 0x02
#define OP_GET_VALUE 0x03
#define OP_MODIFY_VALUE 0x04
#define OP_DELETE_KEY 0x05
#define OP_EXIST 0x06

#define MAX_KEY_LEN 256
#define MAX_VAL1_LEN 256
#define MAX_VEC_LEN 32

#define DEBUG_MODE 0 // Change to 1 to enable debug logs

#if DEBUG_MODE
#define LOG_INFO(...)                                                          \
  {                                                                            \
    printf("[INFO] " __VA_ARGS__);                                             \
    fflush(stdout);                                                            \
  }
#define LOG_ERR(...)                                                           \
  {                                                                            \
    fprintf(stderr, "[ERR] " __VA_ARGS__);                                     \
    fflush(stderr);                                                            \
  }
#else
#define LOG_INFO(...)
#define LOG_ERR(...)
#endif

static int server_running = 1;

// ====================== HELPERS ======================

static int recv_all(int fd, void *buf, size_t len) {
  // Receive exactly 'len' bytes into 'buf', handling partial reads.
  size_t total = 0;
  char *ptr = (char *)buf;
  while (total < len) {
    int n = recv(fd, ptr + total, len - total, 0);
    if (n <= 0)
      return -1;
    total += n;
  }
  return 0;
}

static void send_field(int fd, const char *data) {
  // Send a string field as: [4-byte length][data]
  uint32_t len = htonl(strlen(data));
  send(fd, &len, 4, 0);
  send(fd, data, strlen(data), 0);
}

static int recv_field(int fd, char *buffer) {
  // Receive a string field sent as: [4-byte length][data]
  uint32_t net_len;
  if (recv_all(fd, &net_len, 4) < 0)
    return -1;
  uint32_t len = ntohl(net_len);
  if (recv_all(fd, buffer, len) < 0)
    return -1;
  buffer[len] = '\0';
  return 0;
}

// ====================== WORKER ======================

void *worker_thread(void *arg) {

  // ----- INITIALIZATION -----
  int client_fd = *(int *)arg;
  free(arg);
  LOG_INFO("Worker %d iniciado.\n", client_fd);

  // ----- TIMEOUT -----
  struct timeval tv = {30, 0};
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // ----- RECEIVE REQUESTS -----
  while (server_running) {
    uint8_t op_code; // Read operation code
    int n = recv(
        client_fd, &op_code, 1,
        0); // This will block until a request is received or timeout occurs
    if (n <= 0)
      break;

    switch (op_code) {

    case OP_DESTROY: { // 0x01
      // ----- PROCESS REQUEST -----
      int8_t res = (int8_t)destroy();
      LOG_INFO("Worker %d: destroy() = %d\n", client_fd, res);

      // ----- SEND RESPONSE -----
      send(client_fd, &res, 1, 0);
      break;
    }

    case OP_SET_VALUE:
    case OP_MODIFY_VALUE: { // 0x02 or 0x04
      // ----- RECEIVE REQUEST FIELDS -----
      char key[MAX_KEY_LEN];
      char value1[MAX_VAL1_LEN];
      int N_value2;
      float V_value2[MAX_VEC_LEN];
      struct Paquete value3;

      // Read key and value1
      if (recv_field(client_fd, key) < 0)
        goto disconnect;
      if (recv_field(client_fd, value1) < 0)
        goto disconnect;

      // Read value2 array
      uint32_t n_net;
      if (recv_all(client_fd, &n_net, 4) < 0)
        goto disconnect;
      N_value2 = (int)ntohl(n_net);

      if (recv_all(client_fd, V_value2, N_value2 * sizeof(float)) < 0)
        goto disconnect;

      // Read value3 struct
      int32_t x, y, z;
      if (recv_all(client_fd, &x, 4) < 0)
        goto disconnect;
      if (recv_all(client_fd, &y, 4) < 0)
        goto disconnect;
      if (recv_all(client_fd, &z, 4) < 0)
        goto disconnect;
      value3.x = (int)ntohl(x);
      value3.y = (int)ntohl(y);
      value3.z = (int)ntohl(z);

      // ----- PROCESS REQUEST -----
      int res = (op_code == OP_MODIFY_VALUE)
                    ? modify_value(key, value1, N_value2, V_value2, value3)
                    : set_value(key, value1, N_value2, V_value2, value3);

      LOG_INFO("Worker %d: %s('%s') = %d\n", client_fd,
               op_code == OP_MODIFY_VALUE ? "modify_value" : "set_value", key,
               res);

      // ----- SEND RESPONSE -----
      int8_t r = (int8_t)res;
      send(client_fd, &r, 1, 0);
      break;
    }

    case OP_GET_VALUE: { // 0x03
      // ----- RECEIVE REQUEST FIELDS -----
      char key[MAX_KEY_LEN];
      char value1[MAX_VAL1_LEN];
      int N_value2;
      float V_value2[MAX_VEC_LEN];
      struct Paquete value3;

      // Read key
      if (recv_field(client_fd, key) < 0)
        goto disconnect;

      // ----- PROCESS REQUEST -----
      int res = get_value(key, value1, &N_value2, V_value2, &value3);
      LOG_INFO("Worker %d: get_value('%s') = %d\n", client_fd, key, res);

      // ----- SEND RESPONSE -----
      int8_t r = (int8_t)res;
      send(client_fd, &r, 1, 0);

      if (res == 0) { // Only send values if get_value was successful
        send_field(client_fd, value1);

        uint32_t n_net = htonl((uint32_t)N_value2);
        send(client_fd, &n_net, 4, 0);
        send(client_fd, V_value2, N_value2 * sizeof(float), 0);

        int32_t x = htonl(value3.x);
        int32_t y = htonl(value3.y);
        int32_t z = htonl(value3.z);
        send(client_fd, &x, 4, 0);
        send(client_fd, &y, 4, 0);
        send(client_fd, &z, 4, 0);
      }
      break;
    }

    case OP_DELETE_KEY: { // 0x05
      // Receive key field
      char key[MAX_KEY_LEN];
      if (recv_field(client_fd, key) < 0)
        goto disconnect;

      // ----- PROCESS REQUEST -----
      int res = delete_key(key);
      LOG_INFO("Worker %d: delete_key('%s') = %d\n", client_fd, key, res);

      // ----- SEND RESPONSE -----
      int8_t r = (int8_t)res;
      send(client_fd, &r, 1, 0);
      break;
    }

    case OP_EXIST: { // 0x06
      // Receive key field
      char key[MAX_KEY_LEN];
      if (recv_field(client_fd, key) < 0)
        goto disconnect;

      // ----- PROCESS REQUEST -----
      int res = exist(key);
      LOG_INFO("Worker %d: exist('%s') = %d\n", client_fd, key, res);

      // ----- SEND RESPONSE -----
      int8_t r = (int8_t)res;
      send(client_fd, &r, 1, 0);
      break;
    }

    default:
      // Unknown opcode
      LOG_ERR("Opcode desconocido: 0x%02x\n", op_code);
      goto disconnect;
    }
  }

disconnect:
  close(client_fd);
  LOG_INFO("Worker %d finalizando.\n", client_fd);
  return NULL;
}

// ====================== MAIN ======================

int main(int argc, char **argv) {
  if (argc != 2) { // Check for port argument
    fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
    return 1;
  }

  int port = atoi(argv[1]);

  // ----- CREATE SERVER SOCKET -----
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket");
    return 1;
  }

  // Allow reuse of the address
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // ----- BIND -----
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    return 1;
  }

  // ----- LISTEN -----
  if (listen(server_fd, 10) < 0) {
    perror("listen");
    return 1;
  }

  destroy(); // Init hash table before accepting clients
  printf("Servidor escuchando en el puerto %d\n", port);

  // ----- ACCEPT CLIENTS -----
  while (server_running) {
    int *client_fd = malloc(sizeof(int)); // Allocate on heap to pass to thread
    *client_fd = accept(server_fd, NULL, NULL);
    if (*client_fd < 0) {
      free(client_fd);
      continue;
    }

    pthread_t thread;
    pthread_create(&thread, NULL, worker_thread, client_fd);
    pthread_detach(thread);
  }

  close(server_fd);
  return 0;
}