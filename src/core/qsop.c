#include "dlx4sop/qsop.h"

#include <stdlib.h>

void qsop_free(qsop_instance_t *qsop) {
  if (qsop == NULL) {
    return;
  }

  free(qsop->unary);
  free(qsop->edge_u);
  free(qsop->edge_v);
  free(qsop->edge_q);
  free(qsop);
}

const char *qsop_mode_name(qsop_mode_t mode) {
  switch (mode) {
  case QSOP_MODE_SIGN:
    return "sign";
  case QSOP_MODE_LABELLED:
    return "labelled";
  }
  return "unknown";
}
