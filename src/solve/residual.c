#include "dlx4sop/residual.h"
#include "dlx4sop/bitset.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef enum trail_kind {
  TRAIL_SET_UNARY,
  TRAIL_SET_CONSTANT,
  TRAIL_SET_ACTIVE_VAR,
  TRAIL_SET_ACTIVE_EDGE,
  TRAIL_SET_ACTIVE_DEGREE,
  TRAIL_SET_ACTIVE_INCIDENT_HEAD,
  TRAIL_SET_INCIDENT_NEXT,
  TRAIL_SET_INCIDENT_PREV,
} trail_kind_t;

typedef struct trail_entry {
  trail_kind_t kind;
  uint32_t index;
  uint64_t old_value;
} trail_entry_t;

#define RESIDUAL_NIL UINT32_MAX

struct qsop_residual {
  uint64_t r;
  uint32_t nvars;
  uint32_t nedges;
  uint32_t incident_count;
  uint64_t constant;

  uint64_t *unary;
  uint32_t *edge_u;
  uint32_t *edge_v;
  uint32_t *incident_offset;
  uint32_t *incident_edge;
  uint32_t *incident_var;
  uint32_t *incident_next;
  uint32_t *incident_prev;
  uint32_t *active_incident_head;
  uint32_t *edge_slot_u;
  uint32_t *edge_slot_v;
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

static uint64_t add_mod_u64(uint64_t a, uint64_t b, uint64_t r) {
  if (r == 0) {
    return 0;
  }
  a %= r;
  b %= r;
  if (b == 0) {
    return a;
  }
  return a >= r - b ? a - (r - b) : a + b;
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

static bool set_constant(qsop_residual_t *residual, uint64_t value, qsop_error_t *error) {
  if (residual->constant == value) {
    return true;
  }
  if (!push_trail(residual, TRAIL_SET_CONSTANT, 0, residual->constant, error)) {
    return false;
  }
  residual->constant = value;
  return true;
}

static bool set_unary(qsop_residual_t *residual, uint32_t v, uint64_t value,
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

static bool set_active_incident_head(qsop_residual_t *residual, uint32_t v, uint32_t value,
                                     qsop_error_t *error) {
  if (residual->active_incident_head[v] == value) {
    return true;
  }
  if (!push_trail(residual, TRAIL_SET_ACTIVE_INCIDENT_HEAD, v,
                  residual->active_incident_head[v], error)) {
    return false;
  }
  residual->active_incident_head[v] = value;
  return true;
}

static bool set_incident_next(qsop_residual_t *residual, uint32_t slot, uint32_t value,
                              qsop_error_t *error) {
  if (residual->incident_next[slot] == value) {
    return true;
  }
  if (!push_trail(residual, TRAIL_SET_INCIDENT_NEXT, slot, residual->incident_next[slot],
                  error)) {
    return false;
  }
  residual->incident_next[slot] = value;
  return true;
}

static bool set_incident_prev(qsop_residual_t *residual, uint32_t slot, uint32_t value,
                              qsop_error_t *error) {
  if (residual->incident_prev[slot] == value) {
    return true;
  }
  if (!push_trail(residual, TRAIL_SET_INCIDENT_PREV, slot, residual->incident_prev[slot],
                  error)) {
    return false;
  }
  residual->incident_prev[slot] = value;
  return true;
}

static bool unlink_incidence_slot(qsop_residual_t *residual, uint32_t slot,
                                  qsop_error_t *error) {
  if (slot == RESIDUAL_NIL) {
    return true;
  }

  const uint32_t var = residual->incident_var[slot];
  const uint32_t prev = residual->incident_prev[slot];
  const uint32_t next = residual->incident_next[slot];
  if ((prev == RESIDUAL_NIL &&
       !set_active_incident_head(residual, var, next, error)) ||
      (prev != RESIDUAL_NIL && !set_incident_next(residual, prev, next, error)) ||
      (next != RESIDUAL_NIL && !set_incident_prev(residual, next, prev, error)) ||
      !set_incident_prev(residual, slot, RESIDUAL_NIL, error) ||
      !set_incident_next(residual, slot, RESIDUAL_NIL, error)) {
    return false;
  }
  return true;
}

static bool link_incidence_slot_to_head(qsop_residual_t *residual, uint32_t slot,
                                        qsop_error_t *error) {
  if (slot == RESIDUAL_NIL) {
    return true;
  }

  const uint32_t var = residual->incident_var[slot];
  const uint32_t old_head = residual->active_incident_head[var];
  if (!set_incident_prev(residual, slot, RESIDUAL_NIL, error) ||
      !set_incident_next(residual, slot, old_head, error) ||
      (old_head != RESIDUAL_NIL && !set_incident_prev(residual, old_head, slot, error)) ||
      !set_active_incident_head(residual, var, slot, error)) {
    return false;
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
  const size_t incidence_entries = self_loop ? 4U : 8U;
  const size_t trail_entries = (self_loop ? 2U : 3U) + incidence_entries;
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

  if (active) {
    if (!link_incidence_slot_to_head(residual, residual->edge_slot_u[e], error) ||
        !link_incidence_slot_to_head(residual, residual->edge_slot_v[e], error)) {
      return false;
    }
  } else if (!unlink_incidence_slot(residual, residual->edge_slot_u[e], error) ||
             !unlink_incidence_slot(residual, residual->edge_slot_v[e], error)) {
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
  uint32_t incidence_count = 0;
  for (uint32_t e = 0; e < residual->nedges; e++) {
    if (residual->edge_u[e] == residual->edge_v[e]) {
      incidence_count++;
    } else if (incidence_count <= UINT32_MAX - 2U) {
      incidence_count += 2U;
    } else {
      set_error(error, "too many residual incidence entries");
      return false;
    }
  }
  residual->incident_count = incidence_count;

  residual->incident_offset =
      calloc((size_t)residual->nvars + 1U, sizeof(*residual->incident_offset));
  residual->incident_edge =
      malloc((incidence_count == 0 ? 1U : (size_t)incidence_count) *
             sizeof(*residual->incident_edge));
  residual->incident_var =
      malloc((incidence_count == 0 ? 1U : (size_t)incidence_count) *
             sizeof(*residual->incident_var));
  residual->incident_next =
      malloc((incidence_count == 0 ? 1U : (size_t)incidence_count) *
             sizeof(*residual->incident_next));
  residual->incident_prev =
      malloc((incidence_count == 0 ? 1U : (size_t)incidence_count) *
             sizeof(*residual->incident_prev));
  residual->active_incident_head =
      malloc((residual->nvars == 0 ? 1U : residual->nvars) *
             sizeof(*residual->active_incident_head));
  residual->edge_slot_u =
      malloc((residual->nedges == 0 ? 1U : residual->nedges) * sizeof(*residual->edge_slot_u));
  residual->edge_slot_v =
      malloc((residual->nedges == 0 ? 1U : residual->nedges) * sizeof(*residual->edge_slot_v));
  residual->active_degree =
      calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*residual->active_degree));
  uint32_t *cursor = calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*cursor));
  if (residual->incident_offset == NULL || residual->incident_edge == NULL ||
      residual->incident_var == NULL || residual->incident_next == NULL ||
      residual->incident_prev == NULL || residual->active_incident_head == NULL ||
      residual->edge_slot_u == NULL || residual->edge_slot_v == NULL ||
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
    uint32_t slot = cursor[u]++;
    residual->incident_edge[slot] = e;
    residual->incident_var[slot] = u;
    residual->edge_slot_u[e] = slot;
    if (v != u) {
      slot = cursor[v]++;
      residual->incident_edge[slot] = e;
      residual->incident_var[slot] = v;
      residual->edge_slot_v[e] = slot;
    } else {
      residual->edge_slot_v[e] = RESIDUAL_NIL;
    }
  }

  for (uint32_t v = 0; v < residual->nvars; v++) {
    const uint32_t start = residual->incident_offset[v];
    const uint32_t end = residual->incident_offset[v + 1U];
    residual->active_incident_head[v] = start == end ? RESIDUAL_NIL : start;
    for (uint32_t slot = start; slot < end; slot++) {
      residual->incident_prev[slot] = slot == start ? RESIDUAL_NIL : slot - 1U;
      residual->incident_next[slot] = slot + 1U == end ? RESIDUAL_NIL : slot + 1U;
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
  residual->active_var =
      malloc((qsop->nvars == 0 ? 1U : qsop->nvars) * sizeof(*residual->active_var));
  residual->active_edge =
      malloc((qsop->nedges == 0 ? 1U : qsop->nedges) * sizeof(*residual->active_edge));
  if (residual->unary == NULL || residual->edge_u == NULL || residual->edge_v == NULL ||
      residual->active_var == NULL || residual->active_edge == NULL) {
    qsop_residual_free(residual);
    set_error(error, "out of memory while copying residual state");
    return false;
  }

  for (uint32_t v = 0; v < qsop->nvars; v++) {
    residual->unary[v] = qsop->unary[v];
  }
  memcpy(residual->edge_u, qsop->edge_u, (size_t)qsop->nedges * sizeof(*residual->edge_u));
  memcpy(residual->edge_v, qsop->edge_v, (size_t)qsop->nedges * sizeof(*residual->edge_v));
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
  free(residual->incident_offset);
  free(residual->incident_edge);
  free(residual->incident_var);
  free(residual->incident_next);
  free(residual->incident_prev);
  free(residual->active_incident_head);
  free(residual->edge_slot_u);
  free(residual->edge_slot_v);
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
      residual->unary[entry.index] = entry.old_value;
      break;
    case TRAIL_SET_CONSTANT:
      residual->constant = entry.old_value;
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
    case TRAIL_SET_ACTIVE_INCIDENT_HEAD:
      residual->active_incident_head[entry.index] = (uint32_t)entry.old_value;
      break;
    case TRAIL_SET_INCIDENT_NEXT:
      residual->incident_next[entry.index] = (uint32_t)entry.old_value;
      break;
    case TRAIL_SET_INCIDENT_PREV:
      residual->incident_prev[entry.index] = (uint32_t)entry.old_value;
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
    if (!set_constant(residual, add_mod_u64(residual->constant, residual->unary[v], residual->r),
                      error)) {
      return false;
    }
    for (uint32_t slot = residual->active_incident_head[v]; slot != RESIDUAL_NIL;
         slot = residual->incident_next[slot]) {
      const uint32_t e = residual->incident_edge[slot];

      uint32_t other = UINT32_MAX;
      if (residual->edge_u[e] == v) {
        other = residual->edge_v[e];
      } else if (residual->edge_v[e] == v) {
        other = residual->edge_u[e];
      }

      if (other != UINT32_MAX && residual->active_var[other] != 0 &&
          !set_unary(residual, other,
                     add_mod_u64(residual->unary[other], residual->r / 2U, residual->r),
                     error)) {
        return false;
      }
    }
  }

  for (uint32_t slot = residual->active_incident_head[v]; slot != RESIDUAL_NIL;) {
    const uint32_t next_slot = residual->incident_next[slot];
    const uint32_t e = residual->incident_edge[slot];
    if (residual->active_edge[e] != 0 && !set_edge_active(residual, e, false, error)) {
      return false;
    }
    slot = next_slot;
  }

  return set_var_active(residual, v, false, error);
}

uint64_t qsop_residual_modulus(const qsop_residual_t *residual) {
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

uint64_t qsop_residual_constant(const qsop_residual_t *residual) {
  return residual == NULL ? 0 : residual->constant;
}

uint64_t qsop_residual_unary(const qsop_residual_t *residual, uint32_t v) {
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
      for (uint32_t slot = residual->active_incident_head[v]; slot != RESIDUAL_NIL;
           slot = residual->incident_next[slot]) {
        const uint32_t e = residual->incident_edge[slot];

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

bool qsop_residual_split_without_var(const qsop_residual_t *residual, uint32_t removed,
                                     uint32_t *ncomponents, uint32_t *largest_component,
                                     qsop_error_t *error) {
  if (ncomponents == NULL || largest_component == NULL) {
    set_error(error, "internal error: null residual component output");
    return false;
  }
  *ncomponents = 0;
  *largest_component = 0;

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
  uint32_t largest = 0;
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
      for (uint32_t slot = residual->active_incident_head[v]; slot != RESIDUAL_NIL;
           slot = residual->incident_next[slot]) {
        const uint32_t e = residual->incident_edge[slot];
        if (residual->edge_u[e] == removed || residual->edge_v[e] == removed) {
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
    if (tail > largest) {
      largest = tail;
    }
  }

  free(visited);
  free(queue);
  *ncomponents = components;
  *largest_component = largest;
  return true;
}

bool qsop_residual_components_without_var(const qsop_residual_t *residual, uint32_t removed,
                                          uint32_t *out, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null residual component output");
    return false;
  }
  uint32_t largest = 0;
  return qsop_residual_split_without_var(residual, removed, out, &largest, error);
}

static bool validate_active_variable(const qsop_residual_t *residual, uint32_t v,
                                     const char *what, qsop_error_t *error) {
  if (residual == NULL) {
    set_error(error, "internal error: null residual state");
    return false;
  }
  if (v >= residual->nvars) {
    set_error(error, "%s variable is outside residual range", what);
    return false;
  }
  if (residual->active_var[v] == 0) {
    set_error(error, "%s variable must be active", what);
    return false;
  }
  return true;
}

static bool residual_active_edge_between(const qsop_residual_t *residual, uint32_t left,
                                         uint32_t right) {
  for (uint32_t slot = residual->active_incident_head[left]; slot != RESIDUAL_NIL;
       slot = residual->incident_next[slot]) {
    const uint32_t e = residual->incident_edge[slot];
    if (((residual->edge_u[e] == left && residual->edge_v[e] == right) ||
         (residual->edge_u[e] == right && residual->edge_v[e] == left))) {
      return true;
    }
  }
  return false;
}

static bool residual_active_neighbors(const qsop_residual_t *residual, uint32_t v,
                                      uint32_t *neighbors, uint8_t *is_neighbor,
                                      uint32_t *nneighbors) {
  *nneighbors = 0;
  for (uint32_t slot = residual->active_incident_head[v]; slot != RESIDUAL_NIL;
       slot = residual->incident_next[slot]) {
    const uint32_t e = residual->incident_edge[slot];
    const uint32_t other = residual->edge_u[e] == v ? residual->edge_v[e] : residual->edge_u[e];
    if (other != v && residual->active_var[other] != 0 && is_neighbor[other] == 0) {
      is_neighbor[other] = 1;
      neighbors[(*nneighbors)++] = other;
    }
  }
  return true;
}

bool qsop_residual_fill_edges_without_var(const qsop_residual_t *residual, uint32_t removed,
                                          uint64_t *out, qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null residual fill output");
    return false;
  }
  *out = 0;
  if (!validate_active_variable(residual, removed, "removed", error)) {
    return false;
  }

  uint32_t *neighbors = calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*neighbors));
  uint8_t *is_neighbor = calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*is_neighbor));
  if (neighbors == NULL || is_neighbor == NULL) {
    free(neighbors);
    free(is_neighbor);
    set_error(error, "out of memory while estimating residual fill");
    return false;
  }

  uint32_t nneighbors = 0;
  residual_active_neighbors(residual, removed, neighbors, is_neighbor, &nneighbors);
  uint64_t fill = 0;
  for (uint32_t i = 0; i < nneighbors; i++) {
    for (uint32_t j = i + 1U; j < nneighbors; j++) {
      if (!residual_active_edge_between(residual, neighbors[i], neighbors[j])) {
        fill++;
      }
    }
  }

  free(neighbors);
  free(is_neighbor);
  *out = fill;
  return true;
}

static uint64_t residual_min_fill_edges_for(uint32_t v, const uint64_t *adj, size_t words,
                                            uint32_t nvars, uint32_t *neighbors) {
  const uint64_t *row = qsop_bitset_const_row(adj, words, v);
  uint32_t nneighbors = 0;
  for (uint32_t u = 0; u < nvars; u++) {
    if (qsop_bitset_get(row, u)) {
      neighbors[nneighbors++] = u;
    }
  }

  uint64_t fill = 0;
  for (uint32_t i = 0; i < nneighbors; i++) {
    const uint64_t *neighbor_row = qsop_bitset_const_row(adj, words, neighbors[i]);
    for (uint32_t j = i + 1U; j < nneighbors; j++) {
      if (!qsop_bitset_get(neighbor_row, neighbors[j])) {
        fill++;
      }
    }
  }
  return fill;
}

bool qsop_residual_min_fill_width_without_var(const qsop_residual_t *residual,
                                              uint32_t removed, uint32_t *out,
                                              qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null residual min-fill width output");
    return false;
  }
  *out = 0;
  if (!validate_active_variable(residual, removed, "removed", error)) {
    return false;
  }

  if (residual->active_vars <= 1U) {
    return true;
  }

  const uint32_t n = residual->active_vars - 1U;
  const size_t words = qsop_bitset_words(n);
  uint32_t *map = malloc((residual->nvars == 0 ? 1U : residual->nvars) * sizeof(*map));
  uint32_t *neighbors = malloc((n == 0 ? 1U : n) * sizeof(*neighbors));
  uint8_t *active = calloc(n == 0 ? 1U : n, sizeof(*active));
  uint64_t *adj = calloc((n == 0 ? 1U : (size_t)n) * words, sizeof(*adj));
  if (map == NULL || neighbors == NULL || active == NULL || adj == NULL) {
    free(map);
    free(neighbors);
    free(active);
    free(adj);
    set_error(error, "out of memory while computing residual min-fill probe");
    return false;
  }

  for (uint32_t v = 0; v < residual->nvars; v++) {
    map[v] = UINT32_MAX;
  }
  uint32_t next = 0;
  for (uint32_t v = 0; v < residual->nvars; v++) {
    if (v != removed && residual->active_var[v] != 0) {
      map[v] = next++;
      active[map[v]] = 1U;
    }
  }

  for (uint32_t e = 0; e < residual->nedges; e++) {
    if (residual->active_edge[e] == 0) {
      continue;
    }
    const uint32_t u = residual->edge_u[e];
    const uint32_t v = residual->edge_v[e];
    if (u == removed || v == removed || map[u] == UINT32_MAX || map[v] == UINT32_MAX) {
      continue;
    }
    qsop_bitset_set(qsop_bitset_row(adj, words, map[u]), map[v]);
    qsop_bitset_set(qsop_bitset_row(adj, words, map[v]), map[u]);
  }

  uint32_t width = 0;
  for (uint32_t step = 0; step < n; step++) {
    bool found = false;
    uint32_t best = 0;
    uint64_t best_fill = UINT64_MAX;
    uint32_t best_degree = UINT32_MAX;
    for (uint32_t v = 0; v < n; v++) {
      if (active[v] == 0) {
        continue;
      }
      const uint64_t *row = qsop_bitset_const_row(adj, words, v);
      uint32_t degree = qsop_bitset_popcount(row, words);
      const uint64_t fill = residual_min_fill_edges_for(v, adj, words, n, neighbors);
      if (!found || fill < best_fill || (fill == best_fill && degree < best_degree)) {
        found = true;
        best = v;
        best_fill = fill;
        best_degree = degree;
      }
    }
    if (!found) {
      break;
    }
    if (best_degree > width) {
      width = best_degree;
    }

    uint32_t nneighbors = 0;
    const uint64_t *best_row = qsop_bitset_const_row(adj, words, best);
    for (uint32_t u = 0; u < n; u++) {
      if (qsop_bitset_get(best_row, u)) {
        neighbors[nneighbors++] = u;
      }
    }
    for (uint32_t i = 0; i < nneighbors; i++) {
      for (uint32_t j = i + 1U; j < nneighbors; j++) {
        qsop_bitset_set(qsop_bitset_row(adj, words, neighbors[i]), neighbors[j]);
        qsop_bitset_set(qsop_bitset_row(adj, words, neighbors[j]), neighbors[i]);
      }
    }
    for (uint32_t v = 0; v < n; v++) {
      qsop_bitset_clear(qsop_bitset_row(adj, words, v), best);
    }
    qsop_bitset_zero(qsop_bitset_row(adj, words, best), words);
    active[best] = 0U;
  }

  free(map);
  free(neighbors);
  free(active);
  free(adj);
  *out = width;
  return true;
}

static uint32_t gf2_rank(uint64_t *rows, uint32_t nrows, uint32_t ncols, uint32_t nwords) {
  uint32_t rank = 0;
  for (uint32_t col = 0; col < ncols && rank < nrows; col++) {
    const uint32_t word = col / 64U;
    const uint64_t mask = UINT64_C(1) << (col % 64U);
    uint32_t pivot = rank;
    while (pivot < nrows && (rows[(size_t)pivot * nwords + word] & mask) == 0) {
      pivot++;
    }
    if (pivot == nrows) {
      continue;
    }
    if (pivot != rank) {
      for (uint32_t w = 0; w < nwords; w++) {
        const size_t a = (size_t)rank * nwords + w;
        const size_t b = (size_t)pivot * nwords + w;
        const uint64_t tmp = rows[a];
        rows[a] = rows[b];
        rows[b] = tmp;
      }
    }
    for (uint32_t row = 0; row < nrows; row++) {
      if (row == rank || (rows[(size_t)row * nwords + word] & mask) == 0) {
        continue;
      }
      for (uint32_t w = 0; w < nwords; w++) {
        rows[(size_t)row * nwords + w] ^= rows[(size_t)rank * nwords + w];
      }
    }
    rank++;
  }
  return rank;
}

bool qsop_residual_neighbor_cut_rank(const qsop_residual_t *residual, uint32_t v, uint32_t *out,
                                     qsop_error_t *error) {
  if (out == NULL) {
    set_error(error, "internal error: null residual cut-rank output");
    return false;
  }
  *out = 0;
  if (!validate_active_variable(residual, v, "rankwidth heuristic", error)) {
    return false;
  }

  uint32_t *neighbors = calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*neighbors));
  uint8_t *is_neighbor = calloc(residual->nvars == 0 ? 1U : residual->nvars, sizeof(*is_neighbor));
  uint32_t *col_index = malloc((residual->nvars == 0 ? 1U : residual->nvars) * sizeof(*col_index));
  if (neighbors == NULL || is_neighbor == NULL || col_index == NULL) {
    free(neighbors);
    free(is_neighbor);
    free(col_index);
    set_error(error, "out of memory while estimating residual cut-rank");
    return false;
  }

  uint32_t nneighbors = 0;
  residual_active_neighbors(residual, v, neighbors, is_neighbor, &nneighbors);
  for (uint32_t i = 0; i < residual->nvars; i++) {
    col_index[i] = UINT32_MAX;
  }
  uint32_t ncols = 0;
  for (uint32_t u = 0; u < residual->nvars; u++) {
    if (u != v && residual->active_var[u] != 0 && is_neighbor[u] == 0) {
      col_index[u] = ncols++;
    }
  }

  if (nneighbors == 0 || ncols == 0) {
    free(neighbors);
    free(is_neighbor);
    free(col_index);
    return true;
  }

  const uint32_t nwords = (ncols + 63U) / 64U;
  uint64_t *rows = calloc((size_t)nneighbors * nwords, sizeof(*rows));
  if (rows == NULL) {
    free(neighbors);
    free(is_neighbor);
    free(col_index);
    set_error(error, "out of memory while allocating residual cut-rank matrix");
    return false;
  }

  for (uint32_t row = 0; row < nneighbors; row++) {
    const uint32_t u = neighbors[row];
    for (uint32_t slot = residual->active_incident_head[u]; slot != RESIDUAL_NIL;
         slot = residual->incident_next[slot]) {
      const uint32_t e = residual->incident_edge[slot];
      const uint32_t other = residual->edge_u[e] == u ? residual->edge_v[e] : residual->edge_u[e];
      if (other < residual->nvars && col_index[other] != UINT32_MAX) {
        const uint32_t col = col_index[other];
        rows[(size_t)row * nwords + col / 64U] |= UINT64_C(1) << (col % 64U);
      }
    }
  }

  *out = gf2_rank(rows, nneighbors, ncols, nwords);
  free(rows);
  free(neighbors);
  free(is_neighbor);
  free(col_index);
  return true;
}

bool qsop_residual_var_active(const qsop_residual_t *residual, uint32_t v) {
  return residual != NULL && v < residual->nvars && residual->active_var[v] != 0;
}

bool qsop_residual_edge_active(const qsop_residual_t *residual, uint32_t e) {
  return residual != NULL && e < residual->nedges && residual->active_edge[e] != 0;
}
