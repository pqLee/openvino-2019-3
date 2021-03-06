// Copyright (c) 2018 Intel Corporation
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


#include "fully_connected_grad_weights_kernel_base.h"
#include "kernel_selector_utils.h"
#include <algorithm>
#include <vector>

namespace kernel_selector {
JitConstants FullyConnectedGradWeightsKernelBase::GetJitConstants(
    const fully_connected_grad_weights_params& params) const {
    JitConstants jit = training_kernel_base::GetJitConstants(params);

    return jit;
}

FullyConnectedGradWeightsKernelBase::DispatchData FullyConnectedGradWeightsKernelBase::SetDefault(
    const fully_connected_grad_weights_params& params) const {
    DispatchData kd;

    kd.fp16UnitUsed = params.inputs[0].GetDType() == Datatype::F16;
    size_t gws0 = params.weights.OFM().v * params.weights.IFM().v;
    size_t lws0 = std::min(gws0, static_cast<size_t>(32));
    while (gws0 % lws0) {
        lws0--;
    }
    kd.gws0 = gws0;
    kd.gws1 = params.weights.X().v;
    kd.gws2 = params.weights.Y().v;
    kd.lws0 = lws0;
    kd.lws1 = 1;
    kd.lws2 = 1;
    kd.effiency = DONT_USE_IF_HAVE_SOMETHING_ELSE;
    return kd;
}

KernelsData FullyConnectedGradWeightsKernelBase::GetKernelsData(const Params& params,
                                                                const optional_params& options) const {
    assert(params.GetType() == KernelType::FULLY_CONNECTED_GRAD_WEIGHTS);

    const fully_connected_grad_weights_params& orgParams =
        static_cast<const fully_connected_grad_weights_params&>(params);

    const std::vector<WeightsLayout> weightsLayouts = {WeightsLayout::oi,
                                                       WeightsLayout::io,
                                                       WeightsLayout::oiyx,
                                                       WeightsLayout::iyxo,
                                                       WeightsLayout::yxio,
                                                       WeightsLayout::oyxi};

    DispatchData runInfo = SetDefault(orgParams);
    KernelData kd = KernelData::Default<fully_connected_grad_weights_params>(params);
    fully_connected_grad_weights_params& newParams =
        *static_cast<fully_connected_grad_weights_params*>(kd.params.get());

    bool succeed = UpdateWeightsParams(newParams, options, weightsLayouts, kd.weightsReorderParams);

    if (!succeed) {
        return {};
    }

    auto cldnn_jit = GetJitConstants(orgParams);
    auto entry_point = GetEntryPoint(kernelName, orgParams.layerID, options);
    auto jit = CreateJit(kernelName, cldnn_jit, entry_point);

    auto& kernel = kd.kernels[0];
    FillCLKernelData(kernel,
                     runInfo,
                     params.engineInfo,
                     kernelName,
                     jit,
                     entry_point,
                     DEFAULT,
                     true,
                     !orgParams.bias.empty());
    if (orgParams.use_momentum) {
        kernel.arguments.push_back({ArgumentDescriptor::Types::PREV_WEIGHTS_GRADIENT, 0});
        if (!orgParams.bias.empty())
            kernel.arguments.push_back({ArgumentDescriptor::Types::PREV_BIAS_GRADIENT, 0});
    }
    kernel.arguments.push_back({ArgumentDescriptor::Types::INPUT, 1});
    kernel.arguments.push_back({ArgumentDescriptor::Types::LEARNING_RATE, 0});

    kd.estimatedTime = runInfo.effiency;

    return {kd};
}
}  // namespace kernel_selector