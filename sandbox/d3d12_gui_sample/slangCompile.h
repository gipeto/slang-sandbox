#pragma once

#define WINRT_LEAN_AND_MEAN
#include <winrt/base.h>

#include <d3d12.h>
#include <d3dcompiler.h>

#include <slangy/api/api.hpp>
#include <slangy/com/ComPtr.hpp>
#include <slangy/com/factories.hpp>
#include <slangy/impl/SlangIRBlob.hpp>
#include <slangy/utils/DiagnosticLogger.hpp>
#include <slangy/utils/results.hpp>

/// link precompiled modules to build a shader program returned as a direct 3d blob
template <typename... PreCompiledModule>
winrt::com_ptr<ID3DBlob> buildShader(const slangy::com::ComPtr<slang::IGlobalSession>& globalSession, std::string name,
                                     std::string entryPointName, PreCompiledModule&&... precompiledModules)
{
    using namespace slang;
    using namespace slangy::utils;
    using namespace slangy::com;
    using namespace slangy::impl;
    using namespace slangy::api;

    SessionDesc sDesc{};
    TargetDesc td{};
    td.format = SLANG_DXIL;
    td.profile = globalSession->findProfile("sm_6_3");
    sDesc.targets = &td;
    sDesc.targetCount = 1;

    auto session = value_or_throw(makeSession, globalSession, sDesc);
    session->AddRef();  /// TODO remove this line. The AddRef is wrong and will leak the session, but there is an issue
                        /// with the release of the session causing a crash

    std::vector<ComPtr<IBlob>> blobs;
    (blobs.push_back(makeFromImpl<IBlob, SlangIRBlob>(std::forward<PreCompiledModule>(precompiledModules))), ...);

    std::vector<ComPtr<IModule>> modules(blobs.size());
    std::vector<slang::IComponentType*> componentTypes;
    ComPtr<IEntryPoint> entryPoint;

    for (size_t i = 0; i < blobs.size(); ++i)
    {
        std::string moduleName = name + std::to_string(i);
        std::string logPrefix = std::string("Load module ") + moduleName;
        modules[i].attach(
            session->loadModuleFromIRBlob(moduleName.c_str(), "", blobs[i].get(), DLog(std::move(logPrefix))()));

        if (!modules[i])
        {
            throw std::runtime_error("Failed to load module " + moduleName);
        }

        if (!entryPoint)
        {
            if (SLANG_OK == modules[i]->findEntryPointByName(entryPointName.c_str(), entryPoint.getAddressOf()))
            {
                componentTypes.push_back(entryPoint.get());
            }
        }

        componentTypes.push_back(modules[i].get());
    }

    ComPtr<slang::IComponentType> composedProgram;
    throw_if_error(session->createCompositeComponentType(componentTypes.data(), componentTypes.size(),
                                                         composedProgram.getAddressOf(),
                                                         DLog("Create composite component")()));

    // Link the composed program
    ComPtr<slang::IComponentType> linkedProgram;
    throw_if_error(composedProgram->link(linkedProgram.getAddressOf(), DLog("Link program")()));

    ComPtr<IBlob> shaderBlob;
    throw_if_error(linkedProgram->getTargetCode(0, shaderBlob.getAddressOf(), DLog("Retrieve shader program")()));

    winrt::com_ptr<ID3DBlob> result;
    if (S_OK != D3DCreateBlob(shaderBlob->getBufferSize(), result.put()))
    {
        throw std::runtime_error("Failed to create D3DBlob");
    }

    std::copy_n(reinterpret_cast<const char*>(shaderBlob->getBufferPointer()), shaderBlob->getBufferSize(),
                reinterpret_cast<char*>(result->GetBufferPointer()));
    return result;
}
