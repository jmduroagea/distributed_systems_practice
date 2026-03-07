# OPCIONES DE COMPILACIÓN
CC = gcc
CFLAGS = -Wall -g -I./include -I./xxhash
LIBS = -lrt -lpthread

# OBJETIVOS
all: folders libclaves.so libproxyclaves.so servidor-mq cliente_local cliente_dist

# 0. Crear directorios si no existen (limpieza)
folders:
	@mkdir -p src include

# 1. PARTE A: LIBRERÍA LOCAL (libclaves.so)
# Compila claves.c y xxhash.c con -fPIC para crear código reubicable y luego la librería compartida
libclaves.so: src/claves.c xxhash/xxhash.c
	$(CC) $(CFLAGS) -fPIC -c xxhash/xxhash.c -o xxhash.o
	$(CC) $(CFLAGS) -fPIC -c src/claves.c -o claves.o
	$(CC) -shared -o libclaves.so claves.o xxhash.o

# 2. PARTE B: LIBRERÍA PROXY (libproxyclaves.so)
# Compila proxy-mq.c
libproxyclaves.so: src/proxy-mq.c
	$(CC) $(CFLAGS) -fPIC -c src/proxy-mq.c -o proxy.o
	$(CC) -shared -o libproxyclaves.so proxy.o

# 3. SERVIDOR (Ejecutable)
# Enlaza con la lógica real (libclaves.so)
servidor-mq: src/servidor-mq.c libclaves.so
	$(CC) $(CFLAGS) -o servidor-mq src/servidor-mq.c -L. -lclaves $(LIBS)

# 4. CLIENTE LOCAL (Test Parte A)
# Enlaza con libclaves.so
cliente_local: src/app-cliente.c libclaves.so
	$(CC) $(CFLAGS) -o cliente_local src/app-cliente.c -L. -lclaves $(LIBS)

# 5. CLIENTE DISTRIBUIDO (Test Parte B)
# Enlaza con libproxyclaves.so
cliente_dist: src/app-cliente.c libproxyclaves.so
	$(CC) $(CFLAGS) -o cliente_dist src/app-cliente.c -L. -lproxyclaves $(LIBS)

# LIMPIEZA
clean:
	rm -f *.o *.so servidor-mq cliente_local cliente_dist