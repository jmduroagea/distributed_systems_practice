#include "../../include/claves.h"
#include "clavesRPC.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((constructor)) static void init_storage(void) { destroy(); }

bool_t destroy_1_svc(result_simple *result, struct svc_req *rqstp) {
  result->result = destroy();
  return TRUE;
}

bool_t set_value_1_svc(write_args arg1, result_simple *result,
                       struct svc_req *rqstp) {
  result->result =
      set_value(arg1.key, arg1.value1, arg1.V_value2.V_value2_len,
                arg1.V_value2.V_value2_val,
                (struct Paquete){arg1.value3.x, arg1.value3.y, arg1.value3.z});
  return TRUE;
}

bool_t get_value_1_svc(key_arg arg1, result_get *result,
                       struct svc_req *rqstp) {
  // Local Buffer
  char value1[256];
  int N_value2;
  float V_value2[32];
  struct Paquete value3;

  memset(result, 0, sizeof(*result));

  result->result = get_value(arg1.key, value1, &N_value2, V_value2, &value3);

  if (result->result == 0) {
    // Copy retrieved values
    result->value1 = strdup(value1);
    result->V_value2.V_value2_len = N_value2;
    result->V_value2.V_value2_val = malloc(N_value2 * sizeof(float));
    memcpy(result->V_value2.V_value2_val, V_value2, N_value2 * sizeof(float));
    result->value3.x = value3.x;
    result->value3.y = value3.y;
    result->value3.z = value3.z;
  } else {
    result->value1 = strdup("");
  }

  return TRUE;
}

bool_t modify_value_1_svc(write_args arg1, result_simple *result,
                          struct svc_req *rqstp) {
  result->result = modify_value(
      arg1.key, arg1.value1, arg1.V_value2.V_value2_len,
      arg1.V_value2.V_value2_val,
      (struct Paquete){arg1.value3.x, arg1.value3.y, arg1.value3.z});
  return TRUE;
}

bool_t delete_key_1_svc(key_arg arg1, result_simple *result,
                        struct svc_req *rqstp) {
  result->result = delete_key(arg1.key);
  return TRUE;
}

bool_t exist_1_svc(key_arg arg1, result_simple *result, struct svc_req *rqstp) {
  result->result = exist(arg1.key);
  return TRUE;
}

int claves_prog_1_freeresult(SVCXPRT *transp, xdrproc_t xdr_result,
                             caddr_t result) {
  xdr_free(xdr_result, result);

  /*
   * Insert additional freeing code here, if needed
   */

  return 1;
}
