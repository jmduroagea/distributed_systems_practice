# ─────────────────────────────────────────────────────────────────────────────
# Build configuration
# Uso:
#   make          → build optimizado (release, -O2)
#   make debug    → build con símbolos de depuración (-g, -O0), hace clean primero
#   make clean    → borra artefactos compilados
# ─────────────────────────────────────────────────────────────────────────────
CC         = gcc
CFLAGS     = -Wall -O2 -flto -DNDEBUG -I./include -I./xxhash  # release por defecto
LIBS       = -lrt -lpthread
RPATH      = -Wl,-rpath='$$ORIGIN'
TIRPC_CFLAGS = -I/usr/include/tirpc
TIRPC_LIBS   = -ltirpc

.PHONY: all release debug clean stress-sock stress-dist stress-local

ALL_TARGETS = libclaves.so libproxyclaves.so libproxyclaves-rpc.so \
              servidor_mq servidor clavesRPC_server \
              cliente_local cliente_dist cliente_sock cliente_rpc

# ─────────────────────────────────────────────────────────────────────────────
all: release

release: $(ALL_TARGETS)

# debug limpia primero para garantizar que todo se recompila con -g/-O0.
# Para volver a release: make clean && make
debug: CFLAGS = -Wall -g -O0 -I./include -I./xxhash
debug: clean $(ALL_TARGETS)

# Genera RPC a partir del .x
rpc-gen:
	cd src/rpc && rpcgen -aNM clavesRPC.x

# ── Librerías ─────────────────────────────────────────────────────────────────
libclaves.so: src/utils/claves.c xxhash/xxhash.c
	$(CC) $(CFLAGS) -fPIC -c xxhash/xxhash.c -o xxhash.o
	$(CC) $(CFLAGS) -fPIC -c src/utils/hash-table.c -o hash-table.o
	$(CC) $(CFLAGS) -fPIC -c src/utils/claves.c -o claves.o
	$(CC) -shared -o $@ claves.o xxhash.o hash-table.o

# Ejercicio 1 — proxy MQ
libproxyclaves.so: src/proxy/proxy-mq.c
	$(CC) $(CFLAGS) -fPIC -c src/proxy/proxy-mq.c -o proxy.o
	$(CC) -shared -o $@ proxy.o

# Ejercicio 2 — proxy sockets
libproxyclaves-sock.so: src/proxy/proxy-sock.c
	$(CC) $(CFLAGS) -fPIC -c src/proxy/proxy-sock.c -o proxy-sock.o
	$(CC) -shared -o $@ proxy-sock.o

# Ejercicio 3 - rpc
libproxyclaves-rpc.so: src/proxy-rpc.c src/rpc/clavesRPC_clnt.c src/rpc/clavesRPC_xdr.c
	$(CC) $(CFLAGS) $(TIRPC_CFLAGS) -I./rpc -fPIC -c src/proxy-rpc.c -o proxy-rpc.o
	$(CC) $(CFLAGS) $(TIRPC_CFLAGS) -I./rpc -fPIC -c src/rpc/clavesRPC_clnt.c -o clavesRPC_clnt.o
	$(CC) $(CFLAGS) $(TIRPC_CFLAGS) -I./rpc -fPIC -c src/rpc/clavesRPC_xdr.c -o clavesRPC_xdr.o
	$(CC) -shared -o $@ proxy-rpc.o clavesRPC_clnt.o clavesRPC_xdr.o $(TIRPC_LIBS)

# ── Ejecutables ───────────────────────────────────────────────────────────────
servidor_mq: src/server/servidor-mq.c libclaves.so
	$(CC) $(CFLAGS) -o $@ $< -L. -lclaves $(LIBS) $(RPATH)

servidor: src/server/servidor-sock.c libclaves.so
	$(CC) $(CFLAGS) -o $@ $< -L. -lclaves $(LIBS) $(RPATH)

cliente_local: src/app-cliente.c libclaves.so
	$(CC) $(CFLAGS) -DMODO_LOCAL -o $@ $< -L. -lclaves $(LIBS) $(RPATH)

# Ejercicio 1 — cliente con MQ
cliente_dist: src/app-cliente.c libproxyclaves.so
	$(CC) $(CFLAGS) -o $@ $< -L. -lproxyclaves $(LIBS) $(RPATH)

# Ejercicio 2 — cliente con sockets
cliente_sock: src/app-cliente.c libproxyclaves-sock.so
	$(CC) $(CFLAGS) -o $@ $< -L. -lproxyclaves-sock $(LIBS) $(RPATH)

# Ejercicio 3 -Servidor RPC
clavesRPC_server: src/rpc/clavesRPC_server.c src/rpc/clavesRPC_svc.c src/rpc/clavesRPC_xdr.c libclaves.so
	$(CC) $(CFLAGS) $(TIRPC_CFLAGS) -I./rpc -c src/rpc/clavesRPC_server.c -o clavesRPC_server.o
	$(CC) $(CFLAGS) $(TIRPC_CFLAGS) -I./rpc -c src/rpc/clavesRPC_svc.c -o clavesRPC_svc.o
	$(CC) $(CFLAGS) $(TIRPC_CFLAGS) -I./rpc -c src/rpc/clavesRPC_xdr.c -o clavesRPC_xdr_srv.o
	$(CC) -o $@ clavesRPC_server.o clavesRPC_svc.o clavesRPC_xdr_srv.o -L. -lclaves $(LIBS) $(TIRPC_LIBS) $(RPATH)

# Cliente RPC
cliente_rpc: src/app-cliente.c libproxyclaves-rpc.so
	$(CC) $(CFLAGS) -o $@ $< -L. -lproxyclaves-rpc $(LIBS) $(RPATH)

# ── Stress tests ──────────────────────────────────────────────────────────────
stress-sock: cliente_sock servidor
	./stress_validator.sh --modo sock --clientes 100 --timeout 3000

stress-dist: cliente_dist servidor_mq
	./stress_validator.sh --modo dist --cliente ./cliente_dist --limpiar

stress-local: cliente_local
	./stress_validator.sh --modo local --no-servidor --cliente ./cliente_local

# ── Limpieza ──────────────────────────────────────────────────────────────────
clean:
	rm -f *.o *.so servidor_mq servidor clavesRPC_server \
	      cliente_local cliente_dist cliente_sock cliente_rpc