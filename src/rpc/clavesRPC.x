/* ===================== CONSTANTS ===================== */

/*
 * Wire-format limits. rpcgen turns each const into a #define in clavesRPC.h,
 * so the proxy validates its input against the same values that bound the XDR
 * types below (single source of truth).
 */
const RPC_MAX_STR = 255; /* Max length of key and value1, without the '\0' */
const RPC_MAX_V2  = 32;  /* Max number of floats in V_value2 */

/* ===================== SHARED TYPES ===================== */

/* Paquete in claves.h */
struct paquete_rpc {
    int x;
    int y;
    int z;
};

/* ===================== REQUEST TYPES ===================== */

/* For set and modify */
struct write_args {
    string      key<RPC_MAX_STR>;
    string      value1<RPC_MAX_STR>;
    float       V_value2<RPC_MAX_V2>;
    paquete_rpc value3;
};

/* get_value, delete_key and exist: only the key */
struct key_arg {
    string key<RPC_MAX_STR>;
};

/* ===================== RESULT TYPES ===================== */

/* Simple result: destroy, set, modify, delete, exist */
struct result_simple {
    int result;
};

/* get_value: code + everything */
struct result_get {
    int         result;
    string      value1<RPC_MAX_STR>;
    float       V_value2<RPC_MAX_V2>;
    paquete_rpc value3;
};

/* ===================== RPC PROGRAM ===================== */

program CLAVES_PROG {
    version CLAVES_VERS {
        result_simple DESTROY(void)            = 1;
        result_simple SET_VALUE(write_args)    = 2;
        result_get    GET_VALUE(key_arg)       = 3;
        result_simple MODIFY_VALUE(write_args) = 4;
        result_simple DELETE_KEY(key_arg)      = 5;
        result_simple EXIST(key_arg)           = 6;
    } = 1;
} = 0x20000001;
