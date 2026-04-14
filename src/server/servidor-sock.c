#include "../../include/claves.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
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

// Atomic flag to control server shutdown from signal handlers. Workers check
// this flag to exit gracefully
static volatile sig_atomic_t server_running = 1;

static void handle_signal(int sig) {
  (void)sig;
  server_running = 0;
}

// ====================== HELPERS ======================

// ------- WORK QUEUE -------

#define QUEUE_SIZE 256

typedef struct {
  int fds[QUEUE_SIZE]; // Circular buffer of client file descriptors
  int head;
  int tail;
  int count;
  pthread_mutex_t lock;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
} WorkQueue;

static WorkQueue wq = {
    // Initialize work queue
    .head = 0,
    .tail = 0,
    .count = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .not_empty = PTHREAD_COND_INITIALIZER,
    .not_full = PTHREAD_COND_INITIALIZER,
};

static void wq_push(int fd) {
  // Add a client fd to the work queue
  pthread_mutex_lock(&wq.lock);
  while (wq.count == QUEUE_SIZE)
    pthread_cond_wait(&wq.not_full, &wq.lock); // Wait if queue is full
  wq.fds[wq.tail] = fd;
  wq.tail = (wq.tail + 1) % QUEUE_SIZE; // Circular increment
  wq.count++;

  // Signal that a new client is available
  pthread_cond_signal(&wq.not_empty);
  pthread_mutex_unlock(&wq.lock);
}

static int wq_pop(void) {
  // Remove and return a client fd from the work queue
  pthread_mutex_lock(&wq.lock);
  while (wq.count == 0)
    pthread_cond_wait(&wq.not_empty, &wq.lock);
  int fd = wq.fds[wq.head];             // Get client fd at head of queue
  wq.head = (wq.head + 1) % QUEUE_SIZE; // Circular increment
  wq.count--;                           // Decrease count of items in queue

  // Signal that space is available in the queue
  pthread_cond_signal(&wq.not_full);
  pthread_mutex_unlock(&wq.lock);
  return fd;
}

// ------- INCOMING BUFFER -------
// Minimizes syscalls by caching data read from the socket. The buffer is
// persistent during the TCP connection, so if a request is split across
// multiple recv() calls, we can still assemble
#define INBUF_SIZE 2048
typedef struct {
  char data[INBUF_SIZE];
  int read_pos;
  int write_pos;
} InBuf;

static int inb_read(int fd, InBuf *b, void *dest, size_t len) {
  // Reads 'len' bytes from socket into 'dest', using 'b' as intermediate
  // buffer.
  uint8_t *dst = (uint8_t *)dest;
  size_t needed = len;

  while (needed > 0) {
    // Read from buffer if it has data. Otherwise, read from socket into buffer.
    int available = b->write_pos - b->read_pos;
    if (available > 0) {
      size_t chunk = ((size_t)available < needed) ? (size_t)available : needed;
      memcpy(dst, b->data + b->read_pos, chunk);
      b->read_pos += chunk;
      dst += chunk;
      needed -= chunk;
      if (b->read_pos == b->write_pos) {
        b->read_pos = 0;
        b->write_pos = 0;
      }
    } else {
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

// ------- OUTGOING BUFFER -------
// Same pattern than the proxy: serialize the entire response into a single
// buffer and send with one send() call, to guarantee atomicity and minimize
// syscalls. Maximum response size: 1+4+255+4+32*4+4+4+4 = 663 bytes
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
  for (int i = 0; i < n; i++) {
    uint32_t bits;
    memcpy(&bits, &arr[i], sizeof(float));
    ob_u32(b, bits);
  }
}
static inline void ob_flush(int fd, const OutBuf *b) {
  send(fd, b->d, b->n, 0);
}

// Receives N floats serialized as big-endian uint32_t. We read them into a
// temporary buffer of uint32_t, convert endianness, and then memcpy to float
// array
static int recv_floats(int fd, InBuf *b, float *arr, int n) {
  uint32_t buf[32];
  if (inb_read(fd, b, buf, (size_t)n * sizeof(uint32_t)) < 0)
    return -1;
  for (int i = 0; i < n; i++) {
    buf[i] = ntohl(buf[i]);
    memcpy(&arr[i], &buf[i], sizeof(float));
  }
  return 0;
}

static int recv_field(int fd, InBuf *b, char *buffer) {
  // Receive a string field sent as: [4-byte length][data]
  uint32_t net_len;
  if (inb_read(fd, b, &net_len, 4) < 0)
    return -1;
  uint32_t len = ntohl(net_len);
  if (inb_read(fd, b, buffer, len) < 0)
    return -1;
  buffer[len] = '\0';
  return 0;
}

// ====================== WORKER ======================

void *worker_thread(void *arg) {

  // ----- INITIALIZATION -----
  int id = *(int *)arg;
  free(arg);
  LOG_INFO("Worker %d iniciado.\n", id);

  while (server_running) {
    int client_fd = wq_pop();
    if (client_fd < 0)
      break;

    // ----- SOCKET OPTIONS -----
    struct timeval tv = {30, 0}; // 30s timeout for recv() to detect client disconnects
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int nodelay = 1; // Disable Nagle's algorithm to minimize latency for small messages
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // ----- RECEIVE REQUESTS -----

    InBuf inb = {0};
    uint8_t op_code; // Read operation code
    while (inb_read(client_fd, &inb, &op_code, 1) == 0) {
      switch (op_code) {

      case OP_DESTROY: { // 0x01
        // ----- PROCESS REQUEST -----
        int8_t res = (int8_t)destroy();
        LOG_INFO("Worker %d: destroy() = %d\n", id, res);

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
        if (recv_field(client_fd, &inb, key) < 0)
          goto disconnect;
        if (recv_field(client_fd, &inb, value1) < 0)
          goto disconnect;

        // Read value2 array
        uint32_t n_net;
        if (inb_read(client_fd, &inb, &n_net, 4) < 0)
          goto disconnect;
        N_value2 = (int)ntohl(n_net);

        if (recv_floats(client_fd, &inb, V_value2, N_value2) < 0) // Bug 1 fix
          goto disconnect;

        // Read value3 struct
        int32_t x, y, z;
        if (inb_read(client_fd, &inb, &x, 4) < 0)
          goto disconnect;
        if (inb_read(client_fd, &inb, &y, 4) < 0)
          goto disconnect;
        if (inb_read(client_fd, &inb, &z, 4) < 0)
          goto disconnect;
        value3.x = (int)ntohl(x);
        value3.y = (int)ntohl(y);
        value3.z = (int)ntohl(z);

        // ----- PROCESS REQUEST -----
        int res = (op_code == OP_MODIFY_VALUE)
                      ? modify_value(key, value1, N_value2, V_value2, value3)
                      : set_value(key, value1, N_value2, V_value2, value3);

        LOG_INFO("Worker %d: %s('%s') = %d\n", id,
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
        if (recv_field(client_fd, &inb, key) < 0)
          goto disconnect;

        // ----- PROCESS REQUEST -----
        int res = get_value(key, value1, &N_value2, V_value2, &value3);
        LOG_INFO("Worker %d: get_value('%s') = %d\n", id, key, res);

        if (res == 0) {
          OutBuf rb = {.n = 0};
          ob_u8(&rb, 0); // result = éxito
          ob_str(&rb, value1);
          ob_u32(&rb, (uint32_t)N_value2);
          ob_floats(&rb, V_value2, N_value2);
          ob_i32(&rb, value3.x);
          ob_i32(&rb, value3.y);
          ob_i32(&rb, value3.z);
          ob_flush(client_fd, &rb);
        } else {
          int8_t r = (int8_t)res;
          send(client_fd, &r, 1, 0);
        }
        break;
      }

      case OP_DELETE_KEY: { // 0x05
        // Receive key field
        char key[MAX_KEY_LEN];
        if (recv_field(client_fd, &inb, key) < 0)
          goto disconnect;

        // ----- PROCESS REQUEST -----
        int res = delete_key(key);
        LOG_INFO("Worker %d: delete_key('%s') = %d\n", id, key, res);

        // ----- SEND RESPONSE -----
        int8_t r = (int8_t)res;
        send(client_fd, &r, 1, 0);
        break;
      }

      case OP_EXIST: { // 0x06
        // Receive key field
        char key[MAX_KEY_LEN];
        if (recv_field(client_fd, &inb, key) < 0)
          goto disconnect;

        // ----- PROCESS REQUEST -----
        int res = exist(key);
        LOG_INFO("Worker %d: exist('%s') = %d\n", id, key, res);

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
    LOG_INFO("Worker %d: cliente desconectado.\n", id);
  }

  return NULL;
}

// ====================== MAIN ======================

int main(int argc, char **argv) {
  if (argc != 2) { // Check for port argument
    fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
    return 1;
  }

  int port = atoi(argv[1]);

  // ----- SIGNAL HANDLERS -----
  // SIGINT/SIGTERM: starts graceful shutdown by setting server_running=0,
  struct sigaction sa;
  sa.sa_handler = handle_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  // SIGPIPE: If the client disconnects while we're sending, we don't want the process to terminate
  struct sigaction sa_ign = {.sa_handler = SIG_IGN};
  sigemptyset(&sa_ign.sa_mask);
  sa_ign.sa_flags = 0;
  sigaction(SIGPIPE, &sa_ign, NULL);

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
  if (listen(server_fd, SOMAXCONN) <
      0) { // SOMAXCONN: maximum allowed by the system for backlog
    perror("listen");
    return 1;
  }

  destroy(); // Init hash table before accepting clients
  printf("Servidor escuchando en el puerto %d\n", port);

  // ----- CREATE WORKER THREADS -----
  long num_cores = sysconf(_SC_NPROCESSORS_ONLN); // Get number of CPU cores
  if (num_cores < 1)
    num_cores = 4;
  LOG_INFO("Iniciando %ld workers...\n", num_cores);

  // Create a pool of worker threads that will handle client requests from the
  // work queue
  pthread_t workers[num_cores];
  for (int i = 0; i < num_cores; i++) {
    int *id = malloc(sizeof(int));
    *id = i + 1;
    pthread_create(&workers[i], NULL, worker_thread, id);
  }

  // ----- ACCEPT LOOP -----
  while (server_running) {
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
      if (errno == EINTR)
        continue;  
      perror("accept");
      break; 
    }
    wq_push(client_fd);
  }

  // ----- GRACEFUL SHUTDOWN -----
  for (int i = 0; i < num_cores; i++)
    wq_push(-1);
  for (int i = 0; i < num_cores; i++)
    pthread_join(workers[i], NULL);

  close(server_fd);
  return 0;
}