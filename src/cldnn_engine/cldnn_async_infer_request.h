// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <string>
#include <map>
#include <cpp_interfaces/impl/ie_infer_async_request_thread_safe_default.hpp>
#include "cldnn_infer_request.h"

namespace CLDNNPlugin {

class CLDNNAsyncInferRequest : virtual public InferenceEngine::AsyncInferRequestThreadSafeDefault {
public:
    CLDNNAsyncInferRequest(const InferenceEngine::InferRequestInternal::Ptr &inferRequest,
                            const InferenceEngine::ITaskExecutor::Ptr &taskExecutor,
                            const InferenceEngine::TaskSynchronizer::Ptr &taskSynchronizer,
                            const InferenceEngine::ITaskExecutor::Ptr &callbackExecutor);

    ~CLDNNAsyncInferRequest() override;

    void Infer() override;
};

}  // namespace CLDNNPlugin
