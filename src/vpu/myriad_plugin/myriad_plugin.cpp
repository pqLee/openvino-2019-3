// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <memory>
#include <vector>
#include <tuple>

#include <ie_metric_helpers.hpp>
#include <inference_engine.hpp>
#include <cpp_interfaces/base/ie_plugin_base.hpp>
#include <cpp_interfaces/impl/ie_executable_network_internal.hpp>

#include <vpu/vpu_plugin_config.hpp>
#include <vpu/parsed_config.hpp>
#include <vpu/utils/profiling.hpp>

#include "myriad_plugin.h"

using namespace InferenceEngine;
using namespace InferenceEngine::PluginConfigParams;
using namespace InferenceEngine::VPUConfigParams;
using namespace vpu::MyriadPlugin;

ExecutableNetworkInternal::Ptr Engine::LoadExeNetworkImpl(const ICore * /*core*/, ICNNNetwork &network,
                                                          const std::map<std::string, std::string> &config) {
    VPU_PROFILE(LoadExeNetworkImpl);
    InputsDataMap networkInputs;
    OutputsDataMap networkOutputs;

    network.getInputsInfo(networkInputs);
    network.getOutputsInfo(networkOutputs);

    IE_SUPPRESS_DEPRECATED_START
    auto specifiedDevice = network.getTargetDevice();
    auto supportedDevice = InferenceEngine::TargetDevice::eMYRIAD;
    if (specifiedDevice != InferenceEngine::TargetDevice::eDefault && specifiedDevice != supportedDevice) {
        THROW_IE_EXCEPTION << "The plugin doesn't support target device: " << getDeviceName(specifiedDevice) << ".\n" <<
                           "Supported target device: " << getDeviceName(supportedDevice);
    }
    IE_SUPPRESS_DEPRECATED_END

    // override what was set globally for plugin, otherwise - override default config without touching config for plugin
    auto configCopy = _config;
    for (auto &&entry : config) {
        configCopy[entry.first] = entry.second;
    }

    return std::make_shared<ExecutableNetwork>(network, _devicePool, configCopy);
}

void Engine::SetConfig(const std::map<std::string, std::string> &userConfig) {
    MyriadConfig myriadConfig(userConfig);

    for (auto &&entry : userConfig) {
        _config[entry.first] = entry.second;
    }
}

Parameter Engine::GetConfig(const std::string& name, const std::map<std::string, Parameter>& options) const {
    auto supported_keys = _metrics->SupportedConfigKeys();
    if (std::find(supported_keys.begin(),
        supported_keys.end(), name) == supported_keys.end()) {
        THROW_IE_EXCEPTION << "Unsupported config key : " << name;
    }

    Parameter result;
    auto option = _config.find(name);
    if (option != _config.end())
        result = option->second;

    return result;
}

void Engine::QueryNetwork(const ICNNNetwork& network, QueryNetworkResult& res) const {
    QueryNetwork(network, {}, res);
}

void Engine::QueryNetwork(const ICNNNetwork& network, const std::map<std::string, std::string>& config,
                          QueryNetworkResult& res) const {
    VPU_PROFILE(QueryNetwork);
    auto layerNames = getSupportedLayers(
        network,
        Platform::MYRIAD_2,
        CompilationConfig(),
        std::make_shared<Logger>("GraphCompiler", LogLevel::None, consoleOutput()));

    for (auto && layerName : layerNames) {
        res.supportedLayersMap.insert({ layerName, GetName() });
    }

    IE_SUPPRESS_DEPRECATED_START
    res.supportedLayers.insert(layerNames.begin(), layerNames.end());
    IE_SUPPRESS_DEPRECATED_END
}

Engine::Engine(std::shared_ptr<IMvnc> mvnc) :
    _mvnc(mvnc),
    _metrics(std::make_shared<MyriadMetrics>()) {
    if (!_mvnc) {
        THROW_IE_EXCEPTION << "mvnc is invalid";
    }

    MyriadConfig config;
    _config = config.getDefaultConfig();
    _pluginName = "MYRIAD";
}

// TODO: ImportNetwork and LoadNetwork handle the config parameter in different ways.
// ImportNetwork gets a config provided by an user. LoadNetwork gets the plugin config and merge it with user's config.
// Need to found a common way to handle configs
IExecutableNetwork::Ptr Engine::ImportNetwork(const std::string &modelFileName, const std::map<std::string, std::string> &config) {
    VPU_PROFILE(ImportNetwork);
    std::ifstream blobFile(modelFileName, std::ios::binary);

    if (!blobFile.is_open()) {
        THROW_IE_EXCEPTION << ie::details::as_status << NETWORK_NOT_READ;
    }

    IExecutableNetwork::Ptr executableNetwork;
    // Use config provided by an user ignoring default config
    executableNetwork.reset(new ExecutableNetworkBase<ExecutableNetworkInternal>(
                                std::make_shared<ExecutableNetwork>(modelFileName, _devicePool, config)), [](ie::details::IRelease *p) {p->Release();});

    return executableNetwork;
}

InferenceEngine::Parameter Engine::GetMetric(const std::string& name,
                                     const std::map<std::string, InferenceEngine::Parameter> & options) const {
    if (name == METRIC_KEY(AVAILABLE_DEVICES)) {
        IE_SET_METRIC_RETURN(AVAILABLE_DEVICES, _metrics->AvailableDevicesNames(_mvnc, _devicePool));
    } else if (name == METRIC_KEY(FULL_DEVICE_NAME)) {
        auto availableDevices = _metrics->AvailableDevicesNames(_mvnc, _devicePool);

        if (!availableDevices.size()) {
            THROW_IE_EXCEPTION << "No devices available.";
        }

        if (!options.count(KEY_DEVICE_ID)) {
            if (availableDevices.size() == 1) {
                return _metrics->FullName(availableDevices[0]);
            } else {
                THROW_IE_EXCEPTION << "KEY_DEVICE_ID is undefined.";
            }
        }

        auto deviceName = options.at(KEY_DEVICE_ID).as<std::string>();
        IE_SET_METRIC_RETURN(FULL_DEVICE_NAME, _metrics->FullName(deviceName));
    } else if (name == METRIC_KEY(SUPPORTED_METRICS)) {
        IE_SET_METRIC_RETURN(SUPPORTED_METRICS, _metrics->SupportedMetrics());
    } else if (name == METRIC_KEY(SUPPORTED_CONFIG_KEYS)) {
        IE_SET_METRIC_RETURN(SUPPORTED_CONFIG_KEYS, _metrics->SupportedConfigKeys());
    } else if (name == METRIC_KEY(OPTIMIZATION_CAPABILITIES)) {
        IE_SET_METRIC_RETURN(OPTIMIZATION_CAPABILITIES, _metrics->OptimizationCapabilities());
    } else if (name == METRIC_KEY(RANGE_FOR_ASYNC_INFER_REQUESTS)) {
        IE_SET_METRIC_RETURN(RANGE_FOR_ASYNC_INFER_REQUESTS, _metrics->RangeForAsyncInferRequests(_config));
    }

    THROW_IE_EXCEPTION << NOT_IMPLEMENTED_str;
}
