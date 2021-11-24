// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "ie_core.hpp"
#include "ie_plugin_config.hpp"
#include "details/caseless.hpp"
#include "details/ie_exception_conversion.hpp"
#include "cpp_interfaces/base/ie_plugin_base.hpp"
#include "details/ie_so_pointer.hpp"
#include "multi-device/multi_device_config.hpp"

#include "ie_util_internal.hpp"
#include "file_utils.h"
#include "ie_icore.hpp"

#include <fstream>
#include <sstream>
#include <functional>
#include <string>
#include <memory>
#include <vector>
#include <utility>
#include <map>

#include "xml_parse_utils.h"

file_name_t make_plugin_name(const file_name_t &path, const file_name_t &input);

using namespace InferenceEngine::PluginConfigParams;

namespace InferenceEngine {

namespace  {

IInferencePluginAPI * getInferencePluginAPIInterface(IInferencePlugin * iplugin) {
    return dynamic_cast<IInferencePluginAPI *>(iplugin);
}

IInferencePluginAPI * getInferencePluginAPIInterface(InferenceEnginePluginPtr iplugin) {
    return getInferencePluginAPIInterface(static_cast<IInferencePlugin *>(iplugin.operator->()));
}

IInferencePluginAPI * getInferencePluginAPIInterface(InferencePlugin plugin) {
    return getInferencePluginAPIInterface(static_cast<InferenceEnginePluginPtr>(plugin));
}

}  // namespace

DeviceIDParser::DeviceIDParser(const std::string & deviceNameWithID) {
    deviceName = deviceNameWithID;

    auto pos = deviceName.find('.');
    if (pos != std::string::npos) {
        deviceName = deviceNameWithID.substr(0, pos);
        deviceID = deviceNameWithID.substr(pos + 1, deviceNameWithID.size());
    }
}

std::string DeviceIDParser::getDeviceID() const {
    return deviceID;
}

std::string DeviceIDParser::getDeviceName() const {
    return deviceName;
}

std::vector<std::string> DeviceIDParser::getHeteroDevices(std::string fallbackDevice) {
    std::vector<std::string> deviceNames;

    std::string cdevice;
    char delimiter = ',';
    size_t pos = 0;

    while ((pos = fallbackDevice.find(delimiter)) != std::string::npos) {
        deviceNames.push_back(fallbackDevice.substr(0, pos));
        fallbackDevice.erase(0, pos + 1);
    }

    if (!fallbackDevice.empty())
        deviceNames.push_back(fallbackDevice);

    return deviceNames;
}

std::vector<std::string> DeviceIDParser::getMultiDevices(std::string devicesList) {
    std::vector<std::string> deviceNames;
    auto trim_request_info = [] (std::string device_with_requests){
        auto opening_bracket = device_with_requests.find_first_of('(');
        return device_with_requests.substr(0, opening_bracket);
    };
    std::string device;
    char delimiter = ',';
    size_t pos = 0;
    // in addition to the list of devices, every device can have a #requests in the brackets e.g. "CPU(100)"
    // we skip the #requests info here
    while ((pos = devicesList.find(delimiter)) != std::string::npos) {
        auto d = devicesList.substr(0, pos);
        deviceNames.push_back(trim_request_info(d));
        devicesList.erase(0, pos + 1);
    }

    if (!devicesList.empty())
        deviceNames.push_back(trim_request_info(devicesList));

    return deviceNames;
}


class Core::Impl : public ICore {
    ITaskExecutor::Ptr          _taskExecutor = nullptr;
    mutable std::map<std::string, InferencePlugin, details::CaselessLess<std::string> > plugins;

    struct PluginDescriptor {
        FileUtils::FilePath libraryLocation;
        std::map<std::string, std::string> defaultConfig;
        std::vector<FileUtils::FilePath> listOfExtentions;
    };
    std::map<std::string, PluginDescriptor, details::CaselessLess<std::string> > pluginRegistry;
    IErrorListener * listener = nullptr;

public:
    ~Impl() override;

    /**
     * @brief Register plugins for devices which are located in .xml configuration file. The function supports UNICODE path
     * @param xmlConfigFile - an .xml configuraion with device / plugin information
     */
    void RegisterPluginsInRegistry(const std::string & xmlConfigFile) {
#if defined(ENABLE_UNICODE_PATH_SUPPORT) && defined(_WIN32)
        std::wstring wFilePath = InferenceEngine::details::multiByteCharToWString(xmlConfigFile.c_str());
        const wchar_t* resolvedFilepath = wFilePath.c_str();
#else
        const char* resolvedFilepath = xmlConfigFile.c_str();
#endif


        pugi::xml_document xmlDoc;
        pugi::xml_parse_result res = xmlDoc.load_file(resolvedFilepath);

        if (res.status != pugi::status_ok) {
            std::ifstream t(xmlConfigFile);
            std::string str((std::istreambuf_iterator<char>(t)),
                            std::istreambuf_iterator<char>());

            int line = 1;
            int pos = 0;
            for (auto && token : str) {
                if (token == '\n') {
                    line++;
                    pos = 0;
                } else {
                    pos++;
                }
                if (pos >= res.offset) {
                    break;
                }
            }

            THROW_IE_EXCEPTION << "Error loading xmlfile: " << xmlConfigFile << ", " << res.description()
                                           << " at line: " << line << " pos: " << pos;
        }

        using namespace XMLParseUtils;
        pugi::xml_node ieNode = xmlDoc.document_element();
        pugi::xml_node devicesNode = ieNode.child("plugins");

        for (auto pluginNode = devicesNode.child("plugin"); !pluginNode.empty();
             pluginNode = pluginNode.next_sibling("plugin")) {
            std::string deviceName = GetStrAttr(pluginNode, "name");
            FileUtils::FilePath pluginPath = FileUtils::toFilePath(GetStrAttr(pluginNode, "location").c_str());

            if (deviceName.find('.') != std::string::npos) {
                THROW_IE_EXCEPTION << "Device name must not contain dot '.' symbol";
            }

            // append IR library path for default IE plugins
            {
                FileUtils::FilePath absFilePath = FileUtils::makePath(getInferenceEngineLibraryPath(), pluginPath);
                if (FileUtils::fileExist(absFilePath)) pluginPath = absFilePath;
            }

            // check properties
            auto propertiesNode = pluginNode.child("properties");
            std::map<std::string, std::string> config;

            if (propertiesNode) {
                for (auto propertyNode = propertiesNode.child("property"); !propertyNode.empty();
                     propertyNode = propertyNode.next_sibling("property")) {
                    std::string key = GetStrAttr(propertyNode, "key");
                    std::string value = GetStrAttr(propertyNode, "value");
                    config[key] = value;
                }
            }

            // check extensions
            auto extensionsNode = pluginNode.child("extensions");
            std::vector<FileUtils::FilePath> listOfExtentions;

            if (extensionsNode) {
                for (auto extensionNode = extensionsNode.child("extension"); !extensionNode.empty();
                     extensionNode = extensionNode.next_sibling("extension")) {
                    FileUtils::FilePath extensionLocation = FileUtils::toFilePath(GetStrAttr(extensionNode, "location").c_str());
                    listOfExtentions.push_back(extensionLocation);
                }
            }

            // fill value in plugin registry for later lazy initialization
            {
                PluginDescriptor desc = { pluginPath, config, listOfExtentions };
                pluginRegistry[deviceName] = desc;
            }
        }
    }

    //
    // ICore public API
    //

    /**
     * @brief Returns global task executor
     * @return Reference to task executor
     */
    ITaskExecutor::Ptr GetTaskExecutor() const override {
        return _taskExecutor;
    }

    /**
     * @brief Returns reference to plugin by a device name
     * @param deviceName - a name of device
     * @return Reference to a plugin
     */
    InferenceEnginePluginPtr GetPluginByName(const std::string & deviceName) const override {
        return static_cast<InferenceEnginePluginPtr>(GetCPPPluginByName(deviceName));
    }

    /**
     * @brief Returns reference to CPP plugin wrapper by a device name
     * @param deviceName - a name of device
     * @return Reference to a CPP plugin wrapper
     */
    InferencePlugin GetCPPPluginByName(const std::string & deviceName) const {
        auto it = pluginRegistry.find(deviceName);
        if (it == pluginRegistry.end()) {
            THROW_IE_EXCEPTION << "Device with \"" << deviceName << "\" name is not registered in the InferenceEngine";
        }

        // Plugin is in registry, but not created, let's create

        if (plugins.find(deviceName) == plugins.end()) {
            PluginDescriptor desc = it->second;

            try {
                InferenceEnginePluginPtr plugin(desc.libraryLocation);
                IInferencePlugin * pplugin = static_cast<IInferencePlugin *>(plugin.operator->());
                IInferencePluginAPI * iplugin_api_ptr = dynamic_cast<IInferencePluginAPI *>(pplugin);

                if (iplugin_api_ptr != nullptr) {
                    iplugin_api_ptr->SetName(deviceName);

                    // Set Inference Engine class reference to plugins
                    ICore * mutableCore = const_cast<ICore *>(static_cast<const ICore *>(this));
                    iplugin_api_ptr->SetCore(mutableCore);
                }

                InferencePlugin cppPlugin(plugin);

                // configuring
                {
                    cppPlugin.SetConfig(desc.defaultConfig);

                    for (auto&& extensionLocation : desc.listOfExtentions) {
                        // TODO: fix once InferenceEngine::Extension can accept FileUtils::FilePath
                        //       currently, extensions cannot be loaded using wide path
                        cppPlugin.AddExtension(make_so_pointer<IExtension>(FileUtils::fromFilePath(extensionLocation)));
                    }

                    if (listener)
                        plugin->SetLogCallback(*listener);
                }

                plugins[deviceName] = cppPlugin;
            } catch (const details::InferenceEngineException& ex) {
                THROW_IE_EXCEPTION << "Failed to create plugin " << FileUtils::fromFilePath(desc.libraryLocation)
                                   << " for device " << deviceName << "\n"
                                   << "Please, check your environment\n"
                                   << ex.what() << "\n";
            }
        }

        return plugins[deviceName];
    }

    /**
     * @brief Unregisters plugin for specified device
     * @param deviceName - a name of device
     */
    void UnregisterPluginByName(const std::string & deviceName) {
        auto it = plugins.find(deviceName);
        if (it == plugins.end()) {
            THROW_IE_EXCEPTION << "Device with \"" << deviceName << "\" name is not registered in the InferenceEngine";
        }

        plugins.erase(deviceName);
    }

    /**
     * @brief Registers plugin in registry for specified device
     * @param deviceName - a name of device
     */
    void RegisterPluginByName(const std::string & pluginName, const std::string & deviceName) {
        auto it = pluginRegistry.find(deviceName);
        if (it != pluginRegistry.end()) {
            THROW_IE_EXCEPTION << "Device with \"" << deviceName << "\"  is already registered in the InferenceEngine";
        }

        if (deviceName.find('.') != std::string::npos) {
            THROW_IE_EXCEPTION << "Device name must not contain dot '.' symbol";
        }

        // append IR library path for default IE plugins
        FileUtils::FilePath pluginPath;
        {
            pluginPath = FileUtils::makeSharedLibraryName({}, FileUtils::toFilePath(pluginName.c_str()));

            FileUtils::FilePath absFilePath = FileUtils::makePath(getInferenceEngineLibraryPath(), pluginPath);
            if (FileUtils::fileExist(absFilePath)) pluginPath = absFilePath;
        }

        PluginDescriptor desc = { pluginPath, { }, { } };
        pluginRegistry[deviceName] = desc;
    }

    std::vector<std::string> GetListOfDevicesInRegistry() const {
        std::vector<std::string> listOfDevices;
        for (auto && pluginDesc : pluginRegistry) {
            listOfDevices.push_back(pluginDesc.first);
        }

        return listOfDevices;
    }

    void SetConfigForPlugins(const std::map<std::string, std::string> & config, const std::string & deviceName) {
        // set config for plugins in registry
        for (auto & desc : pluginRegistry) {
            if (deviceName.empty() || deviceName == desc.first) {
                for (auto && conf : config) {
                    desc.second.defaultConfig[conf.first] = conf.second;
                }
            }
        }

        // set config for already created plugins
        for (auto & plugin : plugins) {
            if (deviceName.empty() || deviceName == plugin.first) {
                plugin.second.SetConfig(config);
            }
        }
    }

    void SetErrorListener(IErrorListener * list) {
        listener = list;

        // set for already created plugins
        for (auto & plugin : plugins) {
            GetPluginByName(plugin.first)->SetLogCallback(*listener);
        }
    }
};

Core::Impl::~Impl() {
}

Core::Core(const std::string & xmlConfigFile) {
    _impl = std::make_shared<Impl>();

    std::string xmlConfigFile_ = xmlConfigFile;
    if (xmlConfigFile_.empty()) {
        // register plugins from default plugins.xml config
        FileUtils::FilePath xmlConfigFileDefault = FileUtils::makePath(getInferenceEngineLibraryPath(), FileUtils::toFilePath("plugins.xml"));
        xmlConfigFile_ = FileUtils::fromFilePath(xmlConfigFileDefault);
    }

    RegisterPlugins(xmlConfigFile_);
}

std::map<std::string, Version> Core::GetVersions(const std::string & deviceName) const {
    std::map<std::string, Version> versions;
    std::vector<std::string> deviceNames;

    {
        // for compatibility with samples / demo
        if (deviceName.find("HETERO:") == 0) {
            deviceNames = DeviceIDParser::getHeteroDevices(deviceName.substr(7));
            deviceNames.push_back("HETERO");
        } else  if (deviceName.find("MULTI") == 0) {
            deviceNames.push_back("MULTI");
            deviceNames = DeviceIDParser::getMultiDevices(deviceName.substr(6));
        } else {
            deviceNames.push_back(deviceName);
        }
    }

    for (auto && deviceName_ : deviceNames) {
        DeviceIDParser parser(deviceName_);
        std::string deviceNameLocal = parser.getDeviceName();

        const Version * version = _impl->GetCPPPluginByName(deviceNameLocal).GetVersion();
        versions[deviceNameLocal] = *version;;
    }

    return versions;
}

void Core::SetLogCallback(IErrorListener &listener) const {
    _impl->SetErrorListener(&listener);
}

ExecutableNetwork Core::LoadNetwork(CNNNetwork network, const std::string & deviceName,
                                    const std::map<std::string, std::string> & config) {
    std::map<std::string, std::string> config_ = config;
    std::string deviceName_ = deviceName;

    if (deviceName_.find("HETERO:") == 0) {
        deviceName_ = "HETERO";
        config_["TARGET_FALLBACK"] = deviceName.substr(7);
    } else if (deviceName_.find("MULTI:") == 0) {
        deviceName_ = "MULTI";
        config_[InferenceEngine::MultiDeviceConfigParams::KEY_MULTI_DEVICE_PRIORITIES] = deviceName.substr(6);
    } else {
        DeviceIDParser parser(deviceName_);
        deviceName_ = parser.getDeviceName();
        std::string deviceIDLocal = parser.getDeviceID();

        if (!deviceIDLocal.empty()) {
            config_[KEY_DEVICE_ID] = deviceIDLocal;
        }
    }

    return _impl->GetCPPPluginByName(deviceName_).LoadNetwork(network, config_);
}

void Core::AddExtension(IExtensionPtr extension, const std::string & deviceName_) {
    if (deviceName_.find("HETERO") == 0) {
        THROW_IE_EXCEPTION << "HETERO device does not support extensions. Please, set extensions directly to fallback devices";
    }
    if (deviceName_.find("MULTI") == 0) {
        THROW_IE_EXCEPTION << "MULTI device does not support extensions. Please, set extensions directly to fallback devices";
    }

    DeviceIDParser parser(deviceName_);
    std::string deviceName = parser.getDeviceName();

    _impl->GetCPPPluginByName(deviceName).AddExtension(extension);
}

ExecutableNetwork Core::ImportNetwork(const std::string &modelFileName, const std::string & deviceName_,
                                      const std::map<std::string, std::string> &config_) {
    if (deviceName_.find("HETERO") == 0) {
        THROW_IE_EXCEPTION << "HETERO device does not support ImportNetwork";
    }
    if (deviceName_.find("MULTI") == 0) {
        THROW_IE_EXCEPTION << "MULTI device does not support ImportNetwork";
    }

    DeviceIDParser parser(deviceName_);
    std::string deviceName = parser.getDeviceName();
    std::string deviceID = parser.getDeviceID();
    auto config = config_;

    // add DEVICE_ID from DEVICE_NAME.# if # is here
    if (!deviceID.empty()) {
        config[KEY_DEVICE_ID] = deviceID;
    }

    return _impl->GetCPPPluginByName(deviceName).ImportNetwork(modelFileName, config);
}

QueryNetworkResult Core::QueryNetwork(const ICNNNetwork &network, const std::string & deviceName,
                                      const std::map<std::string, std::string> & config) const {
    QueryNetworkResult res;
    auto config_ = config;
    std::string deviceName_ = deviceName;

    if (deviceName_.find("MULTI") == 0) {
        THROW_IE_EXCEPTION << "MULTI device does not support QueryNetwork";
    }

    if (deviceName_.find("HETERO:") == 0) {
        deviceName_ = "HETERO";
        config_["TARGET_FALLBACK"] = deviceName.substr(7);
    } else {
        DeviceIDParser parser(deviceName_);
        deviceName_ = parser.getDeviceName();
        std::string deviceIDLocal = parser.getDeviceID();

        if (!deviceIDLocal.empty()) {
            config_[KEY_DEVICE_ID] = deviceIDLocal;
        }
    }

    _impl->GetCPPPluginByName(deviceName_).QueryNetwork(network, config_, res);
    return res;
}

void Core::SetConfig(const std::map<std::string, std::string> & config_, const std::string & deviceName_) {
    // HETERO case
    {
        if (deviceName_.find("HETERO:") == 0) {
            THROW_IE_EXCEPTION << "SetConfig is supported only for HETERO itself (without devices). "
                                  "You can configure the devices with SetConfig before creating the HETERO on top.";
        }

        if (config_.find("TARGET_FALLBACK") != config_.end()) {
            THROW_IE_EXCEPTION << "Please, specify TARGET_FALLBACK to the LoadNetwork directly, "
                                  "as you will need to pass the same TARGET_FALLBACK anyway.";
        }
    }

    // MULTI case
    {
        if (deviceName_.find("MULTI:") == 0) {
            THROW_IE_EXCEPTION << "SetConfig is supported only for MULTI itself (without devices). "
                                  "You can configure the devices with SetConfig before creating the MULTI on top.";
        }

        if (config_.find(MultiDeviceConfigParams::KEY_MULTI_DEVICE_PRIORITIES) != config_.end()) {
            THROW_IE_EXCEPTION << "Please, specify DEVICE_PRIORITIES to the LoadNetwork directly, "
                                  "as you will need to pass the same DEVICE_PRIORITIES anyway.";
        }
    }

    if (deviceName_.empty()) {
        _impl->SetConfigForPlugins(config_, std::string());
    } else {
        DeviceIDParser parser(deviceName_);
        std::string deviceName = parser.getDeviceName();
        std::string deviceID = parser.getDeviceID();

        auto config = config_;

        // add DEVICE_ID from DEVICE_NAME.# if # is here
        if (!deviceID.empty()) {
            config[KEY_DEVICE_ID] = deviceID;
        }

        _impl->SetConfigForPlugins(config, deviceName);
    }
}

Parameter Core::GetConfig(const std::string & deviceName_, const std::string & name) const {
    // HETERO case
    {
        if (deviceName_.find("HETERO:") == 0) {
            THROW_IE_EXCEPTION << "You can only GetConfig of the HETERO itself (without devices). "
                                  "GetConfig is also possible for the individual devices before creating the HETERO on top.";
        }
    }
    // MULTI case
    {
        if (deviceName_.find("MULTI:") == 0) {
            THROW_IE_EXCEPTION << "You can only GetConfig of the MULTI itself (without devices). "
                                  "GetConfig is also possible for the individual devices before creating the MULTI on top.";
        }
    }

    DeviceIDParser device(deviceName_);
    std::string deviceName = device.getDeviceName();
    std::string deviceID = device.getDeviceID();

    auto pluginAPIInterface = getInferencePluginAPIInterface(_impl->GetCPPPluginByName(deviceName));

    if (pluginAPIInterface == nullptr) {
        THROW_IE_EXCEPTION << deviceName << " does not implement the GetConfig method";
    }

    std::map<std::string, Parameter> config;
    if (!deviceID.empty()) {
        config[KEY_DEVICE_ID] = deviceID;
    }

    return pluginAPIInterface->GetConfig(name, config);
}

Parameter Core::GetMetric(const std::string & deviceName_, const std::string & name) const {
    // HETERO case
    {
        if (deviceName_.find("HETERO:") == 0) {
            THROW_IE_EXCEPTION
                    << "You can get specific metrics with the GetMetric only for the HETERO itself (without devices). "
                       "To get individual devices's metrics call GetMetric for each device separately";
        }
    }

    // MULTI case
    {
        if (deviceName_.find("MULTI:") == 0) {
            THROW_IE_EXCEPTION
                    << "You can get specific metrics with the GetMetric only for the MULTI itself (without devices). "
                       "To get individual devices's metrics call GetMetric for each device separately";
        }
    }

    DeviceIDParser device(deviceName_);
    std::string deviceName = device.getDeviceName();
    std::string deviceID = device.getDeviceID();

    auto pluginAPIInterface = getInferencePluginAPIInterface(_impl->GetCPPPluginByName(deviceName));

    if (pluginAPIInterface == nullptr) {
        THROW_IE_EXCEPTION << deviceName << " does not implement the GetMetric method";
    }

    std::map<std::string, Parameter> config;
    if (!deviceID.empty()) {
        config[KEY_DEVICE_ID] = deviceID;
    }

    return pluginAPIInterface->GetMetric(name, config);
}

std::vector<std::string> Core::GetAvailableDevices() const {
    std::vector<std::string> devices;

    std::string propertyName = METRIC_KEY(AVAILABLE_DEVICES);

    for (auto && deviceName : _impl->GetListOfDevicesInRegistry()) {
        Parameter p;
        std::vector<std::string> devicesIDs;

        try {
            p = GetMetric(deviceName, propertyName);
            devicesIDs = p.as<std::vector<std::string> >();
        } catch (details::InferenceEngineException &) {
            // plugin is not created by e.g. invalid env
        } catch (const std::exception & ex) {
            THROW_IE_EXCEPTION << "An exception is thrown while trying to create the " <<
                deviceName << " device and call GetMetric: " << ex.what();
        } catch (...) {
            THROW_IE_EXCEPTION << "Unknown exception is thrown while trying to create the " <<
                deviceName << " device and call GetMetric";
        }

        if (devicesIDs.size() > 1) {
            for (auto && deviceID : devicesIDs) {
                devices.push_back(deviceName + '.' + deviceID);
            }
        } else if (!devicesIDs.empty()) {
            devices.push_back(deviceName);
        }
    }

    return devices;
}

void Core::RegisterPlugin(const std::string & pluginName, const std::string & deviceName) {
    _impl->RegisterPluginByName(pluginName, deviceName);
}

void Core::RegisterPlugins(const std::string & xmlConfigFile) {
    _impl->RegisterPluginsInRegistry(xmlConfigFile);
}

void Core::UnregisterPlugin(const std::string & deviceName_) {
    DeviceIDParser parser(deviceName_);
    std::string deviceName = parser.getDeviceName();

    _impl->UnregisterPluginByName(deviceName);
}

}  // namespace InferenceEngine
