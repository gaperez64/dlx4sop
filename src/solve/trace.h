#ifndef DLX4SOP_SOLVE_TRACE_H
#define DLX4SOP_SOLVE_TRACE_H

#include "dlx4sop/qsop_solve.h"

#include <stdint.h>
#include <time.h>

static inline uint64_t qsop_trace_now_ns(void) {
  struct timespec ts = {0};
  if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static inline uint64_t qsop_trace_elapsed_ns(uint64_t start_ns) {
  const uint64_t end_ns = qsop_trace_now_ns();
  if (end_ns < start_ns) {
    return 0;
  }
  return end_ns - start_ns;
}

static inline bool qsop_trace_enabled(const qsop_solve_trace_t *trace) {
  return trace != NULL && trace->emit != NULL;
}

static inline uint64_t qsop_trace_begin(const qsop_solve_trace_t *trace) {
  return qsop_trace_enabled(trace) ? qsop_trace_now_ns() : 0;
}

static inline void qsop_trace_emit(qsop_solve_trace_t *trace, const char *phase, uint32_t depth,
                                   uint64_t items, uint64_t elapsed_ns) {
  if (!qsop_trace_enabled(trace)) {
    return;
  }

  const qsop_solve_trace_event_t event = {
      .phase = phase,
      .depth = depth,
      .items = items,
      .elapsed_ns = elapsed_ns,
  };
  trace->emit(trace->user, &event);
}

static inline void qsop_trace_emit_elapsed(qsop_solve_trace_t *trace, const char *phase,
                                           uint32_t depth, uint64_t items, uint64_t start_ns) {
  if (!qsop_trace_enabled(trace)) {
    return;
  }
  qsop_trace_emit(trace, phase, depth, items, qsop_trace_elapsed_ns(start_ns));
}

#endif
