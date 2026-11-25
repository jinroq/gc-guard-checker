#include "ruby/ruby.h"

/* 報告には v の出どころ (origin) を付け、選別の優先度に使う */

/* 呼び出しの戻り値: この関数のフレームからしか参照されていない可能性が高い */
VALUE bad_origin_call(VALUE obj)
{
    VALUE str = rb_obj_as_string(obj);
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* 引数: VM のスタックなど呼び出し元から到達可能なことが多い */
VALUE bad_origin_argument(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* メモリからの読み出し: 読み出し元から到達可能なことが多い */
VALUE bad_origin_loaded(VALUE *slot)
{
    VALUE str = *slot;
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* phi の incoming に呼び出しの戻り値があれば call とみなす */
VALUE bad_origin_phi_with_call(VALUE obj, VALUE flag)
{
    VALUE str = RTEST(flag) ? rb_obj_as_string(obj) : obj;
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}
