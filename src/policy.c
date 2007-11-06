/*
 * Generic policy interface for SAT solver
 * 
 */

#include "solver.h"

#include <stdio.h>
#include "policy.h"

int
policy_init( const Pool *p )
{
  return 0;
}


int
policy_exit( void )
{
  return 0;
}

/*-----------------------------------------------*/
int
policy_printrules( void )
{
  /* default: false */
  return 0;
}
