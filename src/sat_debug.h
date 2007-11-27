/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 *
 * debug.h
 * general logging function
 *
 */

#ifndef _SAT_DEBUG_H
#define _SAT_DEBUG_H

#include <stdarg.h>
#include <stdio.h>

typedef enum {
    NONE     = -2,
    ALWAYS   = -1,
    ERROR    = 0,
    DEBUG_1  = 1,
    DEBUG_2  = 2,
    DEBUG_3  = 3,
    DEBUG_4  = 4,
    DEBUG_5  = 5
} DebugLevel;

// Callback for logging
typedef void (*SatDebugFn) (char *logString);
void sat_set_debugCallback (SatDebugFn callback);

// debug level
void sat_set_debug (DebugLevel level, int log_line_nr);
DebugLevel sat_debug_level ();

// logging a line
void sat_debug (DebugLevel  level, const char *format, ...);

#endif /* _SAT_DEBUG_H */
