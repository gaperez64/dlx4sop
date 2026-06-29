#include "dlx4sop/qsop.h"

#include <stdlib.h>

void qsop_free(qsop_instance_t *qsop) {
  if (qsop == NULL) {
    return;
  }

  free(qsop->unary);
  free(qsop->edge_u);
  free(qsop->edge_v);
  free(qsop);
}
