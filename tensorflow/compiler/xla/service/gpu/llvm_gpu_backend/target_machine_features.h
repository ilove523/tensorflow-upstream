/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_GPU_TARGET_MACHINE_FEATURES_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_GPU_TARGET_MACHINE_FEATURES_H_

#include <string>


namespace xla {
namespace gpu {

// Abstract interface for classes providing information about the gpu target we're
// compiling for.
class TargetMachineFeatures {
 public:
  // Returns the minimum alignment for a buffer of size size_bytes.

  virtual std::string simt_intrinsic(const std::string &name) = 0;

  virtual ~TargetMachineFeatures() = default;

};

class AMDGPUMachineFeatures : public TargetMachineFeatures {
 public:
  // Returns the minimum alignment for a buffer of size size_bytes.
  std::string simt_intrinsic(const std::string &name);
  AMDGPUMachineFeatures(){};
  ~AMDGPUMachineFeatures();
};


}  // namespace gpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_GPU_TARGET_MACHINE_FEATURES_H_
