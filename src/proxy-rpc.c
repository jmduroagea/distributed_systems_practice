/**
 * @file proxy-rpc.c
 * @brief Client-side proxy for Exercise 3 (ONC RPC).
 *
 * Implements the claves.h API by forwarding every call to the RPC server whose
 * address is given in the IP_TUPLAS environment variable. The client
 * application links against this library instead of libclaves.so and does not
 * need to change a single line.
 *
 * Concurrency: a CLIENT handle is not thread-safe, so every thread owns its
 * own handle (thread-local storage). It is created lazily on the thread's first
 * call and destroyed automatically when the thread exits (pthread key
 * destructor) or when the process exits (atexit, for the thread calling exit).
 * No mutex is needed on the request path.
 *
 * Error handling: input is validated against the wire-format limits generated
 * from clavesRPC.x before anything is sent. If an RPC call fails, the handle is
 * discarded so the next call on that thread reconnects from scratch.
 */
#include "../include/claves.h"
#include "rpc/clavesRPC.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ===================== CONFIGURATION ===================== */

#define ENV_SERVER_IP   "IP_TUPLAS" /* Environment variable with the server address */
#define RPC_PROTOCOL    "tcp"       /* Transport used by clnt_create */
#define RPC_TIMEOUT_SEC 15          /* Per-call timeout, generous enough for stress tests */
#define MIN_V2          1           /* claves.h: N_value2 must be in [1, 32] */
#define PROXY_ERROR     (-1)        /* claves.h: -1 on service or communication error */

/* ===================== PER-THREAD CLIENT HANDLE ===================== */

static __thread CLIENT *clnt = NULL;

/* The key exists only to get a destructor call when each thread exits. */
static pthread_key_t   clnt_key;
static pthread_once_t  clnt_key_once = PTHREAD_ONCE_INIT;

/* Keeps the TLS variable and the key's value in sync, so the destructor always
 * receives the handle that is currently open (or NULL if there is none). */
static void set_client(CLIENT *new_clnt) {
    clnt = new_clnt;
    pthread_setspecific(clnt_key, new_clnt);
}

/* Closes the calling thread's handle, if it has one. Also registered with
 * atexit(): thread-key destructors do not run for the thread that calls exit(). */
static void release_client(void) {
    if (clnt != NULL) {
        clnt_destroy(clnt);
        set_client(NULL);
    }
}

/* Called by the threads library when a thread that used the proxy exits. */
static void client_key_destructor(void *handle) {
    if (handle != NULL) {
        clnt_destroy((CLIENT *)handle);
    }
}

static void init_client_key(void) {
    pthread_key_create(&clnt_key, client_key_destructor);
    atexit(release_client);
}

/* Returns the calling thread's handle, connecting first if needed.
 * Returns NULL if the server address is missing or the connection fails. */
static CLIENT *get_client(void) {
    if (clnt != NULL) {
        return clnt;
    }

    pthread_once(&clnt_key_once, init_client_key);

    const char *ip = getenv(ENV_SERVER_IP);
    if (ip == NULL) {
        fprintf(stderr, "ERROR: %s no definida\n", ENV_SERVER_IP);
        return NULL;
    }

    CLIENT *new_clnt = clnt_create(ip, CLAVES_PROG, CLAVES_VERS, RPC_PROTOCOL);
    if (new_clnt == NULL) {
        clnt_pcreateerror(ip);
        return NULL;
    }

    struct timeval timeout = {.tv_sec = RPC_TIMEOUT_SEC, .tv_usec = 0};
    clnt_control(new_clnt, CLSET_TIMEOUT, (char *)&timeout);

    set_client(new_clnt);
    return new_clnt;
}

/* Reports a failed call and drops the handle: after a transport error its
 * state is unknown, so the next call on this thread reconnects. */
static void handle_rpc_failure(const char *operation) {
    clnt_perror(clnt, operation);
    release_client();
}

/* ===================== INPUT VALIDATION ===================== */

/* True if s is non-NULL and fits in an XDR string<RPC_MAX_STR>.
 * strnlen stops after RPC_MAX_STR + 1 characters, so an unterminated or huge
 * buffer is never scanned to the end. */
static bool string_fits(const char *s) {
    return s != NULL && strnlen(s, RPC_MAX_STR + 1) <= RPC_MAX_STR;
}

/* Shared by set_value and modify_value. */
static bool write_input_is_valid(const char *key, const char *value1,
                                 int N_value2, const float *V_value2) {
    return string_fits(key) && string_fits(value1) && V_value2 != NULL &&
           N_value2 >= MIN_V2 && N_value2 <= RPC_MAX_V2;
}

/* ===================== MARSHALLING HELPERS ===================== */

/* Builds the request for set_value and modify_value. No data is copied: the
 * XDR layer reads straight from the caller's buffers while sending. */
static write_args build_write_args(char *key, char *value1, int N_value2,
                                   float *V_value2, struct Paquete value3) {
    write_args args;
    args.key = key;
    args.value1 = value1;
    args.V_value2.V_value2_len = (u_int)N_value2;
    args.V_value2.V_value2_val = V_value2;
    args.value3.x = value3.x;
    args.value3.y = value3.y;
    args.value3.z = value3.z;
    return args;
}

/* Copies a successful get_value reply into the caller's buffers. XDR already
 * rejected any reply longer than RPC_MAX_STR / RPC_MAX_V2 while decoding, and
 * claves.h requires value1 to have room for RPC_MAX_STR + 1 bytes. */
static void copy_get_result(const result_get *res, char *value1, int *N_value2,
                            float *V_value2, struct Paquete *value3) {
    strncpy(value1, res->value1, RPC_MAX_STR);
    value1[RPC_MAX_STR] = '\0';

    *N_value2 = (int)res->V_value2.V_value2_len;
    memcpy(V_value2, res->V_value2.V_value2_val,
           res->V_value2.V_value2_len * sizeof(float));

    value3->x = res->value3.x;
    value3->y = res->value3.y;
    value3->z = res->value3.z;
}

/* Common tail for every call that returns a result_simple: reports transport
 * errors, then reads the result BEFORE freeing the reply. */
static int finish_simple_call(enum clnt_stat stat, result_simple *res,
                              const char *operation) {
    if (stat != RPC_SUCCESS) {
        handle_rpc_failure(operation);
        return PROXY_ERROR;
    }
    int result = res->result;
    clnt_freeres(clnt, (xdrproc_t)xdr_result_simple, (caddr_t)res);
    return result;
}

/* ===================== PUBLIC API (claves.h) ===================== */

int destroy(void) {
    CLIENT *client = get_client();
    if (client == NULL) {
        return PROXY_ERROR;
    }

    result_simple res;
    memset(&res, 0, sizeof(res));
    enum clnt_stat stat = destroy_1(&res, client);
    return finish_simple_call(stat, &res, "destroy");
}

int set_value(char *key, char *value1, int N_value2, float *V_value2,
              struct Paquete value3) {
    if (!write_input_is_valid(key, value1, N_value2, V_value2)) {
        return PROXY_ERROR;
    }
    CLIENT *client = get_client();
    if (client == NULL) {
        return PROXY_ERROR;
    }

    write_args args = build_write_args(key, value1, N_value2, V_value2, value3);
    result_simple res;
    memset(&res, 0, sizeof(res));
    enum clnt_stat stat = set_value_1(args, &res, client);
    return finish_simple_call(stat, &res, "set_value");
}

int get_value(char *key, char *value1, int *N_value2, float *V_value2,
              struct Paquete *value3) {
    if (!string_fits(key) || value1 == NULL || N_value2 == NULL ||
        V_value2 == NULL || value3 == NULL) {
        return PROXY_ERROR;
    }
    CLIENT *client = get_client();
    if (client == NULL) {
        return PROXY_ERROR;
    }

    key_arg args = {.key = key};
    result_get res;
    memset(&res, 0, sizeof(res)); /* clnt_freeres must never see garbage pointers */

    enum clnt_stat stat = get_value_1(args, &res, client);
    if (stat != RPC_SUCCESS) {
        handle_rpc_failure("get_value");
        return PROXY_ERROR;
    }

    int result = res.result;
    if (result == 0) {
        copy_get_result(&res, value1, N_value2, V_value2, value3);
    }
    clnt_freeres(client, (xdrproc_t)xdr_result_get, (caddr_t)&res);
    return result;
}

int modify_value(char *key, char *value1, int N_value2, float *V_value2,
                 struct Paquete value3) {
    if (!write_input_is_valid(key, value1, N_value2, V_value2)) {
        return PROXY_ERROR;
    }
    CLIENT *client = get_client();
    if (client == NULL) {
        return PROXY_ERROR;
    }

    write_args args = build_write_args(key, value1, N_value2, V_value2, value3);
    result_simple res;
    memset(&res, 0, sizeof(res));
    enum clnt_stat stat = modify_value_1(args, &res, client);
    return finish_simple_call(stat, &res, "modify_value");
}

int delete_key(char *key) {
    if (!string_fits(key)) {
        return PROXY_ERROR;
    }
    CLIENT *client = get_client();
    if (client == NULL) {
        return PROXY_ERROR;
    }

    key_arg args = {.key = key};
    result_simple res;
    memset(&res, 0, sizeof(res));
    enum clnt_stat stat = delete_key_1(args, &res, client);
    return finish_simple_call(stat, &res, "delete_key");
}

int exist(char *key) {
    if (!string_fits(key)) {
        return PROXY_ERROR;
    }
    CLIENT *client = get_client();
    if (client == NULL) {
        return PROXY_ERROR;
    }

    key_arg args = {.key = key};
    result_simple res;
    memset(&res, 0, sizeof(res));
    enum clnt_stat stat = exist_1(args, &res, client);
    return finish_simple_call(stat, &res, "exist"); /* 1 exists, 0 not found, -1 error */
}
