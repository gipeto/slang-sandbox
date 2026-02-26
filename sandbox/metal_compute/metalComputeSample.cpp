
#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

#include <stdexcept>
#include <type_traits>

#include <Foundation/NSSharedPtr.hpp>
#include <Foundation/NSString.hpp>
#include <Metal/MTLDevice.hpp>
#include <Metal/MTLResource.hpp>
#include <Metal/Metal.hpp>

#include <slangy/error_handling.hpp>
#include <slangy/slangy.hpp>

// Include the headers containing the precompiled shader modules
#include <shader_lib.h>
#include <shader_main.h>

int main(int argc, char* argv[])
{
    try
    {
        auto globalSession = slangy::valueOrThrow(slangy::makeGlobalSession());
        slang::SessionDesc sd{};
        slang::TargetDesc td{};
        td.format = SlangCompileTarget::SLANG_METAL_LIB;
        td.profile = globalSession->findProfile("");
        sd.targetCount = 1;
        sd.targets = &td;

        auto computeShader = slangy::valueOrThrow(
            slangy::buildShaderProgram<shaders::SHADER_LIB_SLANG_MODULE, shaders::SHADER_MAIN_SLANG_MODULE>(
                sd, globalSession, "computeMain"))[0];

        auto pDevice = NS::TransferPtr(MTL::CreateSystemDefaultDevice());

        if (!pDevice)
        {
            throw std::runtime_error("Failed to create default device");
        }

        dispatch_data_t dataDispatch =
            dispatch_data_create(computeShader.data(), computeShader.size(), dispatch_get_main_queue(), NULL);

        NS::Error* error;
        auto pLibrary = NS::TransferPtr(pDevice->newLibrary(dataDispatch, &error));

        if (!pLibrary)
        {
            throw std::runtime_error(std::string("Failed to create library from shader blob: ")
                                     + error->localizedDescription()->utf8String());
        }

        auto entryPoint = NS::String::string("computeMain", NS::UTF8StringEncoding);

        auto function = NS::TransferPtr(pLibrary->newFunction(entryPoint));

        if (!function)
        {
            throw std::runtime_error("Cannot get computeMain function from shader library");
        }

        auto pipelineState = NS::TransferPtr(pDevice->newComputePipelineState(function.get(), &error));
        if (!pipelineState)
        {
            throw std::runtime_error(std::string("Failed to create compute pipeline: ")
                                     + error->localizedDescription()->utf8String());
        }

        auto commandQueue = NS::TransferPtr(pDevice->newCommandQueue());

        std::array<int, 4> result = {1, 2, 3, 4};

        auto outputBuffer = NS::TransferPtr(
            pDevice->newBuffer(result.data(), result.size() * sizeof(int), MTL::ResourceStorageModeShared));

        auto commandBuffer = commandQueue->commandBuffer();
        auto encoder = commandBuffer->computeCommandEncoder();

        encoder->setComputePipelineState(pipelineState.get());
        encoder->setBuffer(outputBuffer.get(), 0, 0);

        // Determine threadgroup size – usually the max threads per group
        // supported by the GPU for this pipeline.
        MTL::Size threadGroupSize = MTL::Size(256, 1, 1);
        // Compute how many groups we need to cover 'count' elements.
        MTL::Size threadGroups = MTL::Size(1, 1, 1);

        encoder->dispatchThreadgroups(threadGroups, threadGroupSize);
        encoder->endEncoding();
        commandBuffer->commit();
        commandBuffer->waitUntilCompleted();

        auto contents = outputBuffer->contents();
        std::copy(static_cast<int*>(contents), static_cast<int*>(contents) + result.size(), result.begin());

        std::cout << "Compute shader results: ";
        for (size_t i = 0; i < 3; ++i)
            std::cout << result[i] << ",";
        std::cout << result[3] << std::endl;

        return EXIT_SUCCESS;
    }
    catch (const std::exception& e)
    {
        std::cout << "Failed with error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
