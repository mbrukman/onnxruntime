// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/common/common.h"
#include "core/framework/op_kernel.h"
#include "core/util/math_cpuonly.h"

namespace onnxruntime {
namespace contrib {

template <typename T>
class DequantizeLinear final : public OpKernel {
 public:
  DequantizeLinear(const OpKernelInfo& info) : OpKernel(info) {
    has_axis_ = info.GetAttr<int64_t>("axis", &axis_).IsOK();
  }

  Status Compute(OpKernelContext* context) const override;

 private:
  int64_t axis_ = 0;
  bool has_axis_;
};

template <typename T>
class QuantizeLinear final : public OpKernel {
 public:
  QuantizeLinear(const OpKernelInfo& info) : OpKernel(info) {
    has_axis_ = info.GetAttr<int64_t>("axis", &axis_).IsOK();
  }

  Status Compute(OpKernelContext* context) const override;

 private:
  int64_t axis_ = 0;
  bool has_axis_;
};
}  // namespace contrib
}  // namespace onnxruntime
