//===- StaticLogicToCalyx.h - StaticLogic to Calyx pass entry point -----*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This header file defines prototypes that expose the StaticLogicToCalyx pass
// constructor.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_CONVERSION_STATICLOGICTOCALYX_H
#define CIRCT_CONVERSION_STATICLOGICTOCALYX_H

#include "circt/Support/LLVM.h"
#include "mlir/Dialect/SCF/SCF.h"
#include <memory>

namespace circt {

/// Create a StaticLogic to Calyx conversion pass.
std::unique_ptr<OperationPass<ModuleOp>> createStaticLogicToCalyxPass();

} // namespace circt

#endif // CIRCT_CONVERSION_STATICLOGICTOCALYX_H
