/*
 * km_capture_scope.h - the standard glue between km_capture (core) and the capture ops of the scope command (km_scope.h).
 * Header only, so that neither library needs the other at link time. A diagnostic image does:
 *
 *     static km_capture_t g_cap;
 *     km_capture_init(&g_cap, 240);
 *     km_scope_set_capture(&km_capture_scope_ops, &g_cap);
 */
#ifndef KM_CAPTURE_SCOPE_H
#define KM_CAPTURE_SCOPE_H

#include "km/km_capture.h"
#include "km/km_scope.h"

static bool km_capture_scope_select(void *ctx, unsigned column, unsigned slot)
{
    return km_capture_select((km_capture_t *)ctx, column, slot);
}

static bool km_capture_scope_control(void *ctx, unsigned ctrl)
{
    return km_capture_control((km_capture_t *)ctx, ctrl);
}

static unsigned km_capture_scope_status(void *ctx, uint16_t out[KM_SCOPE_CAP_STATUS_MAX])
{
    km_capture_status((const km_capture_t *)ctx, out);
    return KM_CAPTURE_STATUS_WORDS;
}

static unsigned km_capture_scope_value_count(void *ctx)
{
    return km_capture_value_count((const km_capture_t *)ctx);
}

static unsigned km_capture_scope_read(void *ctx, unsigned first, unsigned n, uint16_t *out)
{
    return km_capture_read((const km_capture_t *)ctx, first, n, out);
}

static unsigned km_capture_scope_run(void *ctx)
{
    return ((const km_capture_t *)ctx)->run;
}

static const km_scope_capture_t km_capture_scope_ops = {
    km_capture_scope_select, km_capture_scope_control, km_capture_scope_status,
    km_capture_scope_value_count, km_capture_scope_read, km_capture_scope_run,
};

#endif /* KM_CAPTURE_SCOPE_H */
