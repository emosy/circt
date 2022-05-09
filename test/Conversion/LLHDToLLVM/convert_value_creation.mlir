// NOTE: Assertions have been autogenerated by utils/generate-test-checks.py
// RUN: circt-opt %s --convert-llhd-to-llvm | FileCheck %s

// CHECK-LABEL: @convert_const
llvm.func @convert_const() {
  // CHECK-NEXT: %{{.*}} = llvm.mlir.constant(true) : i1
  %0 = hw.constant 1 : i1

  // CHECK-NEXT %{{.*}} = llvm.mlir.constant(0 : i32) : i32
  %1 = hw.constant 0 : i32

  // this gets erased
  %2 = llhd.constant_time #llhd.time<0ns, 0d, 0e>

  // CHECK-NEXT %{{.*}} = llvm.mlir.constant(123 : i64) : i64
  %3 = hw.constant 123 : i64

  llvm.return
}

// CHECK-LABEL:   llvm.func @convert_array(
// CHECK-SAME:                          %[[VAL_0:.*]]: i1,
// CHECK-SAME:                          %[[VAL_1:.*]]: i32) {
// CHECK:           %[[VAL_2:.*]] = llvm.mlir.undef : !llvm.array<3 x i1>
// CHECK:           %[[VAL_3:.*]] = llvm.insertvalue %[[VAL_0]], %[[VAL_2]][0 : i32] : !llvm.array<3 x i1>
// CHECK:           %[[VAL_4:.*]] = llvm.insertvalue %[[VAL_0]], %[[VAL_3]][1 : i32] : !llvm.array<3 x i1>
// CHECK:           %[[VAL_5:.*]] = llvm.insertvalue %[[VAL_0]], %[[VAL_4]][2 : i32] : !llvm.array<3 x i1>
// CHECK:           %[[VAL_6:.*]] = llvm.mlir.undef : !llvm.array<4 x i32>
// CHECK:           %[[VAL_7:.*]] = llvm.insertvalue %[[VAL_1]], %[[VAL_6]][0 : i32] : !llvm.array<4 x i32>
// CHECK:           %[[VAL_8:.*]] = llvm.insertvalue %[[VAL_1]], %[[VAL_7]][1 : i32] : !llvm.array<4 x i32>
// CHECK:           %[[VAL_9:.*]] = llvm.insertvalue %[[VAL_1]], %[[VAL_8]][2 : i32] : !llvm.array<4 x i32>
// CHECK:           %[[VAL_10:.*]] = llvm.insertvalue %[[VAL_1]], %[[VAL_9]][3 : i32] : !llvm.array<4 x i32>
// CHECK:           llvm.return
// CHECK:         }
func.func @convert_array(%ci1 : i1, %ci32 : i32) {
  %0 = hw.array_create %ci1, %ci1, %ci1 : i1
  %1 = hw.array_create %ci32, %ci32, %ci32, %ci32 : i32

  return
}

// CHECK-LABEL:   llvm.func @convert_tuple(
// CHECK-SAME:                             %[[VAL_0:.*]]: i1,
// CHECK-SAME:                             %[[VAL_1:.*]]: i2,
// CHECK-SAME:                             %[[VAL_2:.*]]: i3) {
// CHECK:           %[[VAL_3:.*]] = llvm.mlir.undef : !llvm.struct<(i3, i2, i1)>
// CHECK:           %[[VAL_4:.*]] = llvm.insertvalue %[[VAL_2]], %[[VAL_3]][0 : i32] : !llvm.struct<(i3, i2, i1)>
// CHECK:           %[[VAL_5:.*]] = llvm.insertvalue %[[VAL_1]], %[[VAL_4]][1 : i32] : !llvm.struct<(i3, i2, i1)>
// CHECK:           %[[VAL_6:.*]] = llvm.insertvalue %[[VAL_0]], %[[VAL_5]][2 : i32] : !llvm.struct<(i3, i2, i1)>
// CHECK:           llvm.return
// CHECK:         }
func.func @convert_tuple(%ci1 : i1, %ci2 : i2, %ci3 : i3) {
  %0 = hw.struct_create (%ci1, %ci2, %ci3) : !hw.struct<foo: i1, bar: i2, baz: i3>

  return
}
