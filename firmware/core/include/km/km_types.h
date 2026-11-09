/*
 * km_types.h - shared constants for the portable core.
 *
 * The core is plain C11 with no hardware, libc-allocation or stdio dependency,
 * so the same sources build on the host (unit tests) and for the keyboard.
 */
#ifndef KM_TYPES_H
#define KM_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define KM_NUM_SLOTS     76u   /* sensor slots: 4 mux phases x 18 samples + 4 injected channels (km_scan.h) */

#endif /* KM_TYPES_H */
