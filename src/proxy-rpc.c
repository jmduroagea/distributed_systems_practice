#include "../include/claves.h"
#include "rpc/clavesRPC.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ================ GLOBAL STATE ================               
static CLIENT *clnt = NULL;

// ================ INITIALIZATION AND CLEANUP ================               
static void cleanup(void) {
    if (clnt != NULL) {
        clnt_destroy(clnt);
        clnt = NULL;
    }
}

static int init_proxy(void) {
    if (clnt != NULL) // Check if already initialized
        return 0;

    char *ip = getenv("IP_TUPLAS"); // Get IP
    if (ip == NULL) {
        fprintf(stderr, "ERROR: IP_TUPLAS no definida\n");
        return -1;
    }

    // Connect to RPC server
    clnt = clnt_create(ip, CLAVES_PROG, CLAVES_VERS, "tcp");
    if (clnt == NULL) {
        clnt_pcreateerror("clnt_create");
        return -1;
    }

    atexit(cleanup);
    return 0;
}

// ================ DESTROY ================               
int destroy(void) {
    if (init_proxy() != 0) // Ensure proxy is ready
        return -1;

    // Call the RPC function
    result_simple res;
    enum clnt_stat stat = destroy_1(&res, clnt);
    if (stat != RPC_SUCCESS) { // Check for RPC errors
        clnt_perror(clnt, "destroy_1");
        return -1;
    }
    return res.result; // Return the result from the RPC call
}

// ================ SET_VALUE ================               
int set_value(char *key, char *value1, int N_value2, float *V_value2,
              struct Paquete value3) {
    if (init_proxy() != 0)
        return -1;

    // Validate
    if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
        return -1;

    // Translate to RPC types
    write_args args;
    args.key    = key;
    args.value1 = value1;
    args.V_value2.V_value2_len = N_value2;
    args.V_value2.V_value2_val = V_value2;
    args.value3.x = value3.x;
    args.value3.y = value3.y;
    args.value3.z = value3.z;

    result_simple res;
    enum clnt_stat stat = set_value_1(args, &res, clnt); // Call RPC function
    if (stat != RPC_SUCCESS) {
        clnt_perror(clnt, "set_value_1");
        return -1;
    }
    return res.result;
}

// ================ GET_VALUE ================               
int get_value(char *key, char *value1, int *N_value2, float *V_value2,
              struct Paquete *value3) {
    if (init_proxy() != 0)
        return -1;

    // Prepare arguments for RPC call
    key_arg args; 
    args.key = key; // Only the key is needed for the get operation

    result_get res;
    enum clnt_stat stat = get_value_1(args, &res, clnt); // Call the RPC function
    if (stat != RPC_SUCCESS) {
        clnt_perror(clnt, "get_value_1");
        return -1;
    }

    if (res.result != 0) // If the result is not success, return the error code
        return res.result;

    // Copy the results back to the caller's variables
    strncpy(value1, res.value1, 255);
    value1[255] = '\0';
    *N_value2 = res.V_value2.V_value2_len;
    memcpy(V_value2, res.V_value2.V_value2_val,
           res.V_value2.V_value2_len * sizeof(float));
    value3->x = res.value3.x;
    value3->y = res.value3.y;
    value3->z = res.value3.z;

    return 0;
}

// ================ MODIFY_VALUE ================
int modify_value(char *key, char *value1, int N_value2, float *V_value2,
                 struct Paquete value3) {
    if (init_proxy() != 0)
        return -1;

    // Validation
    if (strlen(value1) > 255 || N_value2 < 1 || N_value2 > 32)
        return -1;

    // Prepare arguments for RPC call
    write_args args;
    args.key    = key;
    args.value1 = value1;
    args.V_value2.V_value2_len = N_value2;
    args.V_value2.V_value2_val = V_value2;
    args.value3.x = value3.x;
    args.value3.y = value3.y;
    args.value3.z = value3.z;

    result_simple res;
    enum clnt_stat stat = modify_value_1(args, &res, clnt); // Call the RPC function
    if (stat != RPC_SUCCESS) {
        clnt_perror(clnt, "modify_value_1");
        return -1;
    }
    return res.result;
}

// ================ DELETE_KEY ================               
int delete_key(char *key) {
    if (init_proxy() != 0)
        return -1;

    key_arg args;
    args.key = key; // Only the key is needed for the delete operation

    result_simple res;
    enum clnt_stat stat = delete_key_1(args, &res, clnt); // Call the RPC function
    if (stat != RPC_SUCCESS) {
        clnt_perror(clnt, "delete_key_1");
        return -1;
    }
    return res.result; // Return the result from the RPC call
}

// ================ EXIST ================               
int exist(char *key) {
    if (init_proxy() != 0)
        return -1;

    key_arg args;
    args.key = key; // Only the key is needed to check existence

    result_simple res;
    enum clnt_stat stat = exist_1(args, &res, clnt);
    if (stat != RPC_SUCCESS) {
        clnt_perror(clnt, "exist_1");
        return -1;
    }
    return res.result; // Return 1 if exists, 0 if not, or -1 on error
}