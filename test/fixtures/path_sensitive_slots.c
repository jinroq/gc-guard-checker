#include "ruby/ruby.h"

/* As in pack_pack of CRuby: from is converted with StringValue, so its slot
 * stays in memory. In the next iteration, from is overwritten, and when it is
 * nil the call receives "" rather than a pointer into the string, so the
 * string being out of the slot there does not matter. On the path where the
 * call receives a pointer into the string, the slot holds it. Not reported. */
VALUE
ok_slot_overwritten_on_path_without_pointer(VALUE ary, VALUE res)
{
    VALUE from;
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
        }
        rb_str_cat(res, ptr, len);
    }
    return res;
}

/* When from is nil, the pointer into the string of a previous iteration is
 * used again, while the slot has been overwritten with nil. Reported
 * (boundary). */
VALUE
bad_pointer_kept_after_slot_overwritten(VALUE ary, VALUE res)
{
    VALUE from;
    const char *ptr = "";
    long len = 0;
    long n = RARRAY_LEN(ary);
    for (long i = 0; i < n; i++) {
        from = RARRAY_AREF(ary, i);
        if (!NIL_P(from)) {
            StringValue(from);
            ptr = RSTRING_PTR(from);
            len = RSTRING_LEN(from);
        }
        rb_str_cat(res, ptr, len);
    }
    return res;
}

/* A pointer into the string is saved to a slot whose address escapes. A
 * pointer into an object in the stack does not keep it alive, so the string
 * is not kept. Reported. */
void save_pointer(const char **slot);

long
bad_interior_pointer_in_escaped_slot(void)
{
    VALUE s = rb_str_new_cstr("x");
    const char *p = RSTRING_PTR(s);
    const char *saved = p;
    save_pointer(&saved);
    rb_ary_new();
    return p[0];
}

/* The address of the slot of s is passed to a call, which may store another
 * value there (as rb_string_value(&v) stores the converted value). s may not
 * be in the slot at the GC call after that. Reported. */
void may_overwrite(VALUE *slot);

long
bad_slot_overwritten_by_call(void)
{
    VALUE s = rb_str_new_cstr("x");
    const char *p = RSTRING_PTR(s);
    may_overwrite(&s);
    rb_ary_new();
    return p[0];
}

/* RB_GC_GUARD passes the address of the slot to an asm, which does not store
 * there. Not reported (boundary). */
long
ok_slot_address_to_guard(void)
{
    VALUE s = rb_str_new_cstr("x");
    const char *p = RSTRING_PTR(s);
    RB_GC_GUARD(s);
    rb_ary_new();
    return p[0];
}

/* v = rb_io_taint_check(from) returns from as is, so from, loaded from its
 * slot before v is defined, is v too, and the slot holds it. As in
 * ok_slot_overwritten_on_path_without_pointer, the slot is overwritten only
 * on paths where the call receives "". Not reported. */
VALUE rb_io_taint_check(VALUE io);

VALUE
ok_alias_defined_before_value(VALUE ary, VALUE res)
{
    VALUE from;
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
            VALUE v = rb_io_taint_check(from);
            ptr = RSTRING_PTR(v);
            len = RSTRING_LEN(v);
        }
        rb_str_cat(res, ptr, len);
    }
    return res;
}
