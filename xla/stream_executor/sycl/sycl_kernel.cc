/* Copyright (c) 2023 Intel Corporation
Copyright 2019 The TensorFlow Authors. All Rights Reserved.
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

#include "xla/stream_executor/sycl/sycl_kernel.h"

namespace stream_executor {
namespace gpu {

absl::StatusOr<int32_t> GpuKernel::GetMaxOccupiedBlocksPerCore(
    ThreadDim threads, size_t dynamic_shared_memory_bytes) const {
  int32_t threads_per_block = threads.x * threads.y * threads.z;
  VLOG(3) << "Get kernel block occupancy: " << name_
          << "; threads_per_block: " << threads_per_block
          << "; dynamic_shared_memory_bytes: " << dynamic_shared_memory_bytes;

  return GpuDriver::GetMaxOccupiedBlocksPerCore(gpu_context_, gpu_function_,
                                                threads_per_block,
                                                dynamic_shared_memory_bytes);
}

}  // namespace gpu
}  // namespace stream_executor