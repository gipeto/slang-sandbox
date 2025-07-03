#include <slang-gfx.h>
#include <slang.h>

#include <slangy/slangy.hpp>

// Include the headers containing the precompiled shader modules
#include <shader_lib.h>
#include <shader_main.h>

#include <cstdlib>
#include <iostream>
#include <vector>

#include <slangy/com/factories.hpp>
#include <slangy/impl/SlangIRBlob.hpp>
#include <slangy/utils/DiagnosticLogger.hpp>
#include <slangy/utils/results.hpp>

namespace
{

using namespace slang;
using namespace slangy::utils;
using namespace slangy::com;
using namespace slangy::impl;
using namespace slangy::api;

ComPtr<gfx::IPipelineState> makePipelineStateFromShader(const ComPtr<gfx::IDevice>& device,
                                                        const ComPtr<IComponentType>& slangProgram)
{
    // The `IComponentType` parameter that represents the compute
    // kernel, we can use it to create a `IShaderProgram` object in the graphics
    // layer.
    ComPtr<gfx::IShaderProgram> shaderProgram;
    gfx::IShaderProgram::Desc programDesc = {};
    programDesc.slangGlobalScope = slangProgram.get();
    throw_if_error(device->createProgram(programDesc, shaderProgram.getAddressOf()));

    // Create a compute pipeline state from `shaderProgram`.
    gfx::ComputePipelineStateDesc pipelineDesc = {};
    pipelineDesc.program = shaderProgram.get();
    ComPtr<gfx::IPipelineState> pipelineState;
    throw_if_error(device->createComputePipelineState(pipelineDesc, pipelineState.getAddressOf()));
    return pipelineState;
}

}  // namespace

int main(int argc, char* argv[])
{
    try
    {
        // Compile shaders
        auto globalSession = value_or_throw(makeGlobalSession);

        SessionDesc sDesc{};
        TargetDesc td{};
        td.format = SLANG_DXIL;
        td.profile = globalSession->findProfile("sm_6_3");

        sDesc.targets = &td;
        sDesc.targetCount = 1;

        auto session = value_or_throw(makeSession, globalSession, sDesc);

        // load the pre-compiled shader_lib module defined in shader_lib.h
        auto blob = makeFromImpl<IBlob, SlangIRBlob>(shaders::SHADER_LIB_SLANG_MODULE);

        ComPtr<IModule> libModule;
        libModule.attach(session->loadModuleFromIRBlob("shader_lib", "", blob.get(), DLog("Load shader_lib module")()));

        if (!libModule)
        {
            std::cout << "Failed to load the shader_lib module" << std::endl;
            return EXIT_FAILURE;
        }

        std::cout << "shader_lib module loaded" << std::endl;

        // load the pre-compiled shader_main module defined in shader_main.h
        auto mainBlob = makeFromImpl<IBlob, SlangIRBlob>(shaders::SHADER_MAIN_SLANG_MODULE);

        ComPtr<IModule> mainModule;
        mainModule.attach(
            session->loadModuleFromIRBlob("shader_main", "", mainBlob.get(), DLog("Load shader_main module")()));

        if (!mainModule)
        {
            std::cout << "Failed to load the shader_main module" << std::endl;
            return EXIT_FAILURE;
        }

        std::cout << "shader_main loaded" << std::endl;

        // Compose the modules and entry points.
        ComPtr<slang::IEntryPoint> computeEntryPoint;
        throw_if_error(mainModule->findEntryPointByName("computeMain", computeEntryPoint.getAddressOf()));

        std::cout << "shader_main computeMain entry point found" << std::endl;

        std::vector<slang::IComponentType*> componentTypes;
        componentTypes.push_back(mainModule.get());
        componentTypes.push_back(computeEntryPoint.get());
        componentTypes.push_back(libModule.get());

        ComPtr<slang::IComponentType> composedProgram;
        throw_if_error(session->createCompositeComponentType(componentTypes.data(), componentTypes.size(),
                                                             composedProgram.getAddressOf(),
                                                             DLog("Create composite component")()));

        // Link the composed program
        ComPtr<slang::IComponentType> linkedProgram;
        throw_if_error(composedProgram->link(linkedProgram.getAddressOf(), DLog("Link program")()));
        std::cout << "Composite program created and linked" << std::endl;

        // Execute the shader
        slangy::gfx::DebugCallback debugCall;
        auto device = value_or_throw(slangy::gfx::makeDevice, ::gfx::IDevice::Desc{}, &debugCall);
        auto commandQueue = value_or_throw(slangy::gfx::makeCommandQueue, device,
                                           ::gfx::ICommandQueue::Desc{::gfx::ICommandQueue::QueueType::Graphics});

        gfx::ITransientResourceHeap::Desc trDesc = {};
        trDesc.constantBufferSize = 4096;

        auto transientResource = value_or_throw(slangy::gfx::makeTransientResourceHeap, device, trDesc);

        ComPtr<gfx::ICommandBuffer> commandBuffer;
        throw_if_error(transientResource->createCommandBuffer(commandBuffer.getAddressOf()));

        /// Map buffer data to buffer resource
        std::array<int, 4> bufferData = {1, 2, 3, 4};
        gfx::IBufferResource::Desc bufferDesc = {};
        bufferDesc.sizeInBytes = bufferData.size() * sizeof(int);
        bufferDesc.format = gfx::Format::Unknown;
        bufferDesc.elementSize = sizeof(int);
        bufferDesc.defaultState = gfx::ResourceState::UnorderedAccess;
        bufferDesc.allowedStates = gfx::ResourceStateSet(gfx::ResourceState::UnorderedAccess);

        ComPtr<gfx::IBufferResource> bufferResource;
        throw_if_error(device->createBufferResource(bufferDesc, reinterpret_cast<void*>(bufferData.data()),
                                                    bufferResource.getAddressOf()));

        auto pipelineState = makePipelineStateFromShader(device, linkedProgram);

        if (!pipelineState)
        {
            std::cout << "Failed to create pipeline state" << std::endl;
            return EXIT_FAILURE;
        }

        ComPtr<gfx::IComputeCommandEncoder> encoder;
        commandBuffer->encodeComputeCommands(encoder.getAddressOf());

        if (!encoder)
        {
            std::cout << "Failed to create the encoder" << std::endl;
            return EXIT_FAILURE;
        }

        auto rootObject = encoder->bindPipeline(pipelineState.get());

        if (!rootObject)
        {
            std::cout << "Invalid rootObject" << std::endl;
            return EXIT_FAILURE;
        }

        ComPtr<gfx::IResourceView> bufferResourceView;
        gfx::IResourceView::Desc bwDesc = {};
        bwDesc.type = gfx::IResourceView::Type::UnorderedAccess;
        throw_if_error(
            device->createBufferView(bufferResource.get(), nullptr, bwDesc, bufferResourceView.getAddressOf()));

        throw_if_error(rootObject->setResource(gfx::ShaderOffset{0, 0, 0}, bufferResourceView.get()));

        throw_if_error(encoder->dispatchCompute(4, 1, 1));
        encoder->endEncoding();
        commandBuffer->close();

        std::cout << "Executing compute shader" << std::endl;
        commandQueue->executeCommandBuffer(commandBuffer.get());
        commandQueue->waitOnHost();

        ComPtr<ISlangBlob> resultBlob;
        throw_if_error(device->readBufferResource(bufferResource.get(), 0, bufferData.size() * sizeof(int),
                                                  resultBlob.getAddressOf()));

        std::cout << "Compute shader results: ";
        auto shaderResults = reinterpret_cast<const int*>(resultBlob->getBufferPointer());
        for (int i = 0; i < 3; ++i)
            std::cout << shaderResults[i] << ",";
        std::cout << shaderResults[3] << std::endl;

        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::cout << "Failed with error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
