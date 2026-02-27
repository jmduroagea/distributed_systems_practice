#
# SCRIPT DE COMPILACIÓN AUTOMATIZADA
#
# Debe generar:
# 1. libclaves.so (Lógica local)
# 2. libproxyclaves.so (Lógica cliente distribuido)
# 3. servidor-mq (Ejecutable servidor)
# 4. cliente_local (Test usando libclaves.so)
# 5. cliente_distribuido (Test usando libproxyclaves.so)
#

CC = gcc
CFLAGS = -Wall -g -I./include  # -I indica dónde buscar los .h
LIBS = -lrt -lpthread          # Librerías necesarias (tiempo real e hilos)

# TODO: Definir reglas para compilar .c a .o
# TODO: Definir reglas para crear las librerías dinámicas (-shared -fPIC)
# TODO: Definir reglas para los ejecutables enlazando con las librerías correctas (-L. -l...)

clean:
	# Regla para borrar ejecutables y ficheros temporales