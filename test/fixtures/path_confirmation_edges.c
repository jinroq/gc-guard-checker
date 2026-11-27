#include "ruby/ruby.h"

void escape(VALUE *slot);

/* v is defined by a phi of two new strings. Reported. */
long
bad_value_defined_by_phi(int c)
{
    VALUE v = c ? rb_str_new_cstr("a") : rb_str_new_cstr("bb");
    const char *p = RSTRING_PTR(v);
    rb_ary_new();
    return p[0];
}

/* The lifetime of the slot holding s ends at the end of the block, before the
 * GC call. Reported. */
long
bad_slot_lifetime_ended(void)
{
    const char *p;
    {
        VALUE s = rb_str_new_cstr("x");
        escape(&s);
        p = RSTRING_PTR(s);
    }
    rb_ary_new();
    return p[0];
}

/* The GC call is in the block, where the slot holds s. Not reported
 * (boundary). */
long
ok_slot_lifetime_not_ended(void)
{
    const char *p;
    long r;
    {
        VALUE s = rb_str_new_cstr("x");
        escape(&s);
        p = RSTRING_PTR(s);
        rb_ary_new();
        r = p[0];
    }
    return r;
}

/* The same as ok_slot_overwritten_on_path_without_pointer in
 * path_sensitive_slots.c, but with more pointers derived from the string than
 * the confirmation on one path tracks. It then gives up and keeps the report,
 * which is a false one here. Reported (boundary of the limit). */
VALUE
bad_kept_over_tracking_limit(VALUE ary, VALUE res)
{
    VALUE from;
    long sum = 0;
    long n = RARRAY_LEN(ary);
    for (long i = 0; i < n; i++) {
        const char *ptr;
        long len;
        from = RARRAY_AREF(ary, i);
        if (NIL_P(from)) {
            ptr = "";
            len = 0;
        }
        else {
            StringValue(from);
            ptr = RSTRING_PTR(from);
            len = RSTRING_LEN(from);
            sum += ((volatile const char *)ptr)[0];
            sum += ((volatile const char *)ptr)[1];
            sum += ((volatile const char *)ptr)[2];
            sum += ((volatile const char *)ptr)[3];
            sum += ((volatile const char *)ptr)[4];
            sum += ((volatile const char *)ptr)[5];
            sum += ((volatile const char *)ptr)[6];
            sum += ((volatile const char *)ptr)[7];
            sum += ((volatile const char *)ptr)[8];
            sum += ((volatile const char *)ptr)[9];
            sum += ((volatile const char *)ptr)[10];
            sum += ((volatile const char *)ptr)[11];
            sum += ((volatile const char *)ptr)[12];
            sum += ((volatile const char *)ptr)[13];
            sum += ((volatile const char *)ptr)[14];
            sum += ((volatile const char *)ptr)[15];
            sum += ((volatile const char *)ptr)[16];
            sum += ((volatile const char *)ptr)[17];
            sum += ((volatile const char *)ptr)[18];
            sum += ((volatile const char *)ptr)[19];
            sum += ((volatile const char *)ptr)[20];
            sum += ((volatile const char *)ptr)[21];
            sum += ((volatile const char *)ptr)[22];
            sum += ((volatile const char *)ptr)[23];
            sum += ((volatile const char *)ptr)[24];
            sum += ((volatile const char *)ptr)[25];
            sum += ((volatile const char *)ptr)[26];
            sum += ((volatile const char *)ptr)[27];
            sum += ((volatile const char *)ptr)[28];
            sum += ((volatile const char *)ptr)[29];
            sum += ((volatile const char *)ptr)[30];
            sum += ((volatile const char *)ptr)[31];
            sum += ((volatile const char *)ptr)[32];
            sum += ((volatile const char *)ptr)[33];
            sum += ((volatile const char *)ptr)[34];
            sum += ((volatile const char *)ptr)[35];
            sum += ((volatile const char *)ptr)[36];
            sum += ((volatile const char *)ptr)[37];
            sum += ((volatile const char *)ptr)[38];
            sum += ((volatile const char *)ptr)[39];
            sum += ((volatile const char *)ptr)[40];
            sum += ((volatile const char *)ptr)[41];
            sum += ((volatile const char *)ptr)[42];
            sum += ((volatile const char *)ptr)[43];
            sum += ((volatile const char *)ptr)[44];
            sum += ((volatile const char *)ptr)[45];
            sum += ((volatile const char *)ptr)[46];
            sum += ((volatile const char *)ptr)[47];
            sum += ((volatile const char *)ptr)[48];
            sum += ((volatile const char *)ptr)[49];
            sum += ((volatile const char *)ptr)[50];
            sum += ((volatile const char *)ptr)[51];
            sum += ((volatile const char *)ptr)[52];
            sum += ((volatile const char *)ptr)[53];
            sum += ((volatile const char *)ptr)[54];
            sum += ((volatile const char *)ptr)[55];
            sum += ((volatile const char *)ptr)[56];
            sum += ((volatile const char *)ptr)[57];
            sum += ((volatile const char *)ptr)[58];
            sum += ((volatile const char *)ptr)[59];
            sum += ((volatile const char *)ptr)[60];
            sum += ((volatile const char *)ptr)[61];
            sum += ((volatile const char *)ptr)[62];
            sum += ((volatile const char *)ptr)[63];
            sum += ((volatile const char *)ptr)[64];
            sum += ((volatile const char *)ptr)[65];
            sum += ((volatile const char *)ptr)[66];
            sum += ((volatile const char *)ptr)[67];
            sum += ((volatile const char *)ptr)[68];
            sum += ((volatile const char *)ptr)[69];
        }
        rb_str_cat(res, ptr, len);
    }
    return LONG2FIX(sum);
}
