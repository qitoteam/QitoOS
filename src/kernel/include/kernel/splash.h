/*
 * Qira OS - boot splash
 */
#ifndef QIRA_SPLASH_H
#define QIRA_SPLASH_H

#include <kernel/types.h>

void splash_begin(void);

/* Redraw with a new stage label and completion percentage. */
void splash_update(const char *stage, int percent);

void splash_end(void);
bool_t splash_active(void);
uint64_t splash_elapsed_ms(void);

/* Replace the splash with a failure screen. */
void splash_fail(const char *stage, const char *detail);

#endif /* QIRA_SPLASH_H */
