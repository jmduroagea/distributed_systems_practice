#include "../../include/claves.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
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

// ------- RECEIVE -------

#define INBUF_SIZE 2048
typedef struct {
  char data[INBUF_SIZE];
  int read_pos;
  int write_pos;
} InBuf;

static InBuf proxy_inb = {0};

static int inb_read(int fd, InBuf *b, void *dest, size_t len) {
  // Reads 'len' bytes from socket into 'dest', using 'b' as intermediate
  // buffer
  uint8_t *dst = (uint8_t *)dest;
  size_t needed = len;

  while (needed > 0) {
    int available = b->write_pos - b->read_pos;
    if (available > 0) { // Buffer has data
      // Copy min(available, needed) bytes from buffer to dest
      size_t chunk = ((size_t)available < needed) ? (size_t)available : needed;
      memcpy(dst, b->data + b->read_pos, chunk);
      b->read_pos += chunk;
      dst += chunk;
      needed -= chunk;
      if (b->read_pos == b->write_pos) {
        b->read_pos = 0;
        b->write_pos = 0;
      }
    } else { // Buffer empty, read from socket
      b->read_pos = 0;
      b->write_pos = 0;
      int n = recv(fd, b->data, INBUF_SIZE, 0);
      if (n <= 0)
        return -1;
      b->write_pos = n;
    }
  }
  return 0;
}

static int recv_field(int fd, InBuf *b, char *buffer) {
  // Recibe un campo string: [4 bytes longitud big-endian][datos]
  uint32_t net_len;
  if (inb_read(fd, b, &net_len, 4) < 0)
    return -1;
  uint32_t len = ntohl(net_len);
  if (inb_read(fd, b, buffer, len) < 0)
    return -1;
  buffer[len] = '\0';
  return 0;
}

static int recv_floats(int fd, InBuf *b, float *arr, int n) {
  // Receives N floats serialized as big-endian uint32_t. We read them into a
  // temporary buffer of uint32_t, convert endianness, and then memcpy to float
  // array
  uint32_t buf[32]; // MAX_VEC_LEN = 32 => 128 bytes max
  if (inb_read(fd, b, buf, (size_t)n * sizeof(uint32_t)) < 0)
    return -1;
  for (int i = 0; i < n; i++) {
    buf[i] = ntohl(buf[i]);
    memcpy(&arr[i], &buf[i], sizeof(float));
  }
  return 0;
}

// ------- OUTGOING BUFFER -------
// Each operation is serialized into a single buffer and sent with one send()
// call, ensuring minimal latency and avoiding fragmentation. Maximum petition
// size:
//   SET/MODIFY: 1+4+255+4+255+4+32*4+4+4+4 = 663 bytes
//   GET/DELETE/EXIST: 1+4+255 = 260 bytes
#define OUTBUF_SIZE 768

typedef struct {
  uint8_t d[OUTBUF_SIZE];
  size_t n;
} OutBuf;

static inline void ob_u8(OutBuf *b, uint8_t v) { b->d[b->n++] = v; }
static inline void ob_u32(OutBuf *b, uint32_t v) {
  v = htonl(v);
  memcpy(b->d + b->n, &v, 4);
  b->n += 4;
}
static inline void ob_i32(OutBuf *b, int32_t v) { ob_u32(b, (uint32_t)v); }
static inline void ob_str(OutBuf *b, const char *s) {
  size_t len = strlen(s);
  ob_u32(b, (uint32_t)len);
  memcpy(b->d + b->n, s, len);
  b->n += len;
}
static inline void ob_floats(OutBuf *b, const float *arr, int n) {
  // Serializes N floats as big-endian uint32_t in a single buffer.
  for (int i = 0; i < n; i++) {
    uint32_t bits;
    memcpy(&bits, &arr[i], sizeof(float));
    ob_u32(b, bits); // htonl is called inside ob_u32
  }
}
static inline void ob_flush(int fd, const OutBuf *b) {
  send(fd, b->d, b->n, 0); // 1 syscall, 1 segment on the network
}

// ====================== INIT ======================

static void cleanup(void) { close(sockfd); }

static int init_proxy(void) {

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

  proxy_inb.read_pos = 0;
  proxy_inb.write_pos = 0;

  // ----- SOCKET OPTIONS -----
  // TCP_NODELAY: minimizes latency by disabling Nagle's algorithm.
  int nodelay = 1;
  setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  struct timeval tv = {30, 0};
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  atexit(cleanup);
  proxy_ready = 1;
  return 0;
}

// ====================== API ======================

int destroy(void) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  uint8_t op = OP_DESTROY; // 0x01
  send(sockfd, &op, 1, 0); // Send opcode

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (inb_read(sockfd, &proxy_inb, &result, 1) < 0)
    return -2;
  return result;
}

int set_value(char *key, char *value1, int N_value2, float *V_value2,
              struct Paquete value3) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;
  if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
    return -1;

  // ----- SEND REQUEST -----
  OutBuf b = {.n = 0};
  ob_u8(&b, OP_SET_VALUE); // 0x02

  // Key and value1
  ob_str(&b, key);
  ob_str(&b, value1);

  // value2
  ob_u32(&b, (uint32_t)N_value2);
  ob_floats(&b, V_value2, N_value2);

  // value3
  ob_i32(&b, value3.x);
  ob_i32(&b, value3.y);
  ob_i32(&b, value3.z);

  // Flush buffer to socket
  ob_flush(sockfd, &b);

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (inb_read(sockfd, &proxy_inb, &result, 1) < 0)
    return -2;
  return result;
}

int get_value(char *key, char *value1, int *N_value2, float *V_value2,
              struct Paquete *value3) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  // ----- SEND REQUEST -----
  OutBuf b = {.n = 0};
  ob_u8(&b, OP_GET_VALUE); // 0x03

  // Key
  ob_str(&b, key);
  ob_flush(sockfd, &b);

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (inb_read(sockfd, &proxy_inb, &result, 1) < 0)
    return -2;
  if (result != 0)
    return result;

  if (recv_field(sockfd, &proxy_inb, value1) < 0) // value1
    return -2;
  uint32_t n_net;
  if (inb_read(sockfd, &proxy_inb, &n_net, 4) < 0) // N_value2
    return -2;
  *N_value2 = (int)ntohl(n_net);
  if (recv_floats(sockfd, &proxy_inb, V_value2, *N_value2) < 0) // V_value2
    return -2;

  // value3
  int32_t x, y, z;
  if (inb_read(sockfd, &proxy_inb, &x, 4) < 0)
    return -2;
  if (inb_read(sockfd, &proxy_inb, &y, 4) < 0)
    return -2;
  if (inb_read(sockfd, &proxy_inb, &z, 4) < 0)
    return -2;
  value3->x = (int)ntohl(x);
  value3->y = (int)ntohl(y);
  value3->z = (int)ntohl(z);
  return 0;
}

int modify_value(char *key, char *value1, int N_value2, float *V_value2,
                 struct Paquete value3) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;
  if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
    return -1;

  // ----- SEND REQUEST -----
  OutBuf b = {.n = 0};
  ob_u8(&b, OP_MODIFY_VALUE); // 0x04

  // Key and value1
  ob_str(&b, key);
  ob_str(&b, value1);

  // value2
  ob_u32(&b, (uint32_t)N_value2);
  ob_floats(&b, V_value2, N_value2);

  // value3
  ob_i32(&b, value3.x);
  ob_i32(&b, value3.y);
  ob_i32(&b, value3.z);

  // Flush buffer to socket
  ob_flush(sockfd, &b);

  // ----- RECEIVE RESPONSE -----s
  int8_t result;
  if (inb_read(sockfd, &proxy_inb, &result, 1) < 0)
    return -2;
  return result;
}

int delete_key(char *key) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  // ----- SEND REQUEST -----
  OutBuf b = {.n = 0};
  ob_u8(&b, OP_DELETE_KEY); // 0x05
  ob_str(&b, key);          // Key
  ob_flush(sockfd, &b);

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (inb_read(sockfd, &proxy_inb, &result, 1) < 0)
    return -2;
  return result;
}

int exist(char *key) {
  if (!proxy_ready && init_proxy() != 0)
    return -2;

  // ----- SEND REQUEST -----
  OutBuf b = {.n = 0};
  ob_u8(&b, OP_EXIST); // 0x06
  ob_str(&b, key);     // Key
  ob_flush(sockfd, &b);

  // ----- RECEIVE RESPONSE -----
  int8_t result;
  if (inb_read(sockfd, &proxy_inb, &result, 1) < 0)
    return -2;
  return result;
}