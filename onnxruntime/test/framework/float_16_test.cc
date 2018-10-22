// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/session/inference_session.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <thread>

#include "core/common/logging/logging.h"
#include "core/framework/execution_provider.h"
#include "core/framework/op_kernel.h"
#include "core/framework/op_kernel_abi_wrapper.h"
#include "core/framework/session_state.h"
#include "core/graph/graph.h"
#include "core/graph/model.h"
#include "core/graph/op.h"
#include "core/providers/cpu/cpu_execution_provider.h"
#include "core/providers/cpu/math/element_wise_ops.h"
#include "core/framework/tensorprotoutils.h"
#include "core/inc/op_kernel_author_helper.h"

#include "test/capturing_sink.h"
#include "test/test_environment.h"
#include "test_utils.h"
#include "gtest/gtest.h"
#include "core/graph/schema_registry.h"
#include "core/framework/customregistry.h"
#include "core/util/math.h"

using namespace ONNX_NAMESPACE;
namespace onnxruntime {
namespace test {

class MulFP16Kernel {
 public:
  MulFP16Kernel(const MLOpKernelInfo& /*info*/) {}

  MLStatus Compute(const MLOpKernelContext& context) const {
    const auto X = context.GetInputTensor(0);
    const auto W = context.GetInputTensor(1);

    auto X_Data = X.GetData<MLFloat16>();
    auto W_Data = W.GetData<MLFloat16>();

    auto& shape = X.GetDimensions();
    auto Y = context.GetOutputTensor(0, shape);
    auto Y_Data = Y.GetData<MLFloat16>();

    size_t size = 1;
    for (size_t i = 0; i < shape.size(); i++) {
      size *= shape[i];
    }

    for (size_t i = 0; i < size; i++) {
      Y_Data[i].val = math::floatToHalf(
          math::halfToFloat(X_Data[i].val) *
          math::halfToFloat(W_Data[i].val));
    }

    return MLStatus::OK;
  }
};

//For test purpose, we register this MulFP16Kernel kernel to Mul op.
//Once the custom schema is ready, should update this.
KernelDefBuilder MulFP16KernelDef() {
  KernelDefBuilder def;
  def.SetName("Mul16")
      .SetDomain(onnxruntime::kOnnxDomain)
      .SinceVersion(6)
      .Provider(onnxruntime::kCpuExecutionProvider)
      .TypeConstraint("T", DataTypeImpl::GetTensorType<MLFloat16>());
  return def;
}

MLStatus CreateABIMulFP16Kernel(const IMLOpKernelInfo& kernel_info, IMLOpKernel** op_kernel) {
  return MLOpKernel<MulFP16Kernel>::CreateInstance(kernel_info, op_kernel);
}

// Creates a kernel implementing the built-in OpKernel type.  This wraps
// the ABI kernel as an implementation detail.
OpKernel* CreateMulFP16Kernel(const OpKernelInfo& kernel_info) {
  return new ::onnxruntime::AbiOpKernel(CreateABIMulFP16Kernel, kernel_info, false, false, nullptr, nullptr);
}

ONNX_NAMESPACE::OpSchema GetMulFP16Schema() {
  ONNX_NAMESPACE::OpSchema schema("Mul16", "unknown", 0);
  schema.Input(0,
               "A",
               "First operand, should share the type with the second operand.",
               "T");
  schema.Input(
      1,
      "B",
      "Second operand. With broadcasting can be of smaller size than A. ",
      "T");
  schema.Output(0, "C", "Result, has same dimensions and type as A", "T");
  schema.TypeConstraint(
      "T",
      OpSchema::all_numeric_types(),
      "Constrain input and output types to high-precision numeric tensors.");
  schema.SinceVersion(6);
  return schema;
}

static const std::string MUL_MODEL_URI = "testdata/mul_16.pb";

void RunSession(InferenceSession& session_object,
                RunOptions& run_options,
                std::vector<int64_t>& dims_x,
                std::vector<MLFloat16>& values_x,
                std::vector<int64_t>& dims_y,
                std::vector<MLFloat16>& values_y) {
  // prepare inputs
  MLValue ml_value;
  CreateMLValue<MLFloat16>(TestCPUExecutionProvider()->GetAllocator(0, ONNXRuntimeMemTypeDefault), dims_x, values_x, &ml_value);
  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value));

  // prepare outputs
  std::vector<std::string> output_names;
  output_names.push_back("Y");
  std::vector<MLValue> fetches;

  // Now run
  common::Status st = session_object.Run(run_options, feeds, output_names, &fetches);
  std::cout << "Run returned status: " << st.ErrorMessage() << std::endl;
  EXPECT_TRUE(st.IsOK());
  ASSERT_EQ(1, fetches.size());
  auto& rtensor = fetches.front().Get<Tensor>();
  TensorShape expected_shape(dims_y);
  EXPECT_EQ(expected_shape, rtensor.Shape());
  const std::vector<MLFloat16> found(rtensor.template Data<MLFloat16>(), rtensor.template Data<MLFloat16>() + expected_shape.Size());
  ASSERT_EQ(found.size(), values_y.size());
  for (size_t i = 0; i < found.size(); i++)
    ASSERT_EQ(found[i].val, values_y[i].val);
}

TEST(Float16_Tests, Mul_16_Test) {
  SessionOptions so;

  so.session_logid = "InferenceSessionTests.NoTimeout";

  std::shared_ptr<CustomRegistry> registry = std::make_shared<CustomRegistry>();
  InferenceSession session_object{so, &DefaultLoggingManager()};
  EXPECT_TRUE(session_object.RegisterCustomRegistry(registry).IsOK());
  auto mulfp16_schema = GetMulFP16Schema();
  std::vector<OpSchema> schemas = {mulfp16_schema};

  EXPECT_TRUE(registry->RegisterOpSet(schemas, onnxruntime::kOnnxDomain, 5, 7).IsOK());

  auto def = MulFP16KernelDef();
  //Register a foo kernel which is doing Add, but bind to Mul.
  EXPECT_TRUE(registry->RegisterCustomKernel(def, CreateMulFP16Kernel).IsOK());

  EXPECT_TRUE(session_object.Load(MUL_MODEL_URI).IsOK());
  EXPECT_TRUE(session_object.Initialize().IsOK());

  RunOptions run_options;
  run_options.run_tag = "one session/one tag";

  // prepare inputs
  std::vector<int64_t> dims_x = {3, 2};
  std::vector<float> values_x_32 = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  std::vector<MLFloat16> values_x;
  for (float i : values_x_32) {
    values_x.push_back(MLFloat16(math::floatToHalf(i)));
  }

  // prepare expected inputs and outputs
  std::vector<int64_t> expected_dims_y = {3, 2};
  // now the expected value should be Add's result.
  std::vector<float> expected_values_y_32 = {1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 36.0f};
  std::vector<MLFloat16> expected_values_y;
  for (float i : expected_values_y_32) {
    expected_values_y.push_back(MLFloat16(math::floatToHalf(i)));
  }

  // Now run
  RunSession(session_object, run_options, dims_x, values_x, expected_dims_y, expected_values_y);
}
}  // namespace test
}  // namespace onnxruntime
