/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/gpu/ir_emission_utils.h"

#include <algorithm>
#include <vector>

#include "llvm/IR/Module.h"
#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_util.h"
#include "tensorflow/compiler/xla/service/llvm_ir/llvm_target_features.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/compiler/xla/window_util.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/protobuf.h"

namespace xla {
namespace gpu {

namespace {

// Return whether the given shape is rank 2 excluding the batch dimensions.
bool IsRank2(const Shape& shape, int64 batch_dimensions_size) {
  return shape.rank() == batch_dimensions_size + 2;
}

// In a gemm operation where output = lhs * rhs, check whether the given shapes
// are valid for the operation.
bool AreValidGemmShapes(const Shape& lhs_shape, const Shape& rhs_shape,
                        const Shape& output_shape,
                        int64 batch_dimensions_size) {
  // The inputs and the output must
  // 1) be matrices with no padding and a non-zero number of elements,
  // 2) have an allowed element type.
  PrimitiveType output_primitive_type = output_shape.element_type();
  bool type_is_allowed =
      (output_primitive_type == F16 || output_primitive_type == F32 ||
       output_primitive_type == F64 || output_primitive_type == C64 ||
       output_primitive_type == C128);
  return type_is_allowed && IsRank2(lhs_shape, batch_dimensions_size) &&
         IsRank2(rhs_shape, batch_dimensions_size) &&
         IsRank2(output_shape, batch_dimensions_size) &&
         !ShapeUtil::IsZeroElementArray(lhs_shape) &&
         !ShapeUtil::IsZeroElementArray(rhs_shape);
}

bool DotImplementedAsGemm(const HloInstruction& dot) {
  CHECK_EQ(dot.opcode(), HloOpcode::kDot);
  const Shape& lhs_shape = dot.operand(0)->shape();
  const Shape& rhs_shape = dot.operand(1)->shape();
  const DotDimensionNumbers& dim_numbers = dot.dot_dimension_numbers();

  // If gemm can accept the operand shapes, use it rather than a custom
  // kernel.
  if (AreValidGemmShapes(lhs_shape, rhs_shape, dot.shape(),
                         dim_numbers.lhs_batch_dimensions_size())) {
    // The size of the reduction dimension should match. The shape inference
    // guarantees this invariant, so the check here is for programming
    // errors.
    CHECK_EQ(lhs_shape.dimensions(dim_numbers.lhs_contracting_dimensions(0)),
             rhs_shape.dimensions(dim_numbers.rhs_contracting_dimensions(0)));
    return true;
  }
  return false;
}
}  // namespace

bool ImplementedAsGemm(const HloInstruction& hlo) {
  // For certain types of Dot, we can call pre-canned BLAS gemm.
  if (hlo.opcode() == HloOpcode::kDot) {
    return DotImplementedAsGemm(hlo);
  }

  if (hlo.opcode() == HloOpcode::kFusion &&
      hlo.fusion_kind() == HloInstruction::FusionKind::kOutput &&
      (hlo.fused_expression_root()->opcode() == HloOpcode::kMultiply ||
       hlo.fused_expression_root()->opcode() == HloOpcode::kAdd)) {
    // Try to find the dot inside the output fusion node.
    const HloInstruction* dot = hlo.fused_expression_root()->operand(0);
    if (dot->opcode() != HloOpcode::kDot) {
      dot = hlo.fused_expression_root()->operand(1);
    }
    if (dot->opcode() == HloOpcode::kDot) {
      return DotImplementedAsGemm(*dot);
    }
  }

  return false;
}

const char* const kCudnnBatchNormForwardInferenceCallTarget =
    "__cudnn$batchNormalizationForwardInference";
const char* const kCudnnBatchNormForwardTrainingCallTarget =
    "__cudnn$batchNormalizationForwardTraining";
const char* const kCudnnBatchNormBackwardCallTarget =
    "__cudnn$batchNormalizationBackward";

bool IsCustomCallToDnnBatchNorm(const HloInstruction& hlo) {
  if (hlo.opcode() != HloOpcode::kCustomCall) {
    return false;
  }
  const auto& target = hlo.custom_call_target();
  return target == kCudnnBatchNormForwardInferenceCallTarget ||
         target == kCudnnBatchNormForwardTrainingCallTarget ||
         target == kCudnnBatchNormBackwardCallTarget;
}

const char* const kCudnnConvForwardCallTarget = "__cudnn$convForward";
const char* const kCudnnConvBackwardInputCallTarget =
    "__cudnn$convBackwardInput";
const char* const kCudnnConvBackwardFilterCallTarget =
    "__cudnn$convBackwardFilter";
const char* const kCudnnConvBiasActivationForwardCallTarget =
    "__cudnn$convBiasActivationForward";

bool IsCustomCallToDnnConvolution(const HloInstruction& hlo) {
  if (hlo.opcode() != HloOpcode::kCustomCall) {
    return false;
  }
  const auto& target = hlo.custom_call_target();
  return target == kCudnnConvForwardCallTarget ||
         target == kCudnnConvBackwardInputCallTarget ||
         target == kCudnnConvBackwardFilterCallTarget ||
         target == kCudnnConvBiasActivationForwardCallTarget;
}

bool ImplementedAsLibraryCall(const HloInstruction& hlo) {
  return ImplementedAsGemm(hlo) || IsCustomCallToDnnBatchNorm(hlo) ||
         IsCustomCallToDnnConvolution(hlo);
}

bool IsReductionToVector(const HloInstruction& reduce) {
  if (HloOpcode::kReduce != reduce.opcode()) {
    return false;
  }
  const HloInstruction* input = reduce.operand(0);
  std::vector<int64> dims_to_keep;
  for (int64 dim = 0; dim < input->shape().dimensions().size(); ++dim) {
    if (!absl::c_linear_search(reduce.dimensions(), dim)) {
      dims_to_keep.push_back(dim);
    }
  }
  return LayoutUtil::AreDimensionsConsecutive(input->shape().layout(),
                                              dims_to_keep) &&
         ShapeUtil::Equal(
             reduce.shape(),
             ShapeUtil::FilterDimensions(
                 [&](int64 dim) { return absl::c_count(dims_to_keep, dim); },
                 input->shape()));
}

llvm::Value* EmitDeviceFunctionCall(
    const string& callee_name, absl::Span<llvm::Value* const> operands,
    absl::Span<const PrimitiveType> input_types, PrimitiveType output_type,
    absl::Span<const llvm::Attribute::AttrKind> attributes,
    llvm::IRBuilder<>* ir_builder, llvm::Module* module) {
  std::vector<llvm::Type*> ir_input_types;
  for (PrimitiveType input_type : input_types) {
    ir_input_types.push_back(
        llvm_ir::PrimitiveTypeToIrType(input_type, module));
  }
  llvm::FunctionType* callee_type = llvm::FunctionType::get(
      llvm_ir::PrimitiveTypeToIrType(output_type, module),  // Return type.
      ir_input_types,                                        // Parameter types.
      false);  // No variadic arguments.

  // Declares the callee if it is not declared already.
  llvm::Function* callee = llvm::dyn_cast<llvm::Function>(
      ir_builder->GetInsertBlock()->getModule()->getOrInsertFunction(
          llvm_ir::AsStringRef(callee_name), callee_type).getCallee());



  for (auto attribute : attributes) {
    callee->addFnAttr(attribute);
  }

  return ir_builder->CreateCall(callee, llvm_ir::AsArrayRef(operands));
}

// This emits a device-side call to
// "i32 vprintf(i8* fmt, arguments_type* arguments)" in the driver; see
// http://docs.nvidia.com/cuda/ptx-writers-guide-to-interoperability/index.html#system-calls
llvm::Value* EmitPrintf(absl::string_view fmt,
                        absl::Span<llvm::Value* const> arguments,
                        llvm::IRBuilder<>* builder) {
  std::vector<llvm::Type*> argument_types;
  for (auto argument : arguments) {
    argument_types.push_back(argument->getType());
  }
  auto* arguments_type = llvm::StructType::create(argument_types);
  llvm::Value* arguments_ptr = builder->CreateAlloca(arguments_type);
  for (size_t i = 0; i < arguments.size(); ++i) {
    builder->CreateStore(
        arguments[i],
        builder->CreateGEP(arguments_ptr,
                           {builder->getInt64(0), builder->getInt32(i)}));
  }
  return builder->CreateCall(
      builder->GetInsertBlock()->getParent()->getParent()->getOrInsertFunction(
          "vprintf",
          llvm::FunctionType::get(builder->getInt32Ty(),
                                  {builder->getInt8Ty()->getPointerTo(),
                                   arguments_type->getPointerTo()},
                                  /*isVarArg=*/false)),
      {builder->CreateGlobalStringPtr(llvm_ir::AsStringRef(fmt)),
       arguments_ptr});
}

llvm::Value* EmitFullWarpShuffleDown(
    llvm::Value* value, llvm::Value* offset,

    llvm_ir::LLVMTargetIRBuilder& llvm_target_ir_builder, 
    llvm::Module* module

) {
  llvm::IRBuilder<>* builder = llvm_target_ir_builder.builder();

  int bit_width = value->getType()->getPrimitiveSizeInBits();
  llvm::Value* all_warps_mask = builder->getInt32(-1);

  // Special case for efficiency
  if (value->getType()->isFloatTy() && bit_width == 32) {
    llvm::Value* value_as_int = builder->CreateBitCast(value, builder->getIntNTy(bit_width));
    llvm::Value*  result = EmitDeviceFunctionCall(
        "__ockl_readuplane_i32",
        {value_as_int, offset},
        {S32, S32}, S32, {}, builder, module);

    llvm::Value* result_as_float = builder->CreateBitCast(result, value->getType());
    return result_as_float;
  }


  // We must split values wider than 32 bits as the "shfl" instruction operates
  // on 32-bit values.
  int num_segments = CeilOfRatio(bit_width, 32);
  llvm::Value* x = builder->CreateBitCast(
      builder->CreateZExt(
          builder->CreateBitCast(value, builder->getIntNTy(bit_width)),
          builder->getIntNTy(32 * num_segments)),
      llvm::VectorType::get(builder->getInt32Ty(), num_segments));
  for (int i = 0; i < num_segments; ++i) {
    x = builder->CreateInsertElement(
        x,
        EmitDeviceFunctionCall("__ockl_readuplane_i32",
                               {builder->CreateExtractElement(x, i),
                                offset},
                               {S32, S32}, S32, {}, builder, module),
        i);
  }
  return builder->CreateBitCast(
      builder->CreateTrunc(
          builder->CreateBitCast(x, builder->getIntNTy(32 * num_segments)),
          builder->getIntNTy(bit_width)),
      value->getType());
}

StatusOr<CudnnConvKind> GetCudnnConvKind(
    const HloCustomCallInstruction* instr) {
  absl::string_view target = instr->custom_call_target();
  if (target == kCudnnConvForwardCallTarget) {
    return CudnnConvKind::kForward;
  }
  if (target == kCudnnConvBackwardInputCallTarget) {
    return CudnnConvKind::kBackwardInput;
  }
  if (target == kCudnnConvBackwardFilterCallTarget) {
    return CudnnConvKind::kBackwardFilter;
  }
  if (target == kCudnnConvBiasActivationForwardCallTarget) {
    return CudnnConvKind::kForwardActivation;
  }
  return InternalError("Unexpected call target: %s", target);
}

string CudnnConvKindToString(CudnnConvKind kind) {
  switch (kind) {
    case CudnnConvKind::kForward:
      return "forward";
    case CudnnConvKind::kBackwardFilter:
      return "backward_filter";
    case CudnnConvKind::kBackwardInput:
      return "backward_input";
    case CudnnConvKind::kForwardActivation:
      return "forward with activation";
  }
}

llvm::Value* IsBlock0Thread0(
    llvm_ir::LLVMTargetIRBuilder& llvm_target_ir_builder) {
  llvm::IRBuilder<>* b = llvm_target_ir_builder.builder();
  return b->CreateAnd(
      b->CreateICmpEQ(b->getInt32(0), llvm_ir::EmitCallToTargetIntrinsic(
                                          llvm_ir::kTHREAD_ID_X, {}, {},
                                          llvm_target_ir_builder)),
      b->CreateICmpEQ(b->getInt32(0), llvm_ir::EmitCallToTargetIntrinsic(
                                          llvm_ir::kBLOCK_ID_X, {}, {},
                                          llvm_target_ir_builder)));
}

}  // namespace gpu
}  // namespace xla
