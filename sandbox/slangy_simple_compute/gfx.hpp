#pragma once

#include <slang-gfx.h>

#include <slangy/com/ComPtr.hpp>
#include <slangy/error_handling.hpp>

#include "DebugCallback.hpp"

namespace gfx::utils
{

slangy::ComPtr<::gfx::IDevice> makeDevice(const ::gfx::IDevice::Desc& deviceDesc = {},
                                          DebugCallback* debugCall = nullptr)
{
    if (debugCall)
    {
        ::gfx::gfxEnableDebugLayer(true);
        slangy::throwIfError(::gfx::gfxSetDebugCallback(debugCall));
    }

    slangy::ComPtr<::gfx::IDevice> gDevice;
    slangy::throwIfError(::gfx::gfxCreateDevice(&deviceDesc, gDevice.put()));

    return gDevice;
}

slangy::ComPtr<::gfx::ICommandQueue> makeCommandQueue(const slangy::ComPtr<::gfx::IDevice>& device,
                                                      const ::gfx::ICommandQueue::Desc& cmdQueueDesc = {
                                                          ::gfx::ICommandQueue::QueueType::Graphics})
{
    slangy::ComPtr<::gfx::ICommandQueue> cmdQueue;
    slangy::throwIfError(device->createCommandQueue(cmdQueueDesc, cmdQueue.put()));
    return cmdQueue;
}

slangy::ComPtr<::gfx::ITransientResourceHeap> makeTransientResourceHeap(
    const slangy::ComPtr<::gfx::IDevice>& device, const ::gfx::ITransientResourceHeap::Desc& transientHeapDesc)
{
    slangy::ComPtr<::gfx::ITransientResourceHeap> transientHeap;
    slangy::throwIfError(device->createTransientResourceHeap(transientHeapDesc, transientHeap.put()));

    return transientHeap;
}

}  // namespace gfx::utils
