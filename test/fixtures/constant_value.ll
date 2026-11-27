; v is a constant expression (the address of a global), not computed on the
; paths. The confirmation on one path still takes it as v, so the report is
; kept as by the other checks. Reported.
@g = global [32 x i8] zeroinitializer

declare i64 @rb_ary_new()

define i8 @bad_constant_expression_value() {
entry:
  %obj = inttoptr i64 ptrtoint (ptr @g to i64) to ptr
  %ptr.field = getelementptr i8, ptr %obj, i64 24
  %ptr = load ptr, ptr %ptr.field
  %a = call i64 @rb_ary_new()
  %c = load i8, ptr %ptr
  ret i8 %c
}
