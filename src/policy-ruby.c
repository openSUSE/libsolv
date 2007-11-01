/*
 * Ruby policy backend for SAT solver
 * 
 */
#include <stdio.h>
#include "policy.h"

#include "ruby.h"

static const Pool *pool;
static VALUE cPolicy;

int
policy_init( const Pool *p )
{
  ruby_init();
  ruby_init_loadpath();
  rb_set_safe_level(0); //FIXME

  /* give the ruby code a name */
  ruby_script("satsolver_policy");

  cPolicy = rb_define_class( "SatPolicy", rb_cObject );

  /* load the policy implementation */
  rb_require( "satsolver_policy" );
  
  pool = p;

  return 0;
}


int
policy_exit( void )
{
  pool = NULL;
  return 0;
}

/*-----------------------------------------------*/
int
policy_printrules( void )
{
  static VALUE id = Qnil;
  
  /* check if ruby implementation available */
  if (NIL_P( id )) {
    id = rb_intern( "printrules" );
    if (rb_respond_to( cPolicy, id ) == Qfalse) {
      id = Qfalse;
    }
  }
  
  /* call ruby, if available */
  if (RTEST( id )) {
    return RTEST( rb_funcall( cPolicy, id, 0 ) );
  }

  /* default: false */
  return 0;
}
