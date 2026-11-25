#include "ruby/ruby.h"
#include "ruby/io.h"

/* GetOpenFile calls rb_io_taint_check and rb_io_check_closed. Both may GC
 * only on paths that raise and never return, so it is not reported. */
VALUE ok_get_open_file(VALUE io)
{
    rb_io_t *fptr;
    GetOpenFile(io, fptr);
    return INT2FIX(fptr->fd);
}

/* rb_io_taint_check returns its first argument, so the origin of the result
 * is the argument. */
VALUE bad_origin_through_returned_argument(VALUE io)
{
    VALUE x = rb_io_taint_check(io);
    rb_io_t *fptr = RFILE(x)->fptr;
    rb_ary_new();
    return INT2FIX(fptr->fd);
}
