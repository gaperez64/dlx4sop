#ifndef DLX4SOP_SOLVE_COMPONENT_KEY_H
#define DLX4SOP_SOLVE_COMPONENT_KEY_H

#include "dlx4sop/qsop.h"

#include <stdbool.h>
#include <stdint.h>

bool qsop_canonicalize_small_component(qsop_instance_t *sub, uint32_t max_nvars,
                                       qsop_error_t *error);

#endif
