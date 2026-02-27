#include "claves.h"
#include <stdio.h>
// ...

/*
 * PROGRAMA DE PRUEBAS
 * Genera ejecutables: 
 * - cliente_local (enlazado con libclaves.so)
 * - cliente_dist (enlazado con libproxyclaves.so)
 *
 * OBJETIVO:
 * Validar que la API funciona correctamente (insertar, buscar, borrar)
 * independientemente de si es local o distribuido.
 */

int main() {
    /*
     * Flujo de prueba sugerido:
     * 1. init().
     * 2. set_value() con tupla <"clave1", "val1", 3, vec, struct>.
     * 3. set_value() con tupla <"clave2", ...>.
     * 4. get_value("clave1") -> Verificar datos.
     * 5. modify_value("clave1") -> Cambiar datos.
     * 6. get_value("clave1") -> Verificar cambios.
     * 7. exist("clave1") -> Debe ser 1.
     * 8. delete_key("clave1").
     * 9. exist("clave1") -> Debe ser 0.
     * 10. Pruebas de error (insertar duplicado, borrar inexistente).
     */
    return 0;
}