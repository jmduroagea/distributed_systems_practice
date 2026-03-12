// src/app-cliente-stress.c
#include <stdio.h>
#include <stdlib.h>
#include "../include/claves.h"

#define N_OPS 10000  // 10 clientes x 100 = 1000 peticiones

int main(int argc, char **argv) {
    int id = argc > 1 ? atoi(argv[1]) : 0;
    printf("[C%d] Iniciando stress test (%d operaciones)...\n", id, N_OPS);

    // Solo inicializar en modo local (sin servidor).
    // En modo distribuido el servidor ya llama a destroy() al arrancar.
#ifdef MODO_LOCAL
    destroy();
#endif

    int ok = 0, err = 0;
    char key[64];
    float v2[3] = {1.1f, 2.2f, 3.3f};
    struct Paquete v3 = {id, id*2, id*3};

    for (int i = 0; i < N_OPS; i++) {
        snprintf(key, sizeof(key), "c%d_key_%d", id, i);

        // SET
        int r = set_value(key, "stress_val", 3, v2, v3);
        r == 0 ? ok++ : err++;

        // EXIST
        r = exist(key);
        r == 1 ? ok++ : err++;

        // GET
        char gval[256]; int N; float gv2[32]; struct Paquete gv3;
        r = get_value(key, gval, &N, gv2, &gv3);
        r == 0 ? ok++ : err++;

        // MODIFY
        float v2b[] = {(float)i};
        struct Paquete v3b = {i, i, i};
        r = modify_value(key, "modified", 1, v2b, v3b);
        r == 0 ? ok++ : err++;

        // DELETE
        r = delete_key(key);
        r == 0 ? ok++ : err++;
    }

    printf("[C%d] Done. OK=%d ERR=%d TOTAL=%d\n", id, ok, err, ok+err);
    return 0;
}