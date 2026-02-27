#include "claves.h"
#include <stdio.h>
// ...

/*
 * APLICACIÓN DE PRUEBA.
 * Este programa testea el funcionamiento del servicio.
 * Es agnóstico a si se está usando la versión local o la distribuida
 * (dependerá de con qué librería se enlace en el Makefile).
 */

int main() {
    /*
     * BATERÍA DE PRUEBAS
     * 1. Llamar a init().
     * 2. Insertar una tupla válida (set_value).
     * 3. Intentar insertar una tupla duplicada (debe fallar).
     * 4. Recuperar la tupla (get_value) y comprobar que los datos son correctos.
     * 5. Modificar la tupla (modify_value).
     * 6. Comprobar existencia (exist).
     * 7. Borrar tupla (delete_key).
     * 8. Verificar que ya no existe.
     *
     * Imprimir resultados por pantalla para verificar visualmente.
     */
    
    return 0;
}