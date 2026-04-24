/**
 * @file app-cliente.c
 * @brief Programa de prueba que simula un cliente realizando operaciones a
 *        través de un proxy al servidor. Este programa se puede ejecutar en modo local
 *        (sin servidor) o en modo distribuido (con servidor), dependiendo de la
 *        configuración. En modo local, se inicializa el entorno de claves sin necesidad
 *        de un servidor, mientras que en modo distribuido, se asume que el servidor ya
 *        está gestionando las claves. El programa realiza una serie de operaciones (set,
 *        exist, get, modify, delete) en un bucle para simular una carga de trabajo
 *        realista y medir el número de operaciones exitosas y con error.
 **/
#include "../include/claves.h"
#include <stdio.h>
#include <stdlib.h>


int main(int argc, char **argv) {
  
  int N_OPS = 1;

  // Ignorar argumentos de línea de comandos para este programa
  int id = argc > 1 ? atoi(argv[1]) : 0;
  printf("[C%d] Iniciando stress test (%d operaciones)...\n", id, N_OPS);

  // Solo inicializar en modo local (sin servidor).
  // En modo distribuido el servidor ya llama a destroy() al arrancar.
  #ifdef MODO_LOCAL
    destroy();
  #endif

  // Variables para medir resultadoss
  int ok = 0, err = 0;

  // Variables para get_value
  char key[64];
  float v2[3] = {1.1f, 2.2f, 3.3f};
  struct Paquete v3 = {id, id * 2, id * 3};

  // Bucle de operaciones simulando carga de trabajo
  for (int i = 0; i < N_OPS; i++) {
    // Generar una clave única para cada operación
    snprintf(key, sizeof(key), "c%d_key_%d", id, i);

    // SET
    int r = set_value(key, "stress_val", 3, v2, v3);
    r == 0 ? ok++ : err++;

    // EXIST
    r = exist(key);
    r == 1 ? ok++ : err++;

    // GET
    char gval[256];
    int N;
    float gv2[32];
    struct Paquete gv3;
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

  // Resultados
  printf("[C%d] Done. OK=%d ERR=%d TOTAL=%d\n", id, ok, err, ok + err);
  return 0;
}