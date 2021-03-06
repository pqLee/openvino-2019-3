// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <string>
#include <fstream>
#include <sstream>
#include <memory>
#include <map>

#include "debug.h"
#include "parsers.h"
#include <ie_cnn_net_reader_impl.h>
#include "ie_format_parser.h"
#include <file_utils.h>
#include <ie_plugin.hpp>
#include "xml_parse_utils.h"
#include "details/os/os_filesystem.hpp"

using namespace std;
using namespace InferenceEngine;
using namespace InferenceEngine::details;

CNNNetReaderImpl::CNNNetReaderImpl(const FormatParserCreator::Ptr& _creator)
        : parseSuccess(false), _version(0), parserCreator(_creator) {}

StatusCode CNNNetReaderImpl::SetWeights(const TBlob<uint8_t>::Ptr& weights, ResponseDesc* desc)  noexcept {
    if (!_parser) {
        return DescriptionBuffer(desc) << "network must be read first";
    }
    try {
        _parser->SetWeights(weights);
    }
    catch (const InferenceEngineException& iee) {
        return DescriptionBuffer(desc) << iee.what();
    }

    return OK;
}

size_t CNNNetReaderImpl::GetFileVersion(pugi::xml_node& root) {
    return XMLParseUtils::GetUIntAttr(root, "version", 0);
}

StatusCode CNNNetReaderImpl::ReadNetwork(const void* model, size_t size, ResponseDesc* resp) noexcept {
    if (network) {
        return DescriptionBuffer(NETWORK_NOT_READ, resp) << "Network has been read already, use new reader instance to read new network.";
    }

    pugi::xml_document xmlDoc;
    pugi::xml_parse_result res = xmlDoc.load_buffer(model, size);
    if (res.status != pugi::status_ok) {
        return DescriptionBuffer(resp) << res.description() << "at offset " << res.offset;
    }
    StatusCode ret = ReadNetwork(xmlDoc);
    if (ret != OK) {
        return DescriptionBuffer(resp) << "Error reading network: " << description;
    }
    return OK;
}

StatusCode CNNNetReaderImpl::ReadWeights(const char* filepath, ResponseDesc* resp) noexcept {
    int64_t fileSize = FileUtils::fileSize(filepath);

    if (fileSize < 0)
        return DescriptionBuffer(resp) << "filesize for: " << filepath << " - " << fileSize
            << "<0. Please, check weights file existence.";

    if (network.get() == nullptr) {
        return DescriptionBuffer(resp) << "network is empty";
    }

    auto ulFileSize = static_cast<size_t>(fileSize);

    try {
        TBlob<uint8_t>::Ptr weightsPtr(new TBlob<uint8_t>(TensorDesc(Precision::U8, {ulFileSize}, Layout::C)));
        weightsPtr->allocate();
        FileUtils::readAllFile(filepath, weightsPtr->buffer(), ulFileSize);
        return SetWeights(weightsPtr, resp);
    } catch (const InferenceEngineException& ex) {
        return DescriptionBuffer(resp) << ex.what();
    }
}

StatusCode CNNNetReaderImpl::ReadNetwork(const char* filepath, ResponseDesc* resp) noexcept {
    if (network) {
        return DescriptionBuffer(NETWORK_NOT_READ, resp) << "Network has been read already, use new reader instance to read new network.";
    }

#if defined(ENABLE_UNICODE_PATH_SUPPORT) && defined(_WIN32)
    std::wstring wFilePath = details::multiByteCharToWString(filepath);
    const wchar_t* resolvedFilepath = wFilePath.c_str();
#else
    const char* resolvedFilepath = filepath;
#endif

    pugi::xml_document xmlDoc;
    pugi::xml_parse_result res = xmlDoc.load_file(resolvedFilepath);
    if (res.status != pugi::status_ok) {
        std::ifstream t(resolvedFilepath);
        std::string str((std::istreambuf_iterator<char>(t)),
                std::istreambuf_iterator<char>());

        int line = 1;
        int pos = 0;
        for (auto token : str) {
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

        return DescriptionBuffer(resp) << "Error loading xmlfile: " << filepath << ", " << res.description()
            << " at line: " << line << " pos: " << pos;
    }
    StatusCode ret = ReadNetwork(xmlDoc);
    if (ret != OK) {
        return DescriptionBuffer(resp) << "Error reading network: " << description;
    }
    return OK;
}

StatusCode CNNNetReaderImpl::ReadNetwork(pugi::xml_document& xmlDoc) {
    description.clear();

    try {
        // check which version it is...
        pugi::xml_node root = xmlDoc.document_element();

        _version = GetFileVersion(root);
        if (_version < 2) THROW_IE_EXCEPTION << "deprecated IR version: " << _version;
        if (_version > 7) THROW_IE_EXCEPTION << "cannot parse future versions: " << _version;
        _parser = parserCreator->create(_version);
        network = _parser->Parse(root);
        name = network->getName();
        network->validate(_version);
        parseSuccess = true;
    } catch (const std::string& err) {
        description = err;
        parseSuccess = false;
        return GENERAL_ERROR;
    } catch (const InferenceEngineException& e) {
        description = e.what();
        parseSuccess = false;
        return GENERAL_ERROR;
    } catch (const std::exception& e) {
        description = e.what();
        parseSuccess = false;
        return GENERAL_ERROR;
    } catch (...) {
        description = "Unknown exception thrown";
        parseSuccess = false;
        return UNEXPECTED;
    }

    return OK;
}

std::shared_ptr<IFormatParser> V2FormatParserCreator::create(size_t version) {
    return std::make_shared<FormatParser>(version);
}

INFERENCE_ENGINE_API(InferenceEngine::ICNNNetReader*) InferenceEngine::CreateCNNNetReader() noexcept {
    return new CNNNetReaderImpl(std::make_shared<V2FormatParserCreator>());
}
