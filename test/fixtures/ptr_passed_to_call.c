#include "ruby/ruby.h"
#include <string.h>

/* rb_str_new はオブジェクトを確保してから p をコピーする。確保中の GC で
 * tmp が回収されると、解放済みのメモリからコピーすることになるので報告する */
VALUE bad_ptr_passed_to_allocating_call(VALUE obj)
{
    VALUE tmp = rb_obj_as_string(obj);
    return rb_str_new(RSTRING_PTR(tmp), RSTRING_LEN(tmp));
}

/* 呼び出しの後で tmp を guard しているので報告しない */
VALUE ok_ptr_passed_with_guard(VALUE obj)
{
    VALUE tmp = rb_obj_as_string(obj);
    VALUE r = rb_str_new(RSTRING_PTR(tmp), RSTRING_LEN(tmp));
    RB_GC_GUARD(tmp);
    return r;
}

/* v 自身も同じ呼び出しに渡しているので、呼び出し先が v を保持している。
 * 報告しない */
VALUE ok_value_passed_to_same_call(VALUE obj)
{
    VALUE tmp = rb_obj_as_string(obj);
    return rb_str_cat(tmp, RSTRING_PTR(tmp), 1);
}

/* GC 呼び出しの後で、GC しない memcpy に p を渡して読ませているので報告する */
VALUE bad_ptr_passed_to_memcpy_after_gc(VALUE obj, char *buf, long n)
{
    VALUE tmp = rb_obj_as_string(obj);
    const char *p = RSTRING_PTR(tmp);
    rb_ary_new();
    memcpy(buf, p, n);
    return Qnil;
}

/* GC しない strlen に p を渡した後で GC 呼び出しがあるが、それ以降 p は
 * 使わないので報告しない (境界) */
VALUE ok_ptr_passed_before_gc(VALUE obj)
{
    VALUE tmp = rb_obj_as_string(obj);
    size_t n = strlen(RSTRING_PTR(tmp));
    rb_ary_new();
    return LONG2FIX(n);
}
