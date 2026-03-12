# OPCIONES DE COMPILACIÓN
CC = gcc
CFLAGS = -Wall -g -I./include -I./xxhash
LIBS = -lrt -lpthread
# Bandera MÁGICA para enlazar librerías dinámicas sin modificar LD_LIBRARY_PATH
RPATH = -Wl,-rpath='$$ORIGIN' 

# OBJETIVOS
all: folders libclaves.so libproxyclaves.so servidor_mq cliente_local cliente_dist

# 0. Crear directorios si no existen (limpieza)
folders:
	@mkdir -p src include

# 1. PARTE A: LIBRERÍA LOCAL (libclaves.so) [cite: 133]
# Compila claves.c y xxhash.c con -fPIC para crear código reubicable y luego la librería compartida
libclaves.so: src/claves.c xxhash/xxhash.c
	$(CC) $(CFLAGS) -fPIC -c xxhash/xxhash.c -o xxhash.o
	$(CC) $(CFLAGS) -fPIC -c src/claves.c -o claves.o
	$(CC) -shared -o libclaves.so claves.o xxhash.o

# 2. PARTE B: LIBRERÍA PROXY (libproxyclaves.so) [cite: 133]
# Compila proxy-mq.c
libproxyclaves.so: src/proxy-mq.c
	$(CC) $(CFLAGS) -fPIC -c src/proxy-mq.c -o proxy.o
	$(CC) -shared -o libproxyclaves.so proxy.o

# 3. SERVIDOR (Ejecutable) 
# Enlaza con la lógica real (libclaves.so) y añade RPATH
servidor_mq: src/servidor-mq.c libclaves.so
	$(CC) $(CFLAGS) -o servidor_mq src/servidor-mq.c -L. -lclaves $(LIBS) $(RPATH)

# 4. CLIENTE LOCAL (Test Parte A) [cite: 137]
# Enlaza con libclaves.so y añade RPATH
cliente_local: src/app-cliente.c libclaves.so
	$(CC) $(CFLAGS) -o cliente_local src/app-cliente.c -L. -lclaves $(LIBS) $(RPATH)

# 5. CLIENTE DISTRIBUIDO (Test Parte B) [cite: 136]
# Enlaza con libproxyclaves.so y añade RPATH
cliente_dist: src/app-cliente.c libproxyclaves.so
	$(CC) $(CFLAGS) -o cliente_dist src/app-cliente.c -L. -lproxyclaves $(LIBS) $(RPATH)

# 6. ORQUESTACIÓN DE PRUEBAS (Grader UX)
# Ejecuta el servidor en background, corre el cliente, y apaga el servidor limpiamente
test-distribuido: all
	@echo "\n=== Iniciando Servidor en Background ==="
	./servidor_mq & \
	SERVER_PID=$$!; \
	sleep 1; \
	echo "\n=== Ejecutando Cliente Distribuido ==="; \
	./cliente_dist; \
	echo "\n=== Apagando Servidor Limpiamente (SIGINT) ==="; \
	kill -SIGINT $$SERVER_PID

# LIMPIEZA
clean:
	rm -f *.o *.so servidor_mq cliente_local cliente_dist

CLIENTES_DIST = cliente_dist_1 cliente_dist_2 cliente_dist_3 cliente_dist_4 \
                cliente_dist_5 cliente_dist_6 cliente_dist_7 cliente_dist_8 \
                cliente_dist_9 cliente_dist_10

clientes_dist: $(CLIENTES_DIST)

cliente_dist_%: src/app-cliente-%.c libproxyclaves.so
	$(CC) $(CFLAGS) -o $@ $< -L. -lproxyclaves $(LIBS) $(RPATH)

test_concurrente: all clientes_dist
	chmod +x ./src/test_concurrente.sh
	./src/test_concurrente.sh > resultado_concurrente.txt

# Cliente stress test (local y distribuido)
app-cliente-stress-local: src/app-cliente-stress.c libclaves.so
	$(CC) $(CFLAGS) -DMODO_LOCAL -o app-cliente-stress-local src/app-cliente-stress.c -L. -lclaves $(LIBS) $(RPATH)

app-cliente-stress-dist: src/app-cliente-stress.c libproxyclaves.so
	$(CC) $(CFLAGS) -o app-cliente-stress-dist src/app-cliente-stress.c -L. -lproxyclaves $(LIBS) $(RPATH)

# Stress tests
stress-local: app-cliente-stress-local
	./stress_validator.sh --modo local --no-servidor --cliente ./app-cliente-stress-local

stress-dist: app-cliente-stress-dist servidor_mq
	./stress_validator.sh --modo dist --cliente ./app-cliente-stress-dist --limpiar