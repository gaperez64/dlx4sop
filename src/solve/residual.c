#include "dlx4sop/residual.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef enum trail_kind {
  TRAIL_SET_UNARY,
  TRAIL_SET_CONSTANT,
  TRAIL_SET_ACTIVE_VAR,
  TRAIL_SET_ACTIVE_EDGE,
  TRAIL_SET_ACTIVE_DEGREE,
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
  uint32_t *incident_offset;
  uint32_t *incident_edge;
  uint32_t *active_degree;
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

static uint64_t fingerprint_u64(uint64_t fingerprint, uint64_t value) {
  fingerprint ^= value;
  fingerprint *= UINT64_C(1099511628211);
  fingerprint ^= value >> 32U;
  fingerprint *= UINT64_C(1099511628211);
  return fingerprint;
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

  const uint32_t u = residual->edge_u[e];
  const uint32_t v = residual->edge_v[e];
  const bool self_loop = u == v;
  const size_t trail_entries = self_loop ? 2U : 3U;
  if (!active && (residual->active_degree[u] == 0 ||
                  (!self_loop && residual->active_degree[v] == 0))) {
    set_error(error, "internal error: residual degree underflow");
    return false;
  }
  if (!reserve_trail(residual, residual->trail_len + trail_entries, error) ||
      !push_trail(residual, TRAIL_SET_ACTIVE_EDGE, e, residual->active_edge[e], error) ||
      !push_trail(residual, TRAIL_SET_ACTIVE_DEGREE, u, residual->active_degree[u], error) ||
      (!self_loop &&
       !push_trail(residual, TRAIL_SET_ACTIVE_DEGREE, v, residual->active_degree[v], error))) {
    return false;
  }

  residual->active_edge[e] = next;
  if (active) {
    residual->active_edges++;
    residual->active_degree[u]++;
    if (!self_loop) {
      residual->active_degree[v]++;
    }
  } else {
    residual->active_edges--;
    residual->active_degree[u]--;
    if (!self_loop) {
      residual->active_degree[v]--;
    }
  }
  return true;
}

static bool build_incidence(qsop_residual_t *residual, qsop_error_t *error) {
  residual->incident_offset =
      calloc((size_t)residual->nvars + 1U, sizeof(*residual->incident_offset));
  residual->incident_edge =
      malloc((residual->nedges == 0 ? 1U : (size_t)residual->nedges * 2U) *
             sizeof(*residual->incident_edge));
  residual->active_degree =
      calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*residual->active_degree));
  uint32_t *cursor = calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*cursor));
  if (residual->incident_offset == NULL || residual->incident_edge == NULL ||
      residual->active_degree == NULL || cursor == NULL) {
    free(cursor);
    set_error(error, "out of memory while building residual incidence lists");
    return false;
  }

  for (uint32_t e = 0; e < residual->nedges; e++) {
    const uint32_t u = residual->edge_u[e];
    const uint32_t v = residual->edge_v[e];
    residual->incident_offset[u + 1U]++;
    residual->active_degree[u]++;
    if (v != u) {
      residual->incident_offset[v + 1U]++;
      residual->active_degree[v]++;
    }
  }
  for (uint32_t v = 1; v <= residual->nvars; v++) {
    residual->incident_offset[v] += residual->incident_offset[v - 1U];
  }
  memcpy(cursor, residual->incident_offset, (size_t)residual->nvars * sizeof(*cursor));
  for (uint32_t e = 0; e < residual->nedges; e++) {
    const uint32_t u = residual->edge_u[e];
    const uint32_t v = residual->edge_v[e];
    residual->incident_edge[cursor[u]++] = e;
    if (v != u) {
      residual->incident_edge[cursor[v]++] = e;
    }
  }

  free(cursor);
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
  if (!build_incidence(residual, error)) {
    qsop_residual_free(residual);
    return false;
  }

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
  free(residual->incident_offset);
  free(residual->incident_edge);
  free(residual->active_degree);
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
    case TRAIL_SET_ACTIVE_DEGREE:
      residual->active_degree[entry.index] = (uint32_t)entry.old_value;
      break;
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
    for (uint32_t i = residual->incident_offset[v]; i < residual->incident_offset[v + 1U]; i++) {
      const uint32_t e = residual->incident_edge[i];
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

  for (uint32_t i = residual->incident_offset[v]; i < residual->incident_offset[v + 1U]; i++) {
    const uint32_t e = residual->incident_edge[i];
    if (residual->active_edge[e] != 0 && !set_edge_active(residual, e, false, error)) {
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

uint32_t qsop_residual_edge_u(const qsop_residual_t *residual, uint32_t e) {
  if (residual == NULL || e >= residual->nedges) {
    return 0;
  }
  return residual->edge_u[e];
}

uint32_t qsop_residual_edge_v(const qsop_residual_t *residual, uint32_t e) {
  if (residual == NULL || e >= residual->nedges) {
    return 0;
  }
  return residual->edge_v[e];
}

uint32_t qsop_residual_edge_q(const qsop_residual_t *residual, uint32_t e) {
  if (residual == NULL || e >= residual->nedges) {
    return 0;
  }
  return residual->edge_q[e];
}

uint64_t qsop_residual_fingerprint(const qsop_residual_t *residual) {
  if (residual == NULL) {
    return 0;
  }

  uint64_t fingerprint = UINT64_C(1469598103934665603);
  fingerprint = fingerprint_u64(fingerprint, residual->r);
  fingerprint = fingerprint_u64(fingerprint, residual->nvars);
  fingerprint = fingerprint_u64(fingerprint, residual->nedges);
  fingerprint = fingerprint_u64(fingerprint, residual->constant);
  fingerprint = fingerprint_u64(fingerprint, residual->active_vars);
  fingerprint = fingerprint_u64(fingerprint, residual->active_edges);

  for (uint32_t v = 0; v < residual->nvars; v++) {
    fingerprint = fingerprint_u64(fingerprint, residual->active_var[v]);
    fingerprint =
        fingerprint_u64(fingerprint, residual->active_var[v] == 0 ? 0 : residual->unary[v]);
  }

  for (uint32_t e = 0; e < residual->nedges; e++) {
    fingerprint = fingerprint_u64(fingerprint, residual->edge_u[e]);
    fingerprint = fingerprint_u64(fingerprint, residual->edge_v[e]);
    fingerprint = fingerprint_u64(fingerprint, residual->edge_q[e]);
    fingerprint = fingerprint_u64(fingerprint, residual->active_edge[e]);
  }

  return fingerprint;
}

uint32_t qsop_residual_active_degree(const qsop_residual_t *residual, uint32_t v) {
  if (residual == NULL || v >= residual->nvars || residual->active_var[v] == 0) {
    return 0;
  }
  return residual->active_degree[v];
}

bool qsop_residual_active_components(const qsop_residual_t *residual, uint32_t *component,
                                     uint32_t *ncomponents, qsop_error_t *error) {
  if (component == NULL || ncomponents == NULL) {
    set_error(error, "internal error: null residual component output");
    return false;
  }
  *ncomponents = 0;

  if (residual == NULL) {
    set_error(error, "internal error: null residual state");
    return false;
  }

  uint32_t *queue = calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*queue));
  if (queue == NULL) {
    set_error(error, "out of memory while labelling residual components");
    return false;
  }

  for (uint32_t v = 0; v < residual->nvars; v++) {
    component[v] = UINT32_MAX;
  }

  uint32_t count = 0;
  for (uint32_t start = 0; start < residual->nvars; start++) {
    if (residual->active_var[start] == 0 || component[start] != UINT32_MAX) {
      continue;
    }

    uint32_t head = 0;
    uint32_t tail = 0;
    component[start] = count;
    queue[tail++] = start;
    while (head < tail) {
      const uint32_t v = queue[head++];
      for (uint32_t i = residual->incident_offset[v]; i < residual->incident_offset[v + 1U];
           i++) {
        const uint32_t e = residual->incident_edge[i];
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
            component[other] == UINT32_MAX) {
          component[other] = count;
          queue[tail++] = other;
        }
      }
    }
    count++;
  }

  free(queue);
  *ncomponents = count;
  return true;
}

bool qsop_residual_components_without_var(const qsop_residual_t *residual, uint32_t removed,
                                          uint32_t *out, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null residual component output");
    return false;
  }
  *out = 0;

  if (residual == NULL) {
    set_error(error, "internal error: null residual state");
    return false;
  }
  if (removed >= residual->nvars) {
    set_error(error, "removed variable is outside residual range");
    return false;
  }
  if (residual->active_var[removed] == 0) {
    set_error(error, "removed variable must be active");
    return false;
  }

  uint8_t *visited = calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*visited));
  uint32_t *queue = calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*queue));
  if (visited == NULL || queue == NULL) {
    free(visited);
    free(queue);
    set_error(error, "out of memory while estimating residual components");
    return false;
  }

  uint32_t components = 0;
  for (uint32_t start = 0; start < residual->nvars; start++) {
    if (start == removed || residual->active_var[start] == 0 || visited[start] != 0) {
      continue;
    }

    components++;
    uint32_t head = 0;
    uint32_t tail = 0;
    visited[start] = 1;
    queue[tail++] = start;
    while (head < tail) {
      const uint32_t v = queue[head++];
      for (uint32_t i = residual->incident_offset[v]; i < residual->incident_offset[v + 1U];
           i++) {
        const uint32_t e = residual->incident_edge[i];
        if (residual->active_edge[e] == 0 || residual->edge_u[e] == removed ||
            residual->edge_v[e] == removed) {
          continue;
        }

        uint32_t other = UINT32_MAX;
        if (residual->edge_u[e] == v) {
          other = residual->edge_v[e];
        } else if (residual->edge_v[e] == v) {
          other = residual->edge_u[e];
        }

        if (other != UINT32_MAX && residual->active_var[other] != 0 && visited[other] == 0) {
          visited[other] = 1;
          queue[tail++] = other;
        }
      }
    }
  }

  free(visited);
  free(queue);
  *out = components;
  return true;
}

bool qsop_residual_var_active(const qsop_residual_t *residual, uint32_t v) {
  return residual != NULL && v < residual->nvars && residual->active_var[v] != 0;
}

bool qsop_residual_edge_active(const qsop_residual_t *residual, uint32_t e) {
  return residual != NULL && e < residual->nedges && residual->active_edge[e] != 0;
}
