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
    string key<255>;
    string value1<255>;
    float  V_value2<32>; /* Max 32 */
    paquete_rpc value3;
};

/* get_value, delete_key y exist: only the key */
struct key_arg {
    string key<255>;
};

/* ===================== RESULT TYPES ===================== */

/* Simple result: destroy, set, modify, delete, exist */
struct result_simple {
    int result;
};

/* get_value: code + everything */
struct result_get {
    int         result;
    string      value1<255>;
    float       V_value2<32>;
    paquete_rpc value3;
};

/* ===================== RPC PROGRAM ===================== */

program CLAVES_PROG {
    version CLAVES_VERS {
        result_simple DESTROY(void)            = 1;
        result_simple SET_VALUE(write_args)     = 2;
        result_get    GET_VALUE(key_arg)        = 3;
        result_simple MODIFY_VALUE(write_args)  = 4;
        result_simple DELETE_KEY(key_arg)       = 5;
        result_simple EXIST(key_arg)            = 6;
    } = 1;
} = 0x20000001;
