add_circt_library(CIRCTStandardToStaticLogic
  StandardToStaticLogic.cpp

  ADDITIONAL_HEADER_DIRS
  ${MLIR_MAIN_INCLUDE_DIR}/mlir/Conversion/StandardToStaticLogic

  LINK_LIBS PUBLIC
  MLIRIR
  MLIRFunc
  MLIRPass
  MLIRSupport
  MLIRTransforms
  CIRCTStaticLogicOps
  )
