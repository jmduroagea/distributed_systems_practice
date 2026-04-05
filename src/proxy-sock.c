#include "../include/claves.h"
#include <arpa/inet.h>
#include <netdb.h>
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

static int proxy_ready = 0;
static int sockfd = -1;

// ====================== HELPERS ======================

static int recv_all(int fd, void *buf, size_t len) {
  // Receive exactly 'len' bytes into 'buf', handling partial reads
  // This is crucial for TCP to ensure we get the full message as sent by the
  // server
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
  send(fd, &len, sizeof(len), 0);
  send(fd, data, strlen(data), 0);
}

static void recv_field(int fd, char *buffer) {
  // Receive a string field sent as: [4-byte length][data]
  uint32_t net_len;
  recv_all(fd, &net_len, 4);
  uint32_t len = ntohl(net_len);
  recv_all(fd, buffer, len);
  buffer[len] = '\0';
}

// ====================== INIT ======================

static void cleanup() { close(sockfd); }

static int init_proxy() {

  // ----- VALIDATIONS -----
  if (proxy_ready)
    return 0;

  char *ip = getenv("IP_TUPLAS");
  char *port = getenv("PORT_TUPLAS");

  if (!ip || !port) {
    fprintf(stderr, "ERROR: IP_TUPLAS o PORT_TUPLAS no definidas\n");
    return -2;
  }

  // ----- CREATE SOCKET -----

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return -2;
  }

  // ----- CONNECT -----
  struct addrinfo hints = {0}, *res;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(ip, port, &hints, &res) != 0) {
    perror("getaddrinfo");
    return -2;
  }

  if (connect(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
    perror("connect");
    freeaddrinfo(res);
    return -2;
  }
  freeaddrinfo(res);

  // ----- SET 30sec TIMEOUT -----
  struct timeval tv = {30, 0};
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Register cleanup at exit
  atexit(cleanup);
  proxy_ready = 1;
  return 0;
}

// ====================== API ======================

int destroy(void) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  uint8_t op = OP_DESTROY; // 0x01
  send(sockfd, &op, 1, 0);

  int8_t result;
  if (recv_all(sockfd, &result, 1) < 0)
    return -2;
  return result;
}

int set_value(char *key, char *value1, int N_value2, float *V_value2,
              struct Paquete value3) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  // ----- VALIDATIONS -----
  if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
    return -1;

  // ----- SEND REQUEST -----
  uint8_t op = OP_SET_VALUE; // 0x02
  send(sockfd, &op, 1, 0);   // Operation code

  // Key and value1
  send_field(sockfd, key);
  send_field(sockfd, value1);

  // value2 array
  uint32_t n_net = htonl((uint32_t)N_value2);
  send(sockfd, &n_net, 4, 0);
  send(sockfd, V_value2, N_value2 * sizeof(float), 0);

  // value3 struct
  int32_t x = htonl(value3.x), y = htonl(value3.y), z = htonl(value3.z);
  send(sockfd, &x, 4, 0);
  send(sockfd, &y, 4, 0);
  send(sockfd, &z, 4, 0);

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (recv_all(sockfd, &result, 1) < 0)
    return -2;
  return result;
}

int get_value(char *key, char *value1, int *N_value2, float *V_value2,
              struct Paquete *value3) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  uint8_t op = OP_GET_VALUE; // 0x03
  send(sockfd, &op, 1, 0);   // Send operation code
  send_field(sockfd, key);   // Send key

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (recv_all(sockfd, &result, 1) < 0)
    return -2;
  // We use recv_all to ensure we get the full response byte, which indicates
  // success or error.
  if (result != 0)
    return result; // No error, but key not found or other issue

  // value1
  recv_field(sockfd, value1);

  // value2 array
  uint32_t n_net;
  recv_all(sockfd, &n_net, 4);
  *N_value2 = (int)ntohl(n_net);
  recv_all(sockfd, V_value2, *N_value2 * sizeof(float));

  // value3 struct
  int32_t x, y, z;
  recv_all(sockfd, &x, 4);
  recv_all(sockfd, &y, 4);
  recv_all(sockfd, &z, 4);
  value3->x = (int)ntohl(x);
  value3->y = (int)ntohl(y);
  value3->z = (int)ntohl(z);

  return 0;
}

int modify_value(char *key, char *value1, int N_value2, float *V_value2,
                 struct Paquete value3) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  // ----- VALIDATIONS -----
  if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
    return -1;

  // ----- SEND REQUEST -----
  uint8_t op = OP_MODIFY_VALUE; // 0x04
  send(sockfd, &op, 1, 0);      // Operation code

  // Key and value1
  send_field(sockfd, key);
  send_field(sockfd, value1);

  // value2 array
  uint32_t n_net = htonl((uint32_t)N_value2);
  send(sockfd, &n_net, 4, 0);
  send(sockfd, V_value2, N_value2 * sizeof(float), 0);

  // value3 struct
  int32_t x = htonl(value3.x), y = htonl(value3.y), z = htonl(value3.z);
  send(sockfd, &x, 4, 0);
  send(sockfd, &y, 4, 0);
  send(sockfd, &z, 4, 0);

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (recv_all(sockfd, &result, 1) < 0)
    return -2;
  return result;
}

int delete_key(char *key) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  uint8_t op = OP_DELETE_KEY; // 0x05
  send(sockfd, &op, 1, 0); // Send operation code
  send_field(sockfd, key); // Send key

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (recv_all(sockfd, &result, 1) < 0)
    return -2;
  return result;
}

int exist(char *key) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  uint8_t op = OP_EXIST; // 0x06
  send(sockfd, &op, 1, 0); // Send operation code
  send_field(sockfd, key); // Send key

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (recv_all(sockfd, &result, 1) < 0)
    return -2;
  return result;
}