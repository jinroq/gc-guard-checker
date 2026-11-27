; The GC call is between the origin x and v = rb_io_taint_check(x) in a loop.
; On path b, v is stored to the escaped slot and a pointer into it is passed to
; the call in the next iteration, where the slot still holds v. On path a, the
; slot is overwritten, but the call receives "". The call in the first
; iteration, before v is defined, does not take v out of its slots. Not
; reported.
@.str = private constant [2 x i8] c"x\00"

declare i64 @rb_str_new_static(ptr, i64)
declare i64 @rb_io_taint_check(i64)
declare void @take(ptr)
declare void @escape(ptr)

define void @ok_gc_call_before_value_defined(i1 %c) {
entry:
  %slot = alloca i64
  call void @escape(ptr %slot)
  br label %loop

loop:
  %pp = phi ptr [ @.str, %entry ], [ @.str, %a ], [ %ptr.field, %b ]
  %x = call i64 @rb_str_new_static(ptr @.str, i64 1)
  call void @take(ptr %pp)
  %v = call i64 @rb_io_taint_check(i64 %x)
  br i1 %c, label %a, label %b

a:
  store volatile i64 0, ptr %slot
  br label %loop

b:
  store volatile i64 %v, ptr %slot
  %obj = inttoptr i64 %v to ptr
  %ptr.field = getelementptr i8, ptr %obj, i64 24
  br label %loop
}
