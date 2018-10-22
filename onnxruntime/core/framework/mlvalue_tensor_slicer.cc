// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/mlvalue_tensor_slicer.h"
#include <cassert>

namespace onnxruntime {

template <typename T>
MLValueTensorSlicer<T> MLValueTensorSlicer<T>::Create(T& mlvalue, int64_t slice_dimension, int64_t dim0_offset) {
  static_assert(std::is_same<std::remove_const_t<T>, MLValue>::value,
                "MLValueTensorSlicer can only be used with 'MLValue' or 'const MLValue'");

  ONNXRUNTIME_ENFORCE(mlvalue.IsTensor(), "Can't slice a non-tensor MLValue. Type was ", mlvalue.Type());
  ONNXRUNTIME_ENFORCE(mlvalue.IsAllocated(), "MLValue has not been allocated so can't be sliced.");

  auto& tensor_shape{mlvalue.template Get<Tensor>().Shape()};
  ONNXRUNTIME_ENFORCE(gsl::narrow_cast<int64_t>(tensor_shape.NumDimensions()) > slice_dimension,
              "Insufficient dimensions to slice on ", slice_dimension, ". Shape:", tensor_shape);

  auto dim0_size = tensor_shape[0];
  ONNXRUNTIME_ENFORCE(dim0_offset < dim0_size, "Invalid dim0_offset of ", dim0_offset, ". Dimension 0 is ", dim0_size);

  return MLValueTensorSlicer{mlvalue, slice_dimension, dim0_offset};
};

template <typename T>
MLValueTensorSlicer<T>::Iterator::Iterator(T& mlvalue, size_t slice_dimension, size_t dim0_offset,
                                           int64_t position, Direction direction)
    : mlvalue_{&mlvalue},
      position_{position},
      increment_by_{direction == Direction::kForward ? 1 : -1},
      position_materialized_{-1} {
  const auto& tensor = mlvalue.template Get<Tensor>();
  tensor_data_type_ = tensor.DataType();
  tensor_location_ = &tensor.Location();

  const TensorShape& shape = tensor.Shape();
  sequence_length_ = shape[slice_dimension];
  per_iteration_shape_ = shape.Slice(slice_dimension + 1);
  const int64_t per_iteration_shape_size = per_iteration_shape_.Size();
  assert(per_iteration_shape_size >= 0);
  if (!IAllocator::CalcMemSizeForArray(static_cast<size_t>(per_iteration_shape_size), tensor.DataType()->Size(), &per_iteration_offset_))
    throw std::runtime_error("size overflow");
  const int64_t slice_dimension_size = shape.Slice(slice_dimension).Size();
  assert(slice_dimension_size >= 0);

  size_t total_len;
  if (!IAllocator::CalcMemSizeForArray(static_cast<size_t>(slice_dimension_size), tensor.DataType()->Size(), &total_len))
    throw std::runtime_error("size overflow");
  if (!IAllocator::CalcMemSizeForArray(dim0_offset, total_len, &total_len))
    throw std::runtime_error("size overflow");
  // move tensor_data_raw_ to the start of the section to slice
  tensor_data_raw_ = static_cast<const char*>(tensor.DataRaw()) + total_len;

  // constrain position_ to valid bounds of 0 to sequence_length_ if forward, or -1 to sequence_length_ - 1 if reverse
  if (direction == Direction::kForward) {
    if (position_ > sequence_length_)
      position_ = sequence_length_;  // end()
  } else {
    if (position_ >= sequence_length_)
      position_ = sequence_length_ - 1;  // begin() at first valid position_

    if (position_ < -1)
      position_ = -1;  // end()
  }
}

template <typename T>
void MLValueTensorSlicer<T>::Iterator::MaterializeMLValue() const {
  position_materialized_ = position_;
  const void* tensor_slice_data_raw = static_cast<const char*>(tensor_data_raw_) + (position_ * per_iteration_offset_);

  // create a sub Tensor for the current position, and put it in an MLValue.
  //
  // We need the non-const data pointer from the Tensor in order to create the sub-Tensors as we iterate,
  // so a const_cast is required.
  // However we will only return a non-const MLValue from operator* if MLValueTensorSlicer was created with
  // a non-const MLValue, so externally we maintain constness as expected.
  //
  // TODO: Ideally we could avoid the overhead of creating a new Tensor but that would require
  // a lot more complexity (re-consider how ExecutionFrame and OpKernelContext work and whether
  // they need to be MLValue based, or whether they could be Tensor based).
  // Potential future performance enhancement.
  auto sub_tensor = std::make_unique<Tensor>(tensor_data_type_,
                                             per_iteration_shape_,
                                             const_cast<void*>(tensor_slice_data_raw),
                                             *tensor_location_);

  current_ = MLValue{sub_tensor.release(),
                     DataTypeImpl::GetType<Tensor>(),
                     DataTypeImpl::GetType<Tensor>()->GetDeleteFunc()};
}

template class MLValueTensorSlicer<MLValue>;
template class MLValueTensorSlicer<const MLValue>;

}  // namespace onnxruntime
