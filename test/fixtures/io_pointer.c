#include "ruby/ruby.h"
#include "ruby/io.h"

/* GetOpenFile は rb_io_taint_check と rb_io_check_closed を呼ぶ。どちらも
 * GC が起こりうるのは raise して戻らない経路だけなので報告しない */
VALUE ok_get_open_file(VALUE io)
{
    rb_io_t *fptr;
    GetOpenFile(io, fptr);
    return INT2FIX(fptr->fd);
}

/* rb_io_taint_check は第1引数をそのまま返すので、戻り値の origin は
 * 引数になる */
VALUE bad_origin_through_returned_argument(VALUE io)
{
    VALUE x = rb_io_taint_check(io);
    rb_io_t *fptr = RFILE(x)->fptr;
    rb_ary_new();
    return INT2FIX(fptr->fd);
}
