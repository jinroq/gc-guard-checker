; s is in a slot whose address escapes (to an asm, as by RB_GC_GUARD), and the
; pointer loaded from the slot (s itself, not the address of the slot) is
; passed to a call. The call does not receive the slot, so it cannot store
; there, and s stays in it. Not reported.
declare i64 @rb_str_new_cstr(ptr)
declare i64 @rb_ary_new()
declare void @look(ptr)

@.str = private constant [2 x i8] c"x\00"

define i8 @ok_value_loaded_from_slot_to_call() {
entry:
  %slot = alloca i64
  %s = call i64 @rb_str_new_cstr(ptr @.str)
  store i64 %s, ptr %slot
  call void asm sideeffect "", "r,~{memory}"(ptr %slot)
  %obj = inttoptr i64 %s to ptr
  %ptr.field = getelementptr i8, ptr %obj, i64 24
  %ptr = load ptr, ptr %ptr.field
  %loaded = load ptr, ptr %slot
  call void @look(ptr %loaded)
  %a = call i64 @rb_ary_new()
  %c = load i8, ptr %ptr
  ret i8 %c
}
