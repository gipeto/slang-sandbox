#include <slang-gfx.h>
#include <slang.h>

#include <slangy/slangy.hpp>

// Include the headers containing the precompiled shader modules
#include <shader_lib.h>
#include <shader_main.h>

#include <cstdlib>
#include <iostream>
#include <vector>

#include <slangy/com/helpers.hpp>
#include <slangy/utils/DiagnosticLogger.hpp>
#include <slangy/utils/SlangBlob.hpp>

#include "DebugCallback.hpp"
#include "gfx.hpp"

namespace
{

using namespace slang;
using namespace slangy;

ComPtr<gfx::IPipelineState> makePipelineStateFromShader(const ComPtr<gfx::IDevice>& device,
                                                        const ComPtr<IComponentType>& slangProgram)
{
    // The `IComponentType` parameter that represents the compute
    // kernel, we can use it to create a `IShaderProgram` object in the graphics
    // layer.
    ComPtr<gfx::IShaderProgram> shaderProgram;
    gfx::IShaderProgram::Desc programDesc = {};
    programDesc.slangGlobalScope = slangProgram.get();
    throwIfError(device->createProgram(programDesc, shaderProgram.put()));

    // Create a compute pipeline state from `shaderProgram`.
    gfx::ComputePipelineStateDesc pipelineDesc = {};
    pipelineDesc.program = shaderProgram.get();
    ComPtr<gfx::IPipelineState> pipelineState;
    throwIfError(device->createComputePipelineState(pipelineDesc, pipelineState.put()));
    return pipelineState;
}

}  // namespace

int main(int argc, char* argv[])
{
    try
    {
        // Compile shaders
        auto globalSession = valueOrThrow(makeGlobalSession());

        SessionDesc sDesc{};
        TargetDesc td{};

#ifdef _WIN32
        td.format = SLANG_DXIL;
#elif __APPLE__
        td.format = SLANG_METAL_LIB;
#else
        td.format = SLANG_SPIRV;
#endif

        td.profile = globalSession->findProfile("");

        sDesc.targets = &td;
        sDesc.targetCount = 1;

        auto session = valueOrThrow(makeSession(globalSession, sDesc));

        // load the pre-compiled shader_lib module defined in shader_lib.h
        auto blob = makeFromImpl<IBlob, SlangBlob>(shaders::SHADER_LIB_SLANG_MODULE::blob);

        ComPtr<IModule> libModule;
        libModule.attach(session->loadModuleFromIRBlob("shader_lib", "", blob.get(), DLog("Load shader_lib module")()));

        if (!libModule)
        {
            std::cout << "Failed to load the shader_lib module" << std::endl;
            return EXIT_FAILURE;
        }

        std::cout << "shader_lib module loaded" << std::endl;

        // load the pre-compiled shader_main module defined in shader_main.h
        auto mainBlob = makeFromImpl<IBlob, SlangBlob>(shaders::SHADER_MAIN_SLANG_MODULE::blob);

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
        slangy::throwIfError(mainModule->findEntryPointByName("computeMain", computeEntryPoint.put()));

        std::cout << "shader_main computeMain entry point found" << std::endl;

        std::vector<slang::IComponentType*> componentTypes;
        componentTypes.push_back(mainModule.get());
        componentTypes.push_back(computeEntryPoint.get());
        componentTypes.push_back(libModule.get());

        ComPtr<slang::IComponentType> composedProgram;
        slangy::throwIfError(session->createCompositeComponentType(
            componentTypes.data(), componentTypes.size(), composedProgram.put(), DLog("Create composite component")()));

        // Link the composed program
        ComPtr<slang::IComponentType> linkedProgram;
        slangy::throwIfError(composedProgram->link(linkedProgram.put(), DLog("Link program")()));
        std::cout << "Composite program created and linked" << std::endl;

        // Execute the shader
        gfx::utils::DebugCallback debugCall;
        auto device = gfx::utils::makeDevice(::gfx::IDevice::Desc{}, &debugCall);
        auto commandQueue =
            gfx::utils::makeCommandQueue(device, ::gfx::ICommandQueue::Desc{::gfx::ICommandQueue::QueueType::Graphics});

        gfx::ITransientResourceHeap::Desc trDesc = {};
        trDesc.constantBufferSize = 4096;

        auto transientResource = gfx::utils::makeTransientResourceHeap(device, trDesc);

        ComPtr<gfx::ICommandBuffer> commandBuffer;
        slangy::throwIfError(transientResource->createCommandBuffer(commandBuffer.put()));

        /// Map buffer data to buffer resource
        std::array<int, 4> bufferData = {1, 2, 3, 4};
        gfx::IBufferResource::Desc bufferDesc = {};
        bufferDesc.sizeInBytes = bufferData.size() * sizeof(int);
        bufferDesc.format = gfx::Format::Unknown;
        bufferDesc.elementSize = sizeof(int);
        bufferDesc.defaultState = gfx::ResourceState::UnorderedAccess;
        bufferDesc.allowedStates = gfx::ResourceStateSet(gfx::ResourceState::UnorderedAccess);

        ComPtr<gfx::IBufferResource> bufferResource;
        slangy::throwIfError(
            device->createBufferResource(bufferDesc, reinterpret_cast<void*>(bufferData.data()), bufferResource.put()));

        auto pipelineState = makePipelineStateFromShader(device, linkedProgram);

        if (!pipelineState)
        {
            std::cout << "Failed to create pipeline state" << std::endl;
            return EXIT_FAILURE;
        }

        ComPtr<gfx::IComputeCommandEncoder> encoder;
        commandBuffer->encodeComputeCommands(encoder.put());

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
        slangy::throwIfError(device->createBufferView(bufferResource.get(), nullptr, bwDesc, bufferResourceView.put()));

        slangy::throwIfError(rootObject->setResource(gfx::ShaderOffset{0, 0, 0}, bufferResourceView.get()));

        slangy::throwIfError(encoder->dispatchCompute(4, 1, 1));
        encoder->endEncoding();
        commandBuffer->close();

        std::cout << "Executing compute shader" << std::endl;
        commandQueue->executeCommandBuffer(commandBuffer.get());
        commandQueue->waitOnHost();

        ComPtr<ISlangBlob> resultBlob;
        slangy::throwIfError(
            device->readBufferResource(bufferResource.get(), 0, bufferData.size() * sizeof(int), resultBlob.put()));

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
