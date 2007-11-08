/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * Generic policy interface for SAT solver
 * 
 */

#include "solver.h"

int policy_printrules( void );

int policy_init( const Pool *pool );
int policy_exit( void );
