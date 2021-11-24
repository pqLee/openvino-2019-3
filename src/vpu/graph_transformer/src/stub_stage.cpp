// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <vpu/stub_stage.hpp>

#include <memory>
#include <vector>
#include <utility>

#include <vpu/model/edges.hpp>
#include <vpu/model/data.hpp>

namespace vpu {

StagePtr StubStage::cloneImpl() const {
    return std::make_shared<StubStage>(*this);
}

void StubStage::propagateScaleFactorsImpl(
        const SmallVector<float>& inputScales,
        ScalePropagationStep step,
        StageDataInfo<float>& scaleInfo) {
    if (_type == StageType::StubConv ||
        _type == StageType::StubFullyConnected ||
        _type == StageType::StubDeconv) {
        auto weights = inputEdge(1)->input();
        auto biases = inputEdge(2)->input();

        IE_ASSERT(weights->usage() == DataUsage::Const);
        IE_ASSERT(biases->usage() == DataUsage::Const || biases->usage() == DataUsage::Fake);

        auto inputScale = inputScales[0];

        scaleInfo.setInput(inputEdge(1), step == ScalePropagationStep::Propagate ? 1.0f : inputScale);
        if (biases->usage() == DataUsage::Const) {
            scaleInfo.setInput(inputEdge(2), inputScale);
        }
        scaleInfo.setOutput(outputEdge(0), inputScale);
    } else {
        IE_ASSERT(_type == StageType::StubMaxPool || _type == StageType::StubAvgPool);

        auto input = inputEdge(0)->input();
        auto output = outputEdge(0)->output();

        scaleInfo.setOutput(outputEdge(0), inputScales[0]);
    }
}

void StubStage::propagateDataOrderImpl(StageDataInfo<DimsOrder>&) {
    VPU_THROW_EXCEPTION << "Must be replaced with real stage";
}

void StubStage::getDataStridesRequirementsImpl(StageDataInfo<StridesRequirement>&) {
    VPU_THROW_EXCEPTION << "Must be replaced with real stage";
}

void StubStage::finalizeDataLayoutImpl() {
    VPU_THROW_EXCEPTION << "Must be replaced with real stage";
}

void StubStage::getBatchSupportInfoImpl(StageDataInfo<BatchSupport>& batchInfo) {
    if (_type == StageType::StubConv ||
        _type == StageType::StubFullyConnected ||
        _type == StageType::StubDeconv) {
        auto weights = inputEdge(1)->input();
        auto biases = inputEdge(2)->input();

        IE_ASSERT(weights->usage() == DataUsage::Const);
        IE_ASSERT(biases->usage() == DataUsage::Const || biases->usage() == DataUsage::Fake);

        batchInfo.setInput(inputEdge(0), BatchSupport::Split);
        batchInfo.setOutput(outputEdge(0), BatchSupport::Split);
    } else {
        IE_ASSERT(_type == StageType::StubMaxPool || _type == StageType::StubAvgPool);

        // Pooling will support batch by merging it with previous dimension.
    }
}

void StubStage::initialCheckImpl() const {
    if (_type == StageType::StubConv || _type == StageType::StubFullyConnected || _type == StageType::StubDeconv) {
        assertInputsOutputsTypes(this,
            {{DataType::FP16}, {DataType::FP16}, {DataType::FP16}},
            {{DataType::FP16}});
    } else if (_type == StageType::StubMaxPool || _type == StageType::StubAvgPool) {
        assertInputsOutputsTypes(this, {{DataType::FP16}}, {{DataType::FP16}});
    } else {
        VPU_THROW_EXCEPTION << "unknown type";
    }
}

void StubStage::finalCheckImpl() const {
    VPU_THROW_EXCEPTION << "Must never be called";
}

void StubStage::serializeParamsImpl(BlobSerializer&) const {
    VPU_THROW_EXCEPTION << "Must never be called";
}

void StubStage::serializeDataImpl(BlobSerializer&) const {
    VPU_THROW_EXCEPTION << "Must never be called";
}

}  // namespace vpu
