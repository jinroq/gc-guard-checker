#include "ruby/ruby.h"

/* ループの外で取得した p を、ループ内の GC 呼び出しの後で使う。
 * ループ内に str の使用は無いので報告する */
VALUE bad_ptr_outside_loop(VALUE str, VALUE ary)
{
    const char *p = RSTRING_PTR(str);
    long n = RARRAY_LEN(ary);
    long sum = 0;
    for (long i = 0; i < n; i++) {
        rb_ary_new();
        sum += p[i];
    }
    return LONG2FIX(sum);
}

/* p を毎回 str から取り直すので、GC 呼び出しの時点で str は次の反復の
 * ために生きている。ただしループ最後の反復の後には str の使用が無いが、
 * その後 p も使わないので報告しない */
VALUE ok_ptr_reloaded_in_loop(VALUE str, VALUE ary)
{
    long n = RARRAY_LEN(ary);
    long sum = 0;
    for (long i = 0; i < n; i++) {
        const char *p = RSTRING_PTR(str);
        sum += p[i];
        rb_ary_new();
    }
    return LONG2FIX(sum);
}
