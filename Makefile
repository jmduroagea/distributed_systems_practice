# ─────────────────────────────────────────────
CC     = gcc
CFLAGS = -Wall -g -I./include -I./xxhash
LIBS   = -lrt -lpthread
RPATH  = -Wl,-rpath='$$ORIGIN'

# ─────────────────────────────────────────────
all: libclaves.so libproxyclaves.so servidor_mq cliente_local cliente_dist

# ── Librerías ─────────────────────────────────
libclaves.so: src/claves.c xxhash/xxhash.c
	$(CC) $(CFLAGS) -fPIC -c xxhash/xxhash.c -o xxhash.o
	$(CC) $(CFLAGS) -fPIC -c src/hash-table.c -o hash-table.o
	$(CC) $(CFLAGS) -fPIC -c src/claves.c -o claves.o
	$(CC) -shared -o $@ claves.o xxhash.o hash-table.o

libproxyclaves.so: src/proxy-mq.c
	$(CC) $(CFLAGS) -fPIC -c src/proxy-mq.c -o proxy.o
	$(CC) -shared -o $@ proxy.o

# ── Ejecutables ───────────────────────────────
servidor_mq: src/servidor-mq.c libclaves.so
	$(CC) $(CFLAGS) -o $@ $< -L. -lclaves $(LIBS) $(RPATH)

# cliente_local  → app-cliente.c + libclaves    (sin colas, con destroy())
cliente_local: src/app-cliente.c libclaves.so
	$(CC) $(CFLAGS) -DMODO_LOCAL -o $@ $< -L. -lclaves $(LIBS) $(RPATH)

# cliente_dist   → app-cliente.c + libproxyclaves (con colas)
cliente_dist: src/app-cliente.c libproxyclaves.so
	$(CC) $(CFLAGS) -o $@ $< -L. -lproxyclaves $(LIBS) $(RPATH)

# ── Stress test (reutiliza los mismos ejecutables) ────────────────────────────
stress-local: cliente_local
	./stress_validator.sh --modo local --no-servidor --cliente ./cliente_local

stress-dist: cliente_dist servidor_mq
	./stress_validator.sh --modo dist --cliente ./cliente_dist --limpiar

# ── Limpieza ──────────────────────────────────
clean:
	rm -f *.o *.so servidor_mq cliente_local cliente_dist