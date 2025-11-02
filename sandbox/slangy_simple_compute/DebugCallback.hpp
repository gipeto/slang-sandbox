#pragma once

#include <slang-gfx.h>

#include <functional>
#include <iostream>

namespace gfx::utils
{

class DebugCallback : public ::gfx::IDebugCallback
{
public:
    DebugCallback()
          : DebugCallback([](::gfx::DebugMessageType, ::gfx::DebugMessageSource, const char* msg)
                          { std::cout << msg << std::endl; })
    {
    }

    DebugCallback(std::function<void(::gfx::DebugMessageType, ::gfx::DebugMessageSource, const char*)> callbackFun)
          : m_callbackFun(std::move(callbackFun))
    {
    }

    void SLANG_MCALL handleMessage(::gfx::DebugMessageType type, ::gfx::DebugMessageSource source,
                                   const char* msg) noexcept override
    {
        if (m_callbackFun)
        {
            m_callbackFun(type, source, msg);
        }
    }

private:
    std::function<void(::gfx::DebugMessageType, ::gfx::DebugMessageSource, const char*)> m_callbackFun;
};

}  // namespace slangy::gfx
