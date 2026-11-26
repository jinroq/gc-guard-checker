#include "ruby/ruby.h"

#define NOINLINE __attribute__((noinline))

void use(const char *p);
void use_value(VALUE v);

/* s is made in every iteration. A use of s in the next iteration is of a new
 * value, so it does not keep this one alive. Reported. */
VALUE
bad_value_defined_in_loop(long n)
{
    for (long i = 0; i < n; i++) {
        VALUE s = rb_str_new_cstr("x");
        const char *p = RSTRING_PTR(s);
        rb_ary_new();
        use(p);
    }
    return Qnil;
}

/* p is used only before the GC call. The access in the next iteration is
 * through a new pointer from a new string. Not reported. */
VALUE
ok_ptr_used_before_gc_in_loop(long n)
{
    long sum = 0;
    for (long i = 0; i < n; i++) {
        VALUE s = rb_str_new_cstr("x");
        sum += RSTRING_PTR(s)[0];
        rb_ary_new();
    }
    return LONG2FIX(sum);
}

/* s is used after the GC call in the same iteration. Not reported. */
VALUE
ok_value_defined_in_loop_used_after(long n)
{
    for (long i = 0; i < n; i++) {
        VALUE s = rb_str_new_cstr("x");
        const char *p = RSTRING_PTR(s);
        rb_ary_new();
        use(p);
        use_value(s);
    }
    return Qnil;
}

/* s is carried to the next iteration by a phi, where it is used after the GC
 * call. It is the same value, so it is live. Not reported. */
VALUE
ok_value_carried_to_next_iteration(long n)
{
    VALUE prev = Qnil;
    for (long i = 0; i < n; i++) {
        if (!NIL_P(prev))
            use_value(prev);
        VALUE s = rb_str_new_cstr("x");
        const char *p = RSTRING_PTR(s);
        rb_ary_new();
        use(p);
        prev = s;
    }
    return prev;
}

/* s is stored to an escaped slot after the GC call. At the GC call the slot
 * holds the value of the previous iteration, but the store is a use of s
 * after the GC call, so s is live there. Not reported. */
void escape(VALUE *slot);

VALUE
ok_stored_to_slot_after_gc_in_loop(long n)
{
    VALUE slot = Qnil;
    escape(&slot);
    for (long i = 0; i < n; i++) {
        VALUE s = rb_str_new_cstr("x");
        const char *p = RSTRING_PTR(s);
        rb_ary_new();
        use(p);
        slot = s;
    }
    return Qnil;
}

/* Stored before the GC call, so it is in the slot at the GC call. Not
 * reported. */
VALUE
ok_stored_to_slot_before_gc_in_loop(long n)
{
    VALUE slot = Qnil;
    escape(&slot);
    for (long i = 0; i < n; i++) {
        VALUE s = rb_str_new_cstr("x");
        slot = s;
        const char *p = RSTRING_PTR(s);
        rb_ary_new();
        use(p);
    }
    return Qnil;
}

/* s is reassigned in every iteration and guarded, and loaded from its slot
 * after the call in between. The reassignment in the next iteration is
 * followed by a new load of s, so it does not overwrite the slot between the
 * load and the GC call. Not reported. */
VALUE
ok_guarded_value_reassigned_in_loop(long n)
{
    for (long i = 0; i < n; i++) {
        VALUE s = rb_str_new_cstr("x");
        use("reload s from its slot");
        const char *p = RSTRING_PTR(s);
        rb_ary_new();
        use(p);
        RB_GC_GUARD(s);
    }
    return Qnil;
}

/* A new string is passed to a static function in every iteration. The call in
 * the next iteration passes a new value, so the caller does not keep this one
 * alive. Reported. */
static NOINLINE void
bad_callee_of_temporary_in_loop(VALUE s)
{
    const char *p = RSTRING_PTR(s);
    rb_ary_new();
    use(p);
}

VALUE
loop_caller(long n)
{
    for (long i = 0; i < n; i++)
        bad_callee_of_temporary_in_loop(rb_str_new_cstr("x"));
    return Qnil;
}
