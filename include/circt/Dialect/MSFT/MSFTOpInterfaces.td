//===- MSFTOpInstances.td - MSFT OpInstance definitions ----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

include "mlir/IR/OpBase.td"

def DynInstDataOpInterface : OpInterface<"DynInstDataOpInterface"> {
  let description = [{
    Interface for anything which needs to refer to a GlobalRefOp.
  }];
  let cppNamespace = "::circt::msft";
  let verify = [{
    return ::circt::msft::verifyDynInstData($_op);
  }];

  let methods = [
    InterfaceMethod<
      /*desc=*/[{
        Set the GlobalRefOp to which this op is referring.
      }],
      /*retTy=*/"void",
      /*methodName=*/"setGlobalRef",
      /*args=*/(ins "::circt::hw::GlobalRefOp":$ref),
      /*methodBody=*/[{}],
      /*defaultImplementation=*/[{
        $_op.refAttr(FlatSymbolRefAttr::get(ref));
      }]
    >,
    InterfaceMethod<
      /*desc=*/[{
        Get the symbol of the GlobalRefOp to which this op is referring.
      }],
      /*retTy=*/"FlatSymbolRefAttr",
      /*methodName=*/"getGlobalRefSym",
      /*args=*/(ins),
      /*methodBody=*/[{}],
      /*defaultImplementation=*/[{
        return $_op.refAttr();
      }]
    >
  ];
}
