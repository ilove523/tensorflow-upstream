/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/framework/fake_input.h"
#include "tensorflow/core/framework/function_testlib.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/partial_tensor_shape.h"
#include "tensorflow/core/framework/variant.h"
#include "tensorflow/core/framework/variant_tensor_data.h"
#include "tensorflow/core/kernels/data/dataset_test_base.h"
#include "tensorflow/core/kernels/data/dataset_utils.h"
#include "tensorflow/core/kernels/data/iterator_ops.h"
#include "tensorflow/core/kernels/ops_testutil.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/util/ptr_util.h"

namespace tensorflow {
namespace data {
namespace {

constexpr char kNodeName[] = "tensor_slice_dataset";
constexpr char kOpName[] = "TensorSliceDataset";

class TensorSliceDatasetOpTest : public DatasetOpsTestBase {
 protected:
  // Creates a new TensorSliceDataset op kernel.
  Status CreateTensorSliceDatasetKernel(
      DataTypeVector dtypes, std::vector<PartialTensorShape> shapes,
      std::unique_ptr<OpKernel>* tensor_dataset_kernel) {
    std::vector<string> components;
    components.reserve(dtypes.size());
    for (int i = 0; i < dtypes.size(); i++) {
      components.emplace_back(strings::StrCat("component_", i));
    }

    node_def_ = test::function::NDef(
        kNodeName, kOpName, components,
        {{"Toutput_types", dtypes}, {"output_shapes", shapes}});
    TF_RETURN_IF_ERROR(CreateOpKernel(node_def_, tensor_dataset_kernel));
    return Status::OK();
  }

  // Creates a new TensorSliceDataset op kernel context.
  Status CreateTensorSliceDatasetContext(
      OpKernel* const tensor_dataset_kernel,
      gtl::InlinedVector<TensorValue, 4>* inputs,
      std::unique_ptr<OpKernelContext>* context) {
    TF_RETURN_IF_ERROR(CheckOpKernelInput(*tensor_dataset_kernel, *inputs));
    TF_RETURN_IF_ERROR(
        CreateOpKernelContext(tensor_dataset_kernel, inputs, context));
    return Status::OK();
  }

 private:
  NodeDef node_def_;
};

struct TestParam {
  std::vector<Tensor> components;
  std::vector<Tensor> expected_outputs;
  std::vector<int> breakpoints;
} TestCases[] = {
    // A single tuple of tensors.
    {{{DatasetOpsTestBase::CreateTensor<int64>(TensorShape({2}), {1, 2}),
       DatasetOpsTestBase::CreateTensor<int64>(TensorShape({2, 2}),
                                               {1, 2, 3, 4}),
       DatasetOpsTestBase::CreateTensor<double>(TensorShape({2, 1}),
                                                {37.0, 38.0}),
       DatasetOpsTestBase::CreateTensor<string>(TensorShape({2, 1}),
                                                {"a", "b"})}},  // components
     {{DatasetOpsTestBase::CreateTensor<int64>(TensorShape({}), {1}),
       DatasetOpsTestBase::CreateTensor<int64>(TensorShape({2}), {1, 2}),
       DatasetOpsTestBase::CreateTensor<double>(TensorShape({1}), {37.0}),
       DatasetOpsTestBase::CreateTensor<string>(TensorShape({1}), {"a"}),
       DatasetOpsTestBase::CreateTensor<int64>(TensorShape({}), {2}),
       DatasetOpsTestBase::CreateTensor<int64>(TensorShape({2}), {3, 4}),
       DatasetOpsTestBase::CreateTensor<double>(TensorShape({1}), {38.0}),
       DatasetOpsTestBase::CreateTensor<string>(TensorShape({1}),
                                                {"b"})}},  // expected_outputs
     {{0, 1, 3}}},                                         //  breakpoints
    // Nested tensors
    {{{DatasetOpsTestBase::CreateTensor<Variant>(
           TensorShape({2, 1}),
           {DatasetOpsTestBase::CreateTensor<double>(TensorShape({2, 2}),
                                                     {1.0, 2.0, 3.0, 4.0}),
            DatasetOpsTestBase::CreateTensor<double>(TensorShape({2, 2}),
                                                     {5.0, 6.0, 7.0, 8.0})}),
       DatasetOpsTestBase::CreateTensor<Variant>(
           TensorShape({2, 1}), {DatasetOpsTestBase::CreateTensor<string>(
                                     TensorShape({1, 2}), {"a", "b"}),
                                 DatasetOpsTestBase::CreateTensor<string>(
                                     TensorShape({1, 2}), {"c", "d"})}),
       DatasetOpsTestBase::CreateTensor<int64>(
           TensorShape({2, 3}), {1, 2, 3, 4, 5, 6})}},  // components
     {{DatasetOpsTestBase::CreateTensor<Variant>(
           TensorShape({1}), {DatasetOpsTestBase::CreateTensor<double>(
                                 TensorShape({2, 2}), {1.0, 2.0, 3.0, 4.0})}),
       DatasetOpsTestBase::CreateTensor<Variant>(
           TensorShape({1}), {DatasetOpsTestBase::CreateTensor<string>(
                                 TensorShape({1, 2}), {"a", "b"})}),
       DatasetOpsTestBase::CreateTensor<int64>(TensorShape({3}), {1, 2, 3}),
       DatasetOpsTestBase::CreateTensor<Variant>(
           TensorShape({1}), {DatasetOpsTestBase::CreateTensor<double>(
                                 TensorShape({2, 2}), {5.0, 6.0, 7.0, 8.0})}),
       DatasetOpsTestBase::CreateTensor<Variant>(
           TensorShape({1}), {DatasetOpsTestBase::CreateTensor<string>(
                                 TensorShape({1, 2}), {"c", "d"})}),
       DatasetOpsTestBase::CreateTensor<int64>(
           TensorShape({3}), {4, 5, 6})}},  // expected_outputs
     {{0, 1, 2}}}                           // breakpoints
};

struct DatasetGetNextTest : TensorSliceDatasetOpTest,
                            ::testing::WithParamInterface<TestParam> {};

TEST_P(DatasetGetNextTest, GetNext) {
  int thread_num = 2, cpu_num = 2;
  std::vector<Tensor> components = GetParam().components;
  std::vector<Tensor> expected_outputs = GetParam().expected_outputs;
  size_t num_tensors_per_slice = components.size();

  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  DataTypeVector dtypes;
  std::vector<PartialTensorShape> shapes;
  gtl::InlinedVector<TensorValue, 4> inputs;
  for (auto& component : components) {
    inputs.push_back(&component);
    dtypes.push_back(component.dtype());
  }
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    shapes.emplace_back(expected_outputs[i].shape());
  }

  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  std::unique_ptr<IteratorContext> iterator_context;
  TF_ASSERT_OK(CreateIteratorContext(tensor_slice_dataset_context.get(),
                                     &iterator_context));
  std::unique_ptr<IteratorBase> iterator;
  TF_ASSERT_OK(tensor_slice_dataset->MakeIterator(iterator_context.get(),
                                                  "Iterator", &iterator));
  bool end_of_sequence = false;
  std::vector<Tensor> out_tensors;
  int cur_slice = 0;

  while (!end_of_sequence) {
    TF_EXPECT_OK(iterator->GetNext(iterator_context.get(), &out_tensors,
                                   &end_of_sequence));
    for (int i = 0; i < out_tensors.size(); ++i) {
      EXPECT_LT(i + num_tensors_per_slice * cur_slice, expected_outputs.size());
      if (out_tensors[i].dtype() == DT_VARIANT) {
        // Currently `ExpectEqual()` does not support the variant tensor
        // yet, so we manually cast the variant to numeric/string tensor.
        const Tensor* output = out_tensors[i].scalar<Variant>()().get<Tensor>();
        const Tensor* expected_output =
            expected_outputs[i + num_tensors_per_slice * cur_slice]
                .scalar<Variant>()()
                .get<Tensor>();
        TF_EXPECT_OK(ExpectEqual(*output, *expected_output));
      } else {
        TF_EXPECT_OK(ExpectEqual(
            out_tensors[i],
            expected_outputs[i + num_tensors_per_slice * cur_slice]));
      }
    }
    out_tensors.clear();
    cur_slice++;
  }
}

INSTANTIATE_TEST_CASE_P(TensorDatasetSliceOpTest, DatasetGetNextTest,
                        ::testing::ValuesIn(TestCases));

TEST_F(TensorSliceDatasetOpTest, DatasetName) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  Tensor t1 = CreateTensor<int64>(TensorShape({2, 2}), {1, 2, 3, 4});
  Tensor t2 = CreateTensor<int64>(TensorShape({2, 2}), {5, 6, 7, 8});
  gtl::InlinedVector<TensorValue, 4> inputs = {&t1, &t2};
  DataTypeVector dtypes({DT_INT64, DT_INT64});
  std::vector<PartialTensorShape> shapes = {PartialTensorShape({2}),
                                            PartialTensorShape({2})};
  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  EXPECT_EQ(tensor_slice_dataset->name(), kOpName);
}

struct DatasetOutputDtypesTest : TensorSliceDatasetOpTest,
                                 ::testing::WithParamInterface<TestParam> {};

TEST_P(DatasetOutputDtypesTest, DatasetOutputDtypes) {
  int thread_num = 2, cpu_num = 2;
  std::vector<Tensor> components = GetParam().components;
  std::vector<Tensor> expected_outputs = GetParam().expected_outputs;
  size_t num_tensors_per_slice = components.size();

  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  DataTypeVector dtypes;
  std::vector<PartialTensorShape> shapes;
  gtl::InlinedVector<TensorValue, 4> inputs;
  for (auto& component : components) {
    inputs.emplace_back(&component);
    dtypes.emplace_back(component.dtype());
  }
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    shapes.emplace_back(expected_outputs[i].shape());
  }

  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  const DataTypeVector produced_output_dtypes =
      tensor_slice_dataset->output_dtypes();
  EXPECT_EQ(produced_output_dtypes.size(), num_tensors_per_slice);
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    EXPECT_EQ(produced_output_dtypes[i], expected_outputs[i].dtype());
  }
}

INSTANTIATE_TEST_CASE_P(TensorDatasetSliceOpTest, DatasetOutputDtypesTest,
                        ::testing::ValuesIn(TestCases));

struct DatasetOutputShapesTest : TensorSliceDatasetOpTest,
                                 ::testing::WithParamInterface<TestParam> {};

TEST_P(DatasetOutputShapesTest, DatasetOutputShapes) {
  int thread_num = 2, cpu_num = 2;
  std::vector<Tensor> components = GetParam().components;
  std::vector<Tensor> expected_outputs = GetParam().expected_outputs;
  size_t num_tensors_per_slice = components.size();

  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  DataTypeVector dtypes;
  std::vector<PartialTensorShape> shapes;
  gtl::InlinedVector<TensorValue, 4> inputs;
  for (auto& component : components) {
    inputs.emplace_back(&component);
    dtypes.emplace_back(component.dtype());
  }
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    shapes.emplace_back(expected_outputs[i].shape());
  }
  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  const std::vector<PartialTensorShape> produced_output_shapes =
      tensor_slice_dataset->output_shapes();
  std::vector<PartialTensorShape> expected_output_shapes;
  EXPECT_EQ(produced_output_shapes.size(), num_tensors_per_slice);
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    EXPECT_TRUE(
        produced_output_shapes[i].IsIdenticalTo(expected_outputs[i].shape()));
  }
}

INSTANTIATE_TEST_CASE_P(TensorDatasetSliceOpTest, DatasetOutputShapesTest,
                        ::testing::ValuesIn(TestCases));

struct DatasetCardinalityTest : TensorSliceDatasetOpTest,
                                ::testing::WithParamInterface<TestParam> {};

TEST_P(DatasetCardinalityTest, Cardinality) {
  int thread_num = 2, cpu_num = 2;
  std::vector<Tensor> components = GetParam().components;
  std::vector<Tensor> expected_outputs = GetParam().expected_outputs;
  size_t num_tensors_per_slice = components.size();

  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  DataTypeVector dtypes;
  std::vector<PartialTensorShape> shapes;
  gtl::InlinedVector<TensorValue, 4> inputs;
  for (auto& component : components) {
    inputs.emplace_back(&component);
    dtypes.emplace_back(component.dtype());
  }
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    shapes.emplace_back(expected_outputs[i].shape());
  }
  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  EXPECT_EQ(tensor_slice_dataset->Cardinality(), inputs[0].tensor->dim_size(0));
}

INSTANTIATE_TEST_CASE_P(TensorDatasetSliceOpTest, DatasetCardinalityTest,
                        ::testing::ValuesIn(TestCases));

TEST_F(TensorSliceDatasetOpTest, DatasetSave) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  Tensor t1 = CreateTensor<int64>(TensorShape({2, 2}), {1, 2, 3, 4});
  Tensor t2 = CreateTensor<int64>(TensorShape({2, 2}), {5, 6, 7, 8});
  gtl::InlinedVector<TensorValue, 4> inputs = {&t1, &t2};
  DataTypeVector dtypes({DT_INT64, DT_INT64});
  std::vector<PartialTensorShape> shapes = {PartialTensorShape({2}),
                                            PartialTensorShape({2})};
  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  std::unique_ptr<SerializationContext> serialization_context;
  TF_ASSERT_OK(CreateSerializationContext(&serialization_context));
  VariantTensorData data;
  VariantTensorDataWriter writer(&data);
  TF_ASSERT_OK(
      tensor_slice_dataset->Save(serialization_context.get(), &writer));
  TF_ASSERT_OK(writer.Flush());
}

struct IteratorOutputDtypesTest : TensorSliceDatasetOpTest,
                                  ::testing::WithParamInterface<TestParam> {};

TEST_P(IteratorOutputDtypesTest, IteratorOutputDtypes) {
  int thread_num = 2, cpu_num = 2;
  std::vector<Tensor> components = GetParam().components;
  std::vector<Tensor> expected_outputs = GetParam().expected_outputs;
  size_t num_tensors_per_slice = components.size();

  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  DataTypeVector dtypes;
  std::vector<PartialTensorShape> shapes;
  gtl::InlinedVector<TensorValue, 4> inputs;
  for (auto& component : components) {
    inputs.emplace_back(&component);
    dtypes.emplace_back(component.dtype());
  }
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    shapes.emplace_back(expected_outputs[i].shape());
  }

  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  std::unique_ptr<IteratorContext> iterator_context;
  TF_ASSERT_OK(CreateIteratorContext(tensor_slice_dataset_context.get(),
                                     &iterator_context));
  std::unique_ptr<IteratorBase> iterator;
  TF_ASSERT_OK(tensor_slice_dataset->MakeIterator(iterator_context.get(),
                                                  "Iterator", &iterator));
  const DataTypeVector produced_output_dtypes = iterator->output_dtypes();

  EXPECT_EQ(produced_output_dtypes.size(), num_tensors_per_slice);
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    EXPECT_EQ(produced_output_dtypes[i], expected_outputs[i].dtype());
  }
}

INSTANTIATE_TEST_CASE_P(TensorDatasetSliceOpTest, IteratorOutputDtypesTest,
                        ::testing::ValuesIn(TestCases));

struct IteratorOutputShapesTest : TensorSliceDatasetOpTest,
                                  ::testing::WithParamInterface<TestParam> {};

TEST_P(IteratorOutputShapesTest, IteratorOutputShapes) {
  int thread_num = 2, cpu_num = 2;
  std::vector<Tensor> components = GetParam().components;
  std::vector<Tensor> expected_outputs = GetParam().expected_outputs;
  size_t num_tensors_per_slice = components.size();

  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  DataTypeVector dtypes;
  std::vector<PartialTensorShape> shapes;
  gtl::InlinedVector<TensorValue, 4> inputs;
  for (auto& component : components) {
    inputs.emplace_back(&component);
    dtypes.emplace_back(component.dtype());
  }
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    shapes.emplace_back(expected_outputs[i].shape());
  }

  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  std::unique_ptr<IteratorContext> iterator_context;
  TF_ASSERT_OK(CreateIteratorContext(tensor_slice_dataset_context.get(),
                                     &iterator_context));
  std::unique_ptr<IteratorBase> iterator;
  TF_ASSERT_OK(tensor_slice_dataset->MakeIterator(iterator_context.get(),
                                                  "Iterator", &iterator));
  const std::vector<PartialTensorShape> produced_output_shapes =
      iterator->output_shapes();
  EXPECT_EQ(produced_output_shapes.size(), num_tensors_per_slice);
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    EXPECT_TRUE(
        produced_output_shapes[i].IsIdenticalTo(expected_outputs[i].shape()));
  }
}

INSTANTIATE_TEST_CASE_P(TensorDatasetSliceOpTest, IteratorOutputShapesTest,
                        ::testing::ValuesIn(TestCases));

TEST_F(TensorSliceDatasetOpTest, IteratorOutputPrefix) {
  int thread_num = 2, cpu_num = 2;
  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  Tensor t1 = CreateTensor<int64>(TensorShape({2, 2}), {1, 2, 3, 4});
  Tensor t2 = CreateTensor<int64>(TensorShape({2, 2}), {5, 6, 7, 8});
  gtl::InlinedVector<TensorValue, 4> inputs = {&t1, &t2};
  DataTypeVector dtypes({DT_INT64, DT_INT64});
  std::vector<PartialTensorShape> shapes = {PartialTensorShape({2}),
                                            PartialTensorShape({2})};
  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  std::unique_ptr<IteratorContext> iterator_context;
  TF_ASSERT_OK(CreateIteratorContext(tensor_slice_dataset_context.get(),
                                     &iterator_context));
  std::unique_ptr<IteratorBase> iterator;
  TF_ASSERT_OK(tensor_slice_dataset->MakeIterator(iterator_context.get(),
                                                  "Iterator", &iterator));
  EXPECT_EQ(iterator->prefix(), "Iterator::TensorSlice");
}

struct IteratorRoundtripTest : TensorSliceDatasetOpTest,
                               ::testing::WithParamInterface<TestParam> {};

TEST_P(IteratorRoundtripTest, Roundtrip) {
  int thread_num = 2, cpu_num = 2;
  std::vector<Tensor> components = GetParam().components;
  std::vector<Tensor> expected_outputs = GetParam().expected_outputs;
  std::vector<int> breakpoints = GetParam().breakpoints;
  size_t num_tensors_per_slice = components.size();

  TF_ASSERT_OK(InitThreadPool(thread_num));
  TF_ASSERT_OK(InitFunctionLibraryRuntime({}, cpu_num));

  DataTypeVector dtypes;
  std::vector<PartialTensorShape> shapes;
  gtl::InlinedVector<TensorValue, 4> inputs;
  for (auto& component : components) {
    inputs.emplace_back(&component);
    dtypes.emplace_back(component.dtype());
  }
  for (int i = 0; i < num_tensors_per_slice; ++i) {
    shapes.emplace_back(expected_outputs[i].shape());
  }

  std::unique_ptr<OpKernel> tensor_slice_dataset_kernel;
  TF_ASSERT_OK(CreateTensorSliceDatasetKernel(dtypes, shapes,
                                              &tensor_slice_dataset_kernel));
  std::unique_ptr<OpKernelContext> tensor_slice_dataset_context;
  TF_ASSERT_OK(
      CreateTensorSliceDatasetContext(tensor_slice_dataset_kernel.get(),
                                      &inputs, &tensor_slice_dataset_context));
  DatasetBase* tensor_slice_dataset;
  TF_ASSERT_OK(CreateDataset(tensor_slice_dataset_kernel.get(),
                             tensor_slice_dataset_context.get(),
                             &tensor_slice_dataset));
  core::ScopedUnref scored_unref(tensor_slice_dataset);

  std::unique_ptr<IteratorContext> iterator_context;
  TF_ASSERT_OK(CreateIteratorContext(tensor_slice_dataset_context.get(),
                                     &iterator_context));
  std::unique_ptr<IteratorBase> iterator;
  TF_ASSERT_OK(tensor_slice_dataset->MakeIterator(iterator_context.get(),
                                                  "Iterator", &iterator));
  std::unique_ptr<SerializationContext> serialization_context;
  TF_ASSERT_OK(CreateSerializationContext(&serialization_context));

  int cur_iteration = 0;
  bool end_of_sequence = false;
  int64 num_slices = inputs[0].tensor->dim_size(0);
  std::vector<Tensor> out_tensors;

  for (int breakpoint : breakpoints) {
    while (cur_iteration < breakpoint) {
      TF_EXPECT_OK(iterator->GetNext(iterator_context.get(), &out_tensors,
                                     &end_of_sequence));
      cur_iteration++;
    }

    if (breakpoint == 0) {
      EXPECT_FALSE(end_of_sequence);
    } else if (breakpoint <= num_slices) {
      for (int i = 0; i < out_tensors.size(); ++i) {
        if (out_tensors[i].dtype() == DT_VARIANT) {
          const Tensor* output =
              out_tensors[i].scalar<Variant>()().get<Tensor>();
          const Tensor* expected_output =
              expected_outputs[i + num_tensors_per_slice * (cur_iteration - 1)]
                  .scalar<Variant>()()
                  .get<Tensor>();
          TF_EXPECT_OK(ExpectEqual(*output, *expected_output));
        } else {
          TF_EXPECT_OK(ExpectEqual(
              out_tensors[i], expected_outputs[i + num_tensors_per_slice *
                                                       (cur_iteration - 1)]));
        }
      }
    } else {
      EXPECT_TRUE(end_of_sequence);
    }

    VariantTensorData data;
    VariantTensorDataWriter writer(&data);
    TF_ASSERT_OK(iterator->Save(serialization_context.get(), &writer));
    TF_ASSERT_OK(writer.Flush());
    VariantTensorDataReader reader(&data);
    TF_ASSERT_OK(iterator->Restore(iterator_context.get(), &reader));
  }
}

INSTANTIATE_TEST_CASE_P(TensorDatasetSliceOpTest, IteratorRoundtripTest,
                        ::testing::ValuesIn(TestCases));

}  // namespace
}  // namespace data
}  // namespace tensorflow
