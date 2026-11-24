#ifndef LOG_H
#define LOG_H

#include <stdio.h>

/*
 * Logs de depuración de los servidores.
 * Desactivados por defecto; para activarlos: make clean && make DEBUG_LOGS=1
 */
#ifndef DEBUG_MODE
#define DEBUG_MODE 0
#endif

#if DEBUG_MODE
#define LOG_INFO(...)                                                          \
  do {                                                                         \
    printf("[INFO] " __VA_ARGS__);                                             \
    fflush(stdout);                                                            \
  } while (0)
#define LOG_ERR(...)                                                           \
  do {                                                                         \
    fprintf(stderr, "[ERROR] " __VA_ARGS__);                                   \
    fflush(stderr);                                                            \
  } while (0)
#else
/*
 * Desactivados: el if (0) no genera código, pero el compilador sigue
 * comprobando el formato y considera usados los argumentos (sin esto,
 * variables que solo se usan en logs provocan -Wunused-variable).
 */
#define LOG_INFO(...)                                                          \
  do {                                                                         \
    if (0)                                                                     \
      printf("[INFO] " __VA_ARGS__);                                           \
  } while (0)
#define LOG_ERR(...)                                                           \
  do {                                                                         \
    if (0)                                                                     \
      fprintf(stderr, "[ERROR] " __VA_ARGS__);                                 \
  } while (0)
#endif

#endif
