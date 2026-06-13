#include "dlx4sop/residual.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef enum trail_kind {
  TRAIL_SET_UNARY,
  TRAIL_SET_CONSTANT,
  TRAIL_SET_ACTIVE_VAR,
  TRAIL_SET_ACTIVE_EDGE,
} trail_kind_t;

typedef struct trail_entry {
  trail_kind_t kind;
  uint32_t index;
  uint64_t old_value;
} trail_entry_t;

struct qsop_residual {
  uint32_t r;
  uint32_t nvars;
  uint32_t nedges;
  uint32_t constant;

  uint32_t *unary;
  uint32_t *edge_u;
  uint32_t *edge_v;
  uint32_t *edge_q;
  uint8_t *active_var;
  uint8_t *active_edge;
  uint32_t active_vars;
  uint32_t active_edges;

  trail_entry_t *trail;
  size_t trail_len;
  size_t trail_cap;
};

static void set_error(qsop_error_t *error, const char *fmt, ...) {
  if (error == NULL) {
    return;
  }

  error->path = NULL;
  error->line = 0;
  error->column = 0;

  va_list args;
  va_start(args, fmt);
  vsnprintf(error->message, sizeof(error->message), fmt, args);
  va_end(args);
}

static uint32_t add_mod(uint32_t a, uint32_t b, uint32_t r) {
  return (uint32_t)(((uint64_t)a + b) % r);
}

static bool reserve_trail(qsop_residual_t *residual, size_t needed, qsop_error_t *error) {
  if (needed <= residual->trail_cap) {
    return true;
  }

  size_t new_cap = residual->trail_cap == 0 ? 32U : residual->trail_cap;
  while (new_cap < needed) {
    if (new_cap > SIZE_MAX / 2U) {
      set_error(error, "trail is too large");
      return false;
    }
    new_cap *= 2U;
  }

  trail_entry_t *new_trail = realloc(residual->trail, new_cap * sizeof(*new_trail));
  if (new_trail == NULL) {
    set_error(error, "out of memory while growing residual trail");
    return false;
  }

  residual->trail = new_trail;
  residual->trail_cap = new_cap;
  return true;
}

static bool push_trail(qsop_residual_t *residual, trail_kind_t kind, uint32_t index,
                       uint64_t old_value, qsop_error_t *error) {
  if (!reserve_trail(residual, residual->trail_len + 1U, error)) {
    return false;
  }

  residual->trail[residual->trail_len++] = (trail_entry_t){
      .kind = kind,
      .index = index,
      .old_value = old_value,
  };
  return true;
}

static bool set_constant(qsop_residual_t *residual, uint32_t value, qsop_error_t *error) {
  if (residual->constant == value) {
    return true;
  }
  if (!push_trail(residual, TRAIL_SET_CONSTANT, 0, residual->constant, error)) {
    return false;
  }
  residual->constant = value;
  return true;
}

static bool set_unary(qsop_residual_t *residual, uint32_t v, uint32_t value,
                      qsop_error_t *error) {
  if (residual->unary[v] == value) {
    return true;
  }
  if (!push_trail(residual, TRAIL_SET_UNARY, v, residual->unary[v], error)) {
    return false;
  }
  residual->unary[v] = value;
  return true;
}

static bool set_var_active(qsop_residual_t *residual, uint32_t v, bool active,
                           qsop_error_t *error) {
  const uint8_t next = active ? 1U : 0U;
  if (residual->active_var[v] == next) {
    return true;
  }
  if (!push_trail(residual, TRAIL_SET_ACTIVE_VAR, v, residual->active_var[v], error)) {
    return false;
  }
  residual->active_var[v] = next;
  if (active) {
    residual->active_vars++;
  } else {
    residual->active_vars--;
  }
  return true;
}

static bool set_edge_active(qsop_residual_t *residual, uint32_t e, bool active,
                            qsop_error_t *error) {
  const uint8_t next = active ? 1U : 0U;
  if (residual->active_edge[e] == next) {
    return true;
  }
  if (!push_trail(residual, TRAIL_SET_ACTIVE_EDGE, e, residual->active_edge[e], error)) {
    return false;
  }
  residual->active_edge[e] = next;
  if (active) {
    residual->active_edges++;
  } else {
    residual->active_edges--;
  }
  return true;
}

bool qsop_residual_create(const qsop_instance_t *qsop, qsop_residual_t **out,
                          qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null residual output");
    return false;
  }
  *out = NULL;

  if (qsop == NULL) {
    set_error(error, "internal error: null QSOP instance");
    return false;
  }

  qsop_residual_t *residual = calloc(1, sizeof(*residual));
  if (residual == NULL) {
    set_error(error, "out of memory while allocating residual state");
    return false;
  }

  residual->r = qsop->r;
  residual->nvars = qsop->nvars;
  residual->nedges = qsop->nedges;
  residual->constant = qsop->constant;
  residual->active_vars = qsop->nvars;
  residual->active_edges = qsop->nedges;

  residual->unary = malloc((qsop->nvars == 0 ? 1U : qsop->nvars) * sizeof(*residual->unary));
  residual->edge_u = malloc((qsop->nedges == 0 ? 1U : qsop->nedges) * sizeof(*residual->edge_u));
  residual->edge_v = malloc((qsop->nedges == 0 ? 1U : qsop->nedges) * sizeof(*residual->edge_v));
  residual->edge_q = malloc((qsop->nedges == 0 ? 1U : qsop->nedges) * sizeof(*residual->edge_q));
  residual->active_var =
      malloc((qsop->nvars == 0 ? 1U : qsop->nvars) * sizeof(*residual->active_var));
  residual->active_edge =
      malloc((qsop->nedges == 0 ? 1U : qsop->nedges) * sizeof(*residual->active_edge));
  if (residual->unary == NULL || residual->edge_u == NULL || residual->edge_v == NULL ||
      residual->edge_q == NULL || residual->active_var == NULL || residual->active_edge == NULL) {
    qsop_residual_free(residual);
    set_error(error, "out of memory while copying residual state");
    return false;
  }

  memcpy(residual->unary, qsop->unary, (size_t)qsop->nvars * sizeof(*residual->unary));
  memcpy(residual->edge_u, qsop->edge_u, (size_t)qsop->nedges * sizeof(*residual->edge_u));
  memcpy(residual->edge_v, qsop->edge_v, (size_t)qsop->nedges * sizeof(*residual->edge_v));
  memcpy(residual->edge_q, qsop->edge_q, (size_t)qsop->nedges * sizeof(*residual->edge_q));
  memset(residual->active_var, 1, (size_t)qsop->nvars * sizeof(*residual->active_var));
  memset(residual->active_edge, 1, (size_t)qsop->nedges * sizeof(*residual->active_edge));

  *out = residual;
  return true;
}

void qsop_residual_free(qsop_residual_t *residual) {
  if (residual == NULL) {
    return;
  }

  free(residual->unary);
  free(residual->edge_u);
  free(residual->edge_v);
  free(residual->edge_q);
  free(residual->active_var);
  free(residual->active_edge);
  free(residual->trail);
  free(residual);
}

size_t qsop_residual_checkpoint(const qsop_residual_t *residual) {
  return residual == NULL ? 0 : residual->trail_len;
}

bool qsop_residual_undo(qsop_residual_t *residual, size_t checkpoint, qsop_error_t *error) {
  if (residual == NULL) {
    set_error(error, "internal error: null residual state");
    return false;
  }
  if (checkpoint > residual->trail_len) {
    set_error(error, "invalid residual checkpoint");
    return false;
  }

  while (residual->trail_len > checkpoint) {
    const trail_entry_t entry = residual->trail[--residual->trail_len];
    switch (entry.kind) {
    case TRAIL_SET_UNARY:
      residual->unary[entry.index] = (uint32_t)entry.old_value;
      break;
    case TRAIL_SET_CONSTANT:
      residual->constant = (uint32_t)entry.old_value;
      break;
    case TRAIL_SET_ACTIVE_VAR: {
      const uint8_t old = (uint8_t)entry.old_value;
      if (residual->active_var[entry.index] != old) {
        if (old != 0) {
          residual->active_vars++;
        } else {
          residual->active_vars--;
        }
        residual->active_var[entry.index] = old;
      }
      break;
    }
    case TRAIL_SET_ACTIVE_EDGE: {
      const uint8_t old = (uint8_t)entry.old_value;
      if (residual->active_edge[entry.index] != old) {
        if (old != 0) {
          residual->active_edges++;
        } else {
          residual->active_edges--;
        }
        residual->active_edge[entry.index] = old;
      }
      break;
    }
    }
  }

  return true;
}

bool qsop_residual_branch(qsop_residual_t *residual, uint32_t v, uint8_t value,
                          qsop_error_t *error) {
  if (residual == NULL) {
    set_error(error, "internal error: null residual state");
    return false;
  }
  if (v >= residual->nvars) {
    set_error(error, "branch variable is outside residual range");
    return false;
  }
  if (value > 1U) {
    set_error(error, "branch value must be 0 or 1");
    return false;
  }
  if (residual->active_var[v] == 0) {
    set_error(error, "cannot branch on inactive variable");
    return false;
  }

  if (value == 1U) {
    if (!set_constant(residual, add_mod(residual->constant, residual->unary[v], residual->r),
                      error)) {
      return false;
    }
    for (uint32_t e = 0; e < residual->nedges; e++) {
      if (residual->active_edge[e] == 0) {
        continue;
      }

      uint32_t other = UINT32_MAX;
      if (residual->edge_u[e] == v) {
        other = residual->edge_v[e];
      } else if (residual->edge_v[e] == v) {
        other = residual->edge_u[e];
      }

      if (other != UINT32_MAX && residual->active_var[other] != 0 &&
          !set_unary(residual, other, add_mod(residual->unary[other], residual->edge_q[e],
                                             residual->r),
                     error)) {
        return false;
      }
    }
  }

  for (uint32_t e = 0; e < residual->nedges; e++) {
    if (residual->active_edge[e] != 0 &&
        (residual->edge_u[e] == v || residual->edge_v[e] == v) &&
        !set_edge_active(residual, e, false, error)) {
      return false;
    }
  }

  return set_var_active(residual, v, false, error);
}

uint32_t qsop_residual_modulus(const qsop_residual_t *residual) {
  return residual == NULL ? 0 : residual->r;
}

uint32_t qsop_residual_nvars(const qsop_residual_t *residual) {
  return residual == NULL ? 0 : residual->nvars;
}

uint32_t qsop_residual_nedges(const qsop_residual_t *residual) {
  return residual == NULL ? 0 : residual->nedges;
}

uint32_t qsop_residual_active_vars(const qsop_residual_t *residual) {
  return residual == NULL ? 0 : residual->active_vars;
}

uint32_t qsop_residual_active_edges(const qsop_residual_t *residual) {
  return residual == NULL ? 0 : residual->active_edges;
}

uint32_t qsop_residual_constant(const qsop_residual_t *residual) {
  return residual == NULL ? 0 : residual->constant;
}

uint32_t qsop_residual_unary(const qsop_residual_t *residual, uint32_t v) {
  if (residual == NULL || v >= residual->nvars) {
    return 0;
  }
  return residual->unary[v];
}

bool qsop_residual_var_active(const qsop_residual_t *residual, uint32_t v) {
  return residual != NULL && v < residual->nvars && residual->active_var[v] != 0;
}

bool qsop_residual_edge_active(const qsop_residual_t *residual, uint32_t e) {
  return residual != NULL && e < residual->nedges && residual->active_edge[e] != 0;
}
