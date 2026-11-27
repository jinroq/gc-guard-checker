; Loops whose phis clang does not reliably produce from C. In each function,
; the pointer p derived from v is used after the GC call (rb_ary_new).

@.str = private unnamed_addr constant [2 x i8] c"a\00", align 1

declare i64 @rb_str_new_static(ptr, i64)
declare i64 @rb_ary_new()
declare void @use(ptr)
declare void @use_value(i64)

; prev = cur; cur = rb_str_new_static(...), where the phi of cur comes before
; the phi of prev. The use of cur by the phi of prev happens at the end of the
; latch, before cur is defined again, so cur is live at the GC call through
; prev in the next iteration. Not reported.
define i64 @ok_value_rotated_through_phis(i64 %n) {
entry:
  %first = call i64 @rb_str_new_static(ptr @.str, i64 1)
  br label %loop

loop:
  %cur = phi i64 [ %first, %entry ], [ %next, %latch ]
  %prev = phi i64 [ 4, %entry ], [ %cur, %latch ]
  %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
  %nil = icmp eq i64 %prev, 4
  br i1 %nil, label %body, label %use.prev

use.prev:
  call void @use_value(i64 %prev)
  br label %body

body:
  %obj = inttoptr i64 %cur to ptr
  %flags = load i64, ptr %obj, align 8, !tbaa !0
  %ptr.field = getelementptr inbounds i8, ptr %obj, i64 24
  %p = load ptr, ptr %ptr.field, align 8
  %gc = call i64 @rb_ary_new()
  call void @use(ptr %p)
  %next = call i64 @rb_str_new_static(ptr @.str, i64 1)
  br label %latch

latch:
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i64 4
}

; v is the same value in every iteration, through a phi of itself. It is used
; in the header of the next iteration, which follows every GC call, so it is
; live there. Not reported.
define i64 @ok_loop_invariant_self_phi(i64 %n) {
entry:
  %s = call i64 @rb_str_new_static(ptr @.str, i64 1)
  br label %loop

loop:
  %v = phi i64 [ %s, %entry ], [ %v, %body ]
  %i = phi i64 [ 0, %entry ], [ %i.next, %body ]
  %obj = inttoptr i64 %v to ptr
  %flags = load i64, ptr %obj, align 8, !tbaa !0
  %ptr.field = getelementptr inbounds i8, ptr %obj, i64 24
  %p = load ptr, ptr %ptr.field, align 8
  %done = icmp sge i64 %i, %n
  br i1 %done, label %exit, label %body

body:
  %gc = call i64 @rb_ary_new()
  call void @use(ptr %p)
  %i.next = add i64 %i, 1
  br label %loop

exit:
  ret i64 4
}

; The same, but the phi takes a new string from the latch, so v in the next
; iteration is a new value and v here is not used after the GC call. Reported
; (boundary).
define i64 @bad_phi_of_new_value(i64 %n) {
entry:
  %s = call i64 @rb_str_new_static(ptr @.str, i64 1)
  br label %loop

loop:
  %v = phi i64 [ %s, %entry ], [ %new, %body ]
  %i = phi i64 [ 0, %entry ], [ %i.next, %body ]
  %obj = inttoptr i64 %v to ptr
  %flags = load i64, ptr %obj, align 8, !tbaa !0
  %ptr.field = getelementptr inbounds i8, ptr %obj, i64 24
  %p = load ptr, ptr %ptr.field, align 8
  %done = icmp sge i64 %i, %n
  br i1 %done, label %exit, label %body

body:
  %gc = call i64 @rb_ary_new()
  %c = load i8, ptr %p, align 1
  %new = call i64 @rb_str_new_static(ptr @.str, i64 1)
  %i.next = add i64 %i, 1
  br label %loop

exit:
  ret i64 4
}

; As in gzreader_skip_linebreaks of the zlib extension: v is loaded again in
; every iteration of a loop calling the GC call, and the pointer is derived
; from it after the loop, for the first time. That pointer is from v loaded
; after the GC call. Not reported.
define i64 @ok_ptr_derived_after_loop(ptr %holder) {
entry:
  br label %loop

loop:
  %v = load i64, ptr %holder, align 8
  %obj = inttoptr i64 %v to ptr
  %flags = load i64, ptr %obj, align 8, !tbaa !0
  %empty = icmp eq i64 %flags, 0
  br i1 %empty, label %more, label %done

more:
  %gc = call i64 @rb_ary_new()
  br label %loop

done:
  %ptr.field = getelementptr inbounds i8, ptr %obj, i64 24
  %p = load ptr, ptr %ptr.field, align 8
  %c = load i8, ptr %p, align 1
  ret i64 4
}

; A store through a new pointer f of the new string, of the pointer carried
; from the previous iteration as the value. The store is not through the
; carried pointer, so it is not an access to v of the previous iteration. Not
; reported.
define i64 @ok_carried_ptr_stored_as_value(i64 %n) {
entry:
  br label %loop

loop:
  %prevp = phi ptr [ null, %entry ], [ %f, %loop ]
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %gc = call i64 @rb_ary_new()
  %v = call i64 @rb_str_new_static(ptr @.str, i64 1)
  %obj = inttoptr i64 %v to ptr
  %flags = load i64, ptr %obj, align 8, !tbaa !0
  %f = getelementptr inbounds i8, ptr %obj, i64 24
  store ptr %prevp, ptr %f, align 8
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i64 4
}

; The GC call receives "" in the first iteration, where v is in no slot, and
; a pointer into v in the next one, which it may read after GC. Reported.
declare void @take(ptr)

define void @bad_gc_call_again_with_pointer(i64 %n) {
entry:
  %s = call i64 @rb_str_new_static(ptr @.str, i64 1)
  %obj = inttoptr i64 %s to ptr
  %ptr.field = getelementptr i8, ptr %obj, i64 24
  br label %loop

loop:
  %p = phi ptr [ @.str, %entry ], [ %ptr.field, %loop ]
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  call void @take(ptr %p)
  %i.next = add i64 %i, 1
  %done = icmp slt i64 %i.next, %n
  br i1 %done, label %loop, label %exit

exit:
  ret void
}

; v = rb_io_taint_check(x) is made from x, computed before v. The call
; receives a pointer into v of the previous iteration, but x there is already
; computed again, a new value, so its use after the call (by v) does not keep
; the previous v. Reported.
declare i64 @rb_io_taint_check(i64)

define void @bad_origin_computed_again_before_call(i64 %n) {
entry:
  br label %loop

loop:
  %p = phi ptr [ @.str, %entry ], [ %ptr.field, %loop ]
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %x = call i64 @rb_str_new_static(ptr @.str, i64 1)
  call void @take(ptr %p)
  %v = call i64 @rb_io_taint_check(i64 %x)
  %obj = inttoptr i64 %v to ptr
  %ptr.field = getelementptr i8, ptr %obj, i64 24
  %i.next = add i64 %i, 1
  %done = icmp slt i64 %i.next, %n
  br i1 %done, label %loop, label %exit

exit:
  ret void
}

; As bad_origin_computed_again_before_call, with a value made from x as is
; after the call and used. It is made from the new x, so it does not keep the
; previous v either. Reported.
define void @bad_value_made_from_origin_after_call(i64 %n) {
entry:
  br label %loop

loop:
  %p = phi ptr [ @.str, %entry ], [ %ptr.field, %loop ]
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %x = call i64 @rb_str_new_static(ptr @.str, i64 1)
  call void @take(ptr %p)
  %v = call i64 @rb_io_taint_check(i64 %x)
  %obj = inttoptr i64 %v to ptr
  %ptr.field = getelementptr i8, ptr %obj, i64 24
  %k = call i64 @rb_io_taint_check(i64 %x)
  %i.k = add i64 %i, %k
  %i.next = add i64 %i.k, 1
  %done = icmp slt i64 %i.next, %n
  br i1 %done, label %loop, label %exit

exit:
  ret void
}

; The same, but the call is after v, where x is still v, and its use after the
; call keeps v for the read through the pointer. Not reported (boundary).
define void @ok_origin_not_computed_again_before_call(i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %x = call i64 @rb_str_new_static(ptr @.str, i64 1)
  %v = call i64 @rb_io_taint_check(i64 %x)
  %obj = inttoptr i64 %v to ptr
  %ptr.field = getelementptr i8, ptr %obj, i64 24
  call void @take(ptr %ptr.field)
  %c = load i8, ptr %ptr.field
  %c.ext = zext i8 %c to i64
  %i.x = add i64 %i, %x
  %i.next = add i64 %i.x, %c.ext
  %done = icmp slt i64 %i.next, %n
  br i1 %done, label %loop, label %exit

exit:
  ret void
}

; As bad_origin_computed_again_before_call, with x a phi of the loop, taking
; the next string of strs at the end of the previous iteration. x at the call
; is already the next one, not the v whose pointer the call receives. Reported.
define void @bad_origin_phi_computed_again_before_call(ptr %strs, i64 %n) {
entry:
  %x0 = call i64 @rb_str_new_static(ptr @.str, i64 1)
  br label %loop

loop:
  %x = phi i64 [ %x0, %entry ], [ %x.next, %loop ]
  %p = phi ptr [ @.str, %entry ], [ %ptr.field, %loop ]
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  call void @take(ptr %p)
  %v = call i64 @rb_io_taint_check(i64 %x)
  %obj = inttoptr i64 %v to ptr
  %ptr.field = getelementptr i8, ptr %obj, i64 24
  %x.slot = getelementptr i64, ptr %strs, i64 %i
  %x.next = load i64, ptr %x.slot
  %i.next = add i64 %i, 1
  %done = icmp slt i64 %i.next, %n
  br i1 %done, label %loop, label %exit

exit:
  ret void
}

!0 = !{!1, !2, i64 0}
!1 = !{!"RBasic", !2, i64 0, !2, i64 8}
!2 = !{!"long", !3, i64 0}
!3 = !{!"omnipotent char", !4, i64 0}
!4 = !{!"Simple C/C++ TBAA"}
