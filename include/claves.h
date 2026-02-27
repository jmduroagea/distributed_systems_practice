#ifndef CLAVES_H
#define CLAVES_H


struct Paquete {
   int x ;
   int y ;
   int z ;
} ;


/**
 * @brief Esta llamada permite inicializar el servicio de elementos clave-valor1-valor2-valor3.
 * Mediante este servicio se destruyen todas las tuplas que estuvieran almacenadas previamente.
 * 
 * @return int La función devuelve 0 en caso de éxito y -1 en caso de error.
 * @retval 0 en caso de exito.
 * @retval -1 en caso de error.
 */
int destroy(void);

/**
 * @brief Este servicio inserta el elemento <key, value1, value2, value3>. 
 * El vector correspondiente al valor 2 vendrá dado por la dimensión del vector (N_Value2) y 
 * el vector en si (V_value2). 
 * El servicio devuelve 0 si se insertó con éxito y -1 en caso de error. 
 * Se considera error, intentar insertar una clave que ya existe previamente o 
 * que el valor N_value2 esté fuera de rango. En este caso se devolverá -1 y no se insertará. 
 * También se considerará error cualquier error en las comunicaciones.
 * 
 * 
 * @param key clave.
 * @param value1   valor1 [256].
 * @param N_value2 dimensión del vector V_value2 [1-32].
 * @param V_value2 vector de floats [32].
 * @param value3   estructura Paquete.
 * @return int El servicio devuelve 0 si se insertó con éxito y -1 en caso de error.
 * @retval 0 si se insertó con éxito.
 * @retval -1 en caso de error.
 */
int set_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3);

/**
 * @brief Este servicio permite obtener los valores asociados a la clave key. 
 * La cadena de caracteres asociada se devuelve en value1. 
 * En N_Value2 se devuelve la dimensión del vector asociado al valor 2 y en V_value2 las componentes del vector. 
 * Tanto value1 como V_value2 tienen que tener espacio reservado para poder almacenar el máximo número 
 * de elementos posibles (256 en el caso de la cadena de caracteres y 32 en el caso del vector de floats). 
 * La función devuelve 0 en caso de éxito y -1 en caso de error, por ejemplo, 
 * si no existe un elemento con dicha clave o si se produce un error de comunicaciones.
 * 
 * 
 * @param key clave.
 * @param value1   valor1 [256].
 * @param N_value2 dimensión del vector V_value2 [1-32] por referencia.
 * @param V_value2 vector de floats [32].
 * @param value3   estructura Paquete por referencia.
 * @return int El servicio devuelve 0 si se insertó con éxito y -1 en caso de error.
 * @retval 0 en caso de éxito.
 * @retval -1 en caso de error.
 */
int get_value(char *key, char *value1, int *N_value2, float *V_value2, struct Paquete *value3);

/**
 * @brief Este servicio permite modificar los valores asociados a la clave key. 
 * La función devuelve 0 en caso de éxito y -1 en caso de error, por ejemplo, 
 * si no existe un elemento con dicha clave o si se produce un error en las comunicaciones. 
 * También se devolverá -1 si el valor N_value2 está fuera de rango.
 * 
 * 
 * @param key clave.
 * @param value1   valor1 [256].
 * @param N_value2 dimensión del vector V_value2 [1-32].
 * @param V_value2 vector de floats [32].
 * @param value3   estructura Paquete.
 * @return int El servicio devuelve 0 si se insertó con éxito y -1 en caso de error.
 * @retval 0 si se modificó con éxito.
 * @retval -1 en caso de error.
 */
int modify_value(char *key, char *value1, int N_value2, float *V_value2, struct Paquete value3);

/**
 * @brief Este servicio permite borrar el elemento cuya clave es key. 
 * La función devuelve 0 en caso de éxito y -1 en caso de error. 
 * En caso de que la clave no exista también se devuelve -1.
 * 
 * @param key clave.
 * @return int La función devuelve 0 en caso de éxito y -1 en caso de error.
 * @retval 0 en caso de éxito.
 * @retval -1 en caso de error.
 */
int delete_key(char *key);

/**
 * @brief Este servicio permite determinar si existe un elemento con clave key.
 * La función devuelve 1 en caso de que exista y 0 en caso de que no exista. 
 * En caso de error se devuelve -1. Un error puede ocurrir en este caso por un problema en las comunicaciones.
 * 
 * @param key clave.
 * @return int La función devuelve 1 en caso de que exista y 0 en caso de que no exista. En caso de error se devuelve -1.
 * @retval 1 en caso de que exista.
 * @retval 0 en caso de que no exista.
 * @retval -1 en caso de error.
 */
int exist(char *key);

#endif