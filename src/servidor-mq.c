/**
 * @file servidor-mq.c
 * @brief Servidor concurrente usando Colas de Mensajes POSIX y un Pool de Hilos Simétrico.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <mqueue.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>

#include "claves.h"
#include "mensajes.h"

#define DEBUG_MODE 1 // Set to 0 to silence terminal output for final submission

#if DEBUG_MODE
    #define LOG_INFO(...) { printf("[INFO] " __VA_ARGS__); fflush(stdout); }
    #define LOG_ERR(...)  { fprintf(stderr, "[ERROR] " __VA_ARGS__); fflush(stderr); }
#else
    #define LOG_INFO(...)
    #define LOG_ERR(...)
#endif

// Variables globales para el manejo seguro de señales
mqd_t q_servidor = (mqd_t)-1;
atomic_int server_running = 1;

/* ========================================================================= *
 * FUNCIONES AUXILIARES Y DE SISTEMA
 * ========================================================================= */

/**
 * @brief Manejador de señales para un apagado elegante (Graceful Shutdown)
 */
void handle_sigint(int sig) {
    server_running = 0;
    LOG_INFO("Señal SIGINT recibida. Apagando servidor...\n");
    
    if (q_servidor != (mqd_t)-1) {
        mq_close(q_servidor);
        // Al cerrar la cola, los hilos bloqueados en mq_receive despertarán con EBADF
        mq_unlink(SERVER_QUEUE); 
    }
    
    LOG_INFO("Limpieza de colas completada. Adiós.\n");
    exit(0);
}

/**
 * @brief Lee el límite de profundidad de colas impuesto por el kernel de Linux
 */
long get_max_queue_depth() {
    FILE *f = fopen("/proc/sys/fs/mqueue/msg_max", "r");
    long max_msg = 10; // Fallback seguro de POSIX
    if (f != NULL) {
        if (fscanf(f, "%ld", &max_msg) == 1) {
            fclose(f);
            return max_msg;
        }
        fclose(f);
    }
    return max_msg;
}

/**
 * @brief Envía la respuesta al cliente usando la estrategia de Micro-Reintentos
 */
void enviar_respuesta(const char *cola_cliente, Respuesta *resp) {
    mqd_t q_client = mq_open(cola_cliente, O_WRONLY);
    if (q_client == (mqd_t)-1) {
        LOG_ERR("Cola cliente %s no existe. Cliente desconectado.\n", cola_cliente);
        return; 
    }

    int retries = 3;
    int success = 0;

    while (retries > 0 && !success) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += 50000000; // 50 milisegundos de timeout
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000;
        }

        if (mq_timedsend(q_client, (const char*)resp, sizeof(Respuesta), 0, &timeout) == 0) {
            success = 1;
        } else {
            if (errno == EINTR || errno == ETIMEDOUT || errno == EAGAIN) {
                usleep(10000); // Esperar 10ms antes de reintentar
                retries--;
            } else {
                break; // Error fatal de la cola, abortar
            }
        }
    }

    if (!success) {
        LOG_ERR("Imposible enviar respuesta a %s tras 3 intentos.\n", cola_cliente);
    }
    
    mq_close(q_client);
}

/* ========================================================================= *
 * HILO TRABAJADOR (SYMMETRIC WORKER POOL)
 * ========================================================================= */

void* worker_thread(void* arg) {
    int thread_id = *(int*)arg;
    free(arg); // Liberar memoria del ID
    
    Peticion req;
    Respuesta resp;
    unsigned int prio;

    LOG_INFO("Worker %d iniciado y esperando peticiones.\n", thread_id);

    while (server_running) {
        // Bloqueo eficiente esperando mensajes. El kernel despierta a UN solo hilo.
        ssize_t bytes_read = mq_receive(q_servidor, (char*)&req, sizeof(Peticion), &prio);
        
        if (bytes_read == -1) {
            if (errno == EBADF || errno == EINVAL) break; // Cola cerrada por SIGINT
            continue;
        }

        // Preparar la plantilla de respuesta básica
        memset(&resp, 0, sizeof(Respuesta));
        resp.id_correlacion = req.id_correlacion;

        // Despachador de funciones de libclaves.so
        switch (req.op_code) {
            case OP_INIT:
                resp.result = destroy(); // Inicializa el servicio 
                LOG_INFO("Worker %d ejecutó destroy(). Resultado: %d\n", thread_id, resp.result);
                break;
                
            case OP_SET:
                resp.result = set_value(
                    req.payload.escritura.key, 
                    req.payload.escritura.value1, 
                    req.payload.escritura.N_value2, 
                    req.payload.escritura.V_value2, 
                    req.payload.escritura.value3
                ); // Inserta el elemento 
                LOG_INFO("Worker %d ejecutó set_value('%s'). Resultado: %d\n", thread_id, req.payload.escritura.key, resp.result);
                break;
                
            case OP_GET:
                // Las variables se pasan por referencia para ser rellenadas 
                resp.result = get_value(
                    req.payload.lectura.key, 
                    resp.value1, 
                    &resp.N_value2, 
                    resp.V_value2, 
                    &resp.value3
                ); // Obtiene los valores 
                LOG_INFO("Worker %d ejecutó get_value('%s'). Resultado: %d\n", thread_id, req.payload.lectura.key, resp.result);
                break;
                
            case OP_MODIFY:
                resp.result = modify_value(
                    req.payload.escritura.key, 
                    req.payload.escritura.value1, 
                    req.payload.escritura.N_value2, 
                    req.payload.escritura.V_value2, 
                    req.payload.escritura.value3
                ); // Modifica los valores 
                LOG_INFO("Worker %d ejecutó modify_value('%s'). Resultado: %d\n", thread_id, req.payload.escritura.key, resp.result);
                break;
                
            case OP_DELETE:
                resp.result = delete_key(req.payload.lectura.key); // Borra el elemento 
                LOG_INFO("Worker %d ejecutó delete_key('%s'). Resultado: %d\n", thread_id, req.payload.lectura.key, resp.result);
                break;
                
            case OP_EXIST:
                resp.result = exist(req.payload.lectura.key); // Determina si existe 
                LOG_INFO("Worker %d ejecutó exist('%s'). Resultado: %d\n", thread_id, req.payload.lectura.key, resp.result);
                break;
                
            default:
                resp.result = -1; // Error lógico devuelto al cliente 
                LOG_ERR("Código de operación desconocido: %d\n", req.op_code);
        }

        // Responder al cliente
        enviar_respuesta(req.q_name, &resp);
    }
    
    LOG_INFO("Worker %d finalizando.\n", thread_id);
    return NULL;
}

/* ========================================================================= *
 * BUCLE PRINCIPAL (MAIN)
 * ========================================================================= */

int main(int argc, char **argv) {
    // 1. Configurar cierre elegante (Graceful Shutdown)
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 2. Limpieza preventiva (por si el servidor crasheó previamente) 
    mq_unlink(SERVER_QUEUE);

    // 3. Inspección del sistema para la cola
    long max_msgs = get_max_queue_depth();
    
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = max_msgs; 
    attr.mq_msgsize = sizeof(Peticion);
    attr.mq_curmsgs = 0;

    // 4. Crear la cola principal del servidor
    q_servidor = mq_open(SERVER_QUEUE, O_CREAT | O_RDONLY, 0666, &attr);
    if (q_servidor == (mqd_t)-1) {
        perror("Error al crear la cola del servidor");
        exit(EXIT_FAILURE);
    }
    
    LOG_INFO("Cola del servidor creada: %s (Max msgs: %ld, Size: %ld bytes)\n", 
             SERVER_QUEUE, attr.mq_maxmsg, attr.mq_msgsize);

    // Inicializar el servicio subyacente de la API de forma centralizada 
    if (destroy() == -1) {
        LOG_ERR("Fallo al inicializar la estructura de datos interna.\n");
    }

    // 5. Inspección del sistema para hilos (Symmetric Thread Pool)
    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores < 1) num_cores = 4; // Fallback razonable
    
    LOG_INFO("Detectados %ld núcleos CPU. Iniciando Thread Pool...\n", num_cores);
    
    pthread_t workers[num_cores];
    for (int i = 0; i < num_cores; i++) {
        int *thread_id = malloc(sizeof(int));
        *thread_id = i + 1;
        if (pthread_create(&workers[i], NULL, worker_thread, thread_id) != 0) {
            perror("Error al crear hilo trabajador");
        }
    }

    LOG_INFO("Servidor listo y escuchando peticiones (Pulsa Ctrl+C para apagar)\n");

    // 6. El hilo principal duerme, despertará solo con la señal SIGINT
    while (server_running) {
        pause(); 
    }

    // Esperar a que los hilos terminen limpiamente tras el SIGINT
    for (int i = 0; i < num_cores; i++) {
        pthread_join(workers[i], NULL);
    }

    return 0;
}