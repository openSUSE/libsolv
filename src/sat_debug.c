/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 *
 * debug.c
 * general logging function
 *
 */


#include <sat_debug.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>

#define MAX_OUTPUT_LEN 200

// debug level which can be set
static DebugLevel debug_level = ERROR;
// log file,function,line too
static int sat_log_lineNr = 0;


void
sat_set_debug (DebugLevel level, int log_line_nr)
{
    debug_level = level;
    sat_log_lineNr = log_line_nr;
}

DebugLevel sat_debug_level ()
{
    return debug_level;
}

void
sat_debug (DebugLevel level, const char *format, ...)
{
    va_list args;
    char str[MAX_OUTPUT_LEN];

    va_start (args, format);
    vsnprintf (str, MAX_OUTPUT_LEN, format, args);
    va_end (args);

    if (sat_debug_level() >= level) {
	if (sat_log_lineNr) {
	    char pre[MAX_OUTPUT_LEN];
	    snprintf (pre, MAX_OUTPUT_LEN, "(%s, %s:%d) ", __FUNCTION__, __FILE__, __LINE__);
	    printf("%s", pre);
	}
	printf ("%s", str);
    }
}
