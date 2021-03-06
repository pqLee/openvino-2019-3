//
// Copyright (c) 2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once

#include "convolution_kernel_base.h"
#include <vector>

namespace kernel_selector {

class ConvolutionKernel_bfzyx_f16 : public ConvolutionKernelBase {
public:
    using Parent = ConvolutionKernelBase;

    explicit ConvolutionKernel_bfzyx_f16(Datatype use_data_type) :
        ConvolutionKernelBase(use_data_type == Datatype::F32 ? "gen9_common_conv_fwd_data_f32" : "gen9_common_conv_fwd_data_f16"),
        use_data_type(use_data_type) {}

    virtual ~ConvolutionKernel_bfzyx_f16() {}

    KernelsData GetKernelsData(const Params& params, const optional_params& options) const override;
    ParamsKey GetSupportedKey() const override;

protected:
    std::vector<WeightsLayout> GetSupportedWeightLayouts(const convolution_params&) const override {
        return {
            WeightsLayout::o_i_zyx_i16_o16,
        };
    }
    bool Validate(const Params& p, const optional_params& o) const override;
    DispatchData SetDefault(const convolution_params& arg, int autoTuneIndex = -1) const override;
    JitConstants GetJitConstants(const convolution_params& params, const DispatchData& kd) const override;

    // This class is base one for FP16 and FP32 classes
    Datatype use_data_type;
};
}  // namespace kernel_selector
