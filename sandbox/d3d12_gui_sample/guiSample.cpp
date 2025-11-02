
#include "d3dPso.h"
#include "d3dwin.h"

#include <d3dcompiler.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <ranges>

#include <slangy/error_handling.hpp>
#include <slangy/slangy.hpp>

#include <pixel_shader.h>
#include <vertex_shader.h>

using namespace slangy;
using namespace DirectX;

float t{};
float step{0.02f};
std::array clearColor = {0.4f, 0.6f, 0.9f, 1.0f};

struct DescriptorHeap
{
    ComPtr<ID3D12DescriptorHeap> rtvDescriptorHeap;
    std::uint32_t descriptorSize{};
};

struct D3DResources
{
    static constexpr size_t BACK_BUFFER_CNT{3};
    size_t currentBufferIdx{};

    ComPtr<ID3D12Device> device;

    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    std::array<ComPtr<ID3D12CommandAllocator>, BACK_BUFFER_CNT> commandAllocators;

    ComPtr<IDXGISwapChain4> swapChain;

    DescriptorHeap rtvHeap;
    std::array<ComPtr<ID3D12Resource>, BACK_BUFFER_CNT> backBuffers;

    //  DescriptorHeap dsvHeap;
    //  ComPtr<ID3D12Resource> depthBuffer;

    ComPtr<ID3D12Fence> fence;
    std::array<std::uint64_t, BACK_BUFFER_CNT> frameFenceValues;
    std::uint64_t fenceValue{};
    HANDLE fenceEvent;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;

    // Data to render

    D3D12_VERTEX_BUFFER_VIEW vertexBufferView{};
    size_t faceCount{};
    D3D12_INDEX_BUFFER_VIEW indexBufferView{};
};

struct DisplayOptions
{
    bool vsync{true};
    bool tearingSupported{false};
    bool fullscreen{false};
    bool readyToDraw{false};
};

void enableDebugLayer()
{
    auto debugInterface = asComPtr<ID3D12Debug>(D3D12GetDebugInterface);

    if (!debugInterface)
    {
        throw std::runtime_error("Failed to enable debug layer");
    }
    debugInterface->EnableDebugLayer();
}

ComPtr<IDXGIFactory6> makeDxgiFactory(bool debug = false)
{
    std::uint32_t flags = debug ? DXGI_CREATE_FACTORY_DEBUG : 0;
    auto dxgiFactory = asComPtr<IDXGIFactory6>(CreateDXGIFactory2, flags);

    if (!dxgiFactory)
    {
        throw std::runtime_error("Failed to create DXGI factory: " + std::to_string(GetLastError()));
    }

    return dxgiFactory;
}

ComPtr<IDXGIAdapter4> getAdapter(bool debug = false)
{
    auto dxgiFactory = makeDxgiFactory(debug);

    ComPtr<IDXGIAdapter4> dxgiAdapter;

    std::uint32_t adapterIdx{};
    while (true)
    {
        if (auto result = dxgiFactory->EnumAdapterByGpuPreference(
                adapterIdx++, DXGI_GPU_PREFERENCE::DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter4),
                dxgiAdapter.put_void());
            result == DXGI_ERROR_NOT_FOUND || result != S_OK)
        {
            throw std::runtime_error("Failed find a compatible adapter");
        }

        DXGI_ADAPTER_DESC3 adapterDesc{};
        dxgiAdapter->GetDesc3(&adapterDesc);

        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG3::DXGI_ADAPTER_FLAG3_SOFTWARE)
        {
            std::runtime_error("Failed to find an hardware adapter");
        }

        if (D3D12CreateDevice(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr) >= 0)
        {
            return dxgiAdapter;
        }
    }

    return dxgiAdapter;
}

ComPtr<ID3D12Device> makeDevice(const ComPtr<IDXGIAdapter4>& adapter, bool debug = false)
{
    auto device = asComPtr<ID3D12Device>(D3D12CreateDevice, adapter.get(), D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_0);
    if (!device)
    {
        throw std::runtime_error("Faile to create device");
    }

    if (!debug)
    {
        return device;
    }

    if (auto pInfoQueue = device.as<ID3D12InfoQueue>())
    {
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

        // Suppress messages based on their severity level
        D3D12_MESSAGE_SEVERITY Severities[] = {D3D12_MESSAGE_SEVERITY_INFO};

        // Suppress individual messages by their ID
        D3D12_MESSAGE_ID DenyIds[] = {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,  // I'm really not sure how to avoid this
                                                                           // message.
            D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,    // This warning occurs when using capture frame while graphics
                                                       // debugging.
            D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,  // This warning occurs when using capture frame while graphics
                                                       // debugging.
        };

        D3D12_INFO_QUEUE_FILTER NewFilter = {};
        NewFilter.DenyList.NumSeverities = _countof(Severities);
        NewFilter.DenyList.pSeverityList = Severities;
        NewFilter.DenyList.NumIDs = _countof(DenyIds);
        NewFilter.DenyList.pIDList = DenyIds;

        if (S_OK != pInfoQueue->PushStorageFilter(&NewFilter))
        {
            throw std::runtime_error("Failed to setup debug info queue filter");
        }
    }

    return device;
}

ComPtr<ID3D12CommandQueue> makeCommandQueue(const ComPtr<ID3D12Device>& device, D3D12_COMMAND_LIST_TYPE type)
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = type;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    auto commandQueue = asComPtr<ID3D12CommandQueue, &ID3D12Device::CreateCommandQueue>(*device, &desc);

    if (!commandQueue)
    {
        throw std::runtime_error("Failed to create command queue");
    }

    return commandQueue;
}

bool tearingSupported()
{
    auto dxgiFactory = makeDxgiFactory();
    BOOL tearingSupported{};
    if (S_OK
        != dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearingSupported,
                                            sizeof(tearingSupported)))
    {
        return false;
    }

    return tearingSupported != 0;
}

ComPtr<IDXGISwapChain4> makeSwapChain(HWND hwnd, const ComPtr<ID3D12CommandQueue>& cmdQueue, std::uint32_t bufferCount,
                                      bool tearingSupported, bool debug = false)
{
    auto dxgiFactory = makeDxgiFactory(debug);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = bufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swapChain;
    if (S_OK != dxgiFactory->CreateSwapChainForHwnd(cmdQueue.get(), hwnd, &desc, nullptr, nullptr, swapChain.put()))
    {
        throw std::runtime_error("Failed to create swap chain");
    }

    if (S_OK != dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER))
    {
        throw std::runtime_error("Failed to disable fullscreen with alt+enter");
    }

    return swapChain.as<IDXGISwapChain4>();
}

DescriptorHeap makeDescriptorHeap(const ComPtr<ID3D12Device>& device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                  std::uint32_t descriptorsCount)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.NumDescriptors = descriptorsCount;
    desc.Type = type;
    if (auto dheap = asComPtr<ID3D12DescriptorHeap, &ID3D12Device::CreateDescriptorHeap>(*device, &desc))
    {
        return {std::move(dheap), device->GetDescriptorHandleIncrementSize(type)};
    }

    throw std::runtime_error("Failed to create descriptor heap");
}

ComPtr<ID3D12Resource> makeBufferResource(const ComPtr<ID3D12Device>& device, size_t byteSize, D3D12_HEAP_TYPE heapType,
                                          D3D12_RESOURCE_STATES resourceStates)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = heapType;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = byteSize;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.DepthOrArraySize = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return asComPtr<ID3D12Resource, &ID3D12Device::CreateCommittedResource>(
        *device, &heapProperties, D3D12_HEAP_FLAG_NONE, &desc, resourceStates, nullptr);
}

void updateRenderTargetViews(D3DResources& resources)
{
    auto rtvHandle = resources.rtvHeap.rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

    for (std::uint32_t i = 0; i < resources.BACK_BUFFER_CNT; ++i)
    {
        auto backBuffer = asComPtr<ID3D12Resource, &IDXGISwapChain4::GetBuffer>(*resources.swapChain, i);
        if (!backBuffer)
        {
            throw std::runtime_error("Failed to get back buffer");
        }

        resources.device->CreateRenderTargetView(backBuffer.get(), nullptr, rtvHandle);
        resources.backBuffers[i] = backBuffer;
        rtvHandle.ptr += size_t(resources.rtvHeap.descriptorSize);
    }
}

ComPtr<ID3D12CommandAllocator> makeCommandAllocator(const ComPtr<ID3D12Device>& device,
                                                    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT)
{
    auto cmdAllocator = asComPtr<ID3D12CommandAllocator, &ID3D12Device::CreateCommandAllocator>(*device, type);
    if (!cmdAllocator)
    {
        throw std::runtime_error("Failed to create command allocator");
    }

    return cmdAllocator;
}

ComPtr<ID3D12Fence> makeFence(const ComPtr<ID3D12Device>& device,
                              D3D12_FENCE_FLAGS flags = D3D12_FENCE_FLAGS::D3D12_FENCE_FLAG_NONE,
                              std::uint64_t initValue = 0)
{
    auto fence = asComPtr<ID3D12Fence, &ID3D12Device::CreateFence>(*device, initValue, flags);
    if (!fence)
    {
        throw std::runtime_error("Failed to create fece");
    }

    return fence;
}

std::uint64_t signalAndUpdate(ComPtr<ID3D12CommandQueue> cmdQueue, ComPtr<ID3D12Fence> fence, std::uint64_t& fenceValue)
{
    const auto val = ++fenceValue;

    if (S_OK != cmdQueue->Signal(fence.get(), val))
    {
        throw std::runtime_error("Faild to signal fence");
    }

    return val;
}

void waitForFence(ComPtr<ID3D12Fence> fence, std::uint64_t fenceValue, HANDLE fenceEvent,
                  std::chrono::milliseconds duration = std::chrono::milliseconds::max())
{
    if (fence->GetCompletedValue() < fenceValue)
    {
        if (S_OK != fence->SetEventOnCompletion(fenceValue, fenceEvent))
        {
            throw std::runtime_error("Failed to set fence event");
        }
        ::WaitForSingleObject(fenceEvent, static_cast<DWORD>(duration.count()));
    }
}

void flushQueue(ComPtr<ID3D12CommandQueue> cmdQueue, ComPtr<ID3D12Fence> fence, std::uint64_t& fenceValue,
                HANDLE fenceEvent)
{
    auto waitValue = signalAndUpdate(cmdQueue, fence, fenceValue);
    waitForFence(fence, waitValue, fenceEvent);
}

void render(const D3D12_VIEWPORT& viewPort, D3DResources& d3dRes, const DisplayOptions& options)
{
    D3D12_RECT scissorRect{0, 0, LONG_MAX, LONG_MAX};

    XMMATRIX viewProjection;
    {
        // setup view (camera) matrix
        const auto eyePosition = XMVectorSet(0, 0, -6, 1);
        const auto focusPoint = XMVectorSet(0, 0, 0, 1);
        const auto upDirection = XMVectorSet(0, 1, 0, 0);
        const auto view = XMMatrixLookAtLH(eyePosition, focusPoint, upDirection);
        // setup perspective projection matrix
        const auto aspectRatio = float(viewPort.Width) / float(viewPort.Height);

        const auto projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(65.f), aspectRatio, 0.1f, 100.0f);
        // combine matrices
        viewProjection = view * projection;
    }

    const auto mvp = XMMatrixTranspose(XMMatrixRotationX(1.0f * t + 1.f) * XMMatrixRotationY(1.2f * t + 2.f)
                                       * XMMatrixRotationZ(1.1f * t + 0.f) * viewProjection);

    auto& currentBufferIdx = d3dRes.currentBufferIdx;
    auto& cmdAllocator = d3dRes.commandAllocators[currentBufferIdx];

    cmdAllocator->Reset();
    d3dRes.commandList->Reset(cmdAllocator.get(), nullptr);

    auto& backBuffer = d3dRes.backBuffers[currentBufferIdx];

    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE::D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAGS::D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = backBuffer.get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

        d3dRes.commandList->ResourceBarrier(1, &barrier);

        auto rtvHandle = d3dRes.rtvHeap.rtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += size_t(d3dRes.rtvHeap.descriptorSize * currentBufferIdx);
        d3dRes.commandList->ClearRenderTargetView(rtvHandle, clearColor.data(), 0, nullptr);

        // d3dRescommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

        d3dRes.commandList->SetPipelineState(d3dRes.pipelineState.get());
        d3dRes.commandList->SetGraphicsRootSignature(d3dRes.rootSignature.get());
        d3dRes.commandList->SetGraphicsRoot32BitConstants(0, sizeof(mvp) / 4, &mvp, 0);

        d3dRes.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dRes.commandList->IASetVertexBuffers(0, 1, &d3dRes.vertexBufferView);
        d3dRes.commandList->IASetIndexBuffer(&d3dRes.indexBufferView);
        d3dRes.commandList->RSSetViewports(1, &viewPort);
        d3dRes.commandList->RSSetScissorRects(1, &scissorRect);
        d3dRes.commandList->OMSetRenderTargets(1, &rtvHandle, TRUE, nullptr);

        d3dRes.commandList->DrawIndexedInstanced(d3dRes.faceCount, 1, 0, 0, 0);
    }

    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE::D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAGS::D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = backBuffer.get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

        d3dRes.commandList->ResourceBarrier(1, &barrier);

        if (S_OK != d3dRes.commandList->Close())
        {
            throw std::runtime_error("Failed to close command list");
        }

        std::array<ID3D12CommandList* const, 1> commandLists = {d3dRes.commandList.get()};
        d3dRes.commandQueue->ExecuteCommandLists(commandLists.size(), commandLists.data());

        d3dRes.frameFenceValues[currentBufferIdx] =
            signalAndUpdate(d3dRes.commandQueue, d3dRes.fence, d3dRes.fenceValue);

        std::uint32_t syncInterval = options.vsync ? 1 : 0;
        std::uint32_t presentFlags = options.tearingSupported && !options.vsync ? DXGI_PRESENT_ALLOW_TEARING : 0;
        if (S_OK != d3dRes.swapChain->Present(syncInterval, presentFlags))
        {
            throw std::runtime_error("Failed to present frame");
        }

        currentBufferIdx = d3dRes.swapChain->GetCurrentBackBufferIndex();

        waitForFence(d3dRes.fence, d3dRes.fenceValue, d3dRes.fenceEvent);
    }

    t += step;
}

void resize(HWND hwnd, RECT& rect, D3DResources& d3dRes)
{
    RECT updatedRect{};
    ::GetClientRect(hwnd, &updatedRect);

    const auto currentWidth = static_cast<std::uint32_t>(rect.right - rect.left);
    const auto currentHeight = static_cast<std::uint32_t>(rect.bottom - rect.top);

    const auto newWidth = static_cast<std::uint32_t>(updatedRect.right - updatedRect.left);
    const auto newHeight = static_cast<std::uint32_t>(updatedRect.bottom - updatedRect.top);

    if (newWidth != currentWidth || newHeight != currentHeight)
    {
        rect = updatedRect;

        const auto clippedWidth = std::max(std::uint32_t(1), newWidth);
        const auto clippedHeight = std::max(std::uint32_t(1), newHeight);

        flushQueue(d3dRes.commandQueue, d3dRes.fence, d3dRes.fenceValue, d3dRes.fenceEvent);

        for (size_t i = 0; i < d3dRes.BACK_BUFFER_CNT; ++i)
        {
            d3dRes.backBuffers[i] = nullptr;
            d3dRes.frameFenceValues[i] = d3dRes.frameFenceValues[d3dRes.currentBufferIdx];
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        if (S_OK != d3dRes.swapChain->GetDesc(&desc))
        {
            throw std::runtime_error("Failed to get swap chain descriptor");
        }

        if (S_OK
            != d3dRes.swapChain->ResizeBuffers(d3dRes.BACK_BUFFER_CNT, clippedWidth, clippedHeight,
                                               desc.BufferDesc.Format, desc.Flags))
        {
            throw std::runtime_error("Failed to get swap chain descriptor");
        }

        d3dRes.currentBufferIdx = d3dRes.swapChain->GetCurrentBackBufferIndex();
        updateRenderTargetViews(d3dRes);
    }
}

struct WApp
{
    WNDCLASS wc{};
    HWND hwnd{};
    RECT winRect{};
    DisplayOptions options{};
    D3DResources d3dRes{};
};

int WINAPI wWinMain(HINSTANCE module, HINSTANCE, PWSTR pCmdLine, int nCmdShow)
{
    try
    {
        bool debug{};

        std::wstring cmdString(pCmdLine);
        if (cmdString == L"debug")
        {
            debug = true;
            enableDebugLayer();
            std::cout << "Debug layer enabled" << std::endl;
        }

        WApp app{};

        const auto className = L"guiSampleWinClass";

        app.wc.style = CS_HREDRAW | CS_VREDRAW;
        app.wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        app.wc.hInstance = module;
        app.wc.lpszClassName = className;
        app.wc.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT
        {
            if (WM_NCCREATE == message)
            {
                auto pcs = reinterpret_cast<CREATESTRUCT*>(lparam);
                auto pT = static_cast<WApp*>(pcs->lpCreateParams);
                pT->hwnd = hwnd;
                SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pcs->lpCreateParams));
                return TRUE;
            }

            auto pApp = reinterpret_cast<WApp*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

            if (pApp == nullptr || !pApp->options.readyToDraw)
            {
                return DefWindowProc(hwnd, message, wparam, lparam);
            }

            if (message == WM_DESTROY)
            {
                PostQuitMessage(0);
                return 0;
            }

            if (message == WM_SIZE)
            {
                resize(pApp->hwnd, pApp->winRect, pApp->d3dRes);
                return 0;
            }

            if (message == WM_PAINT)
            {
                D3D12_VIEWPORT viewPort{.TopLeftX = 0.f,
                                        .TopLeftY = 0.f,
                                        .Width = float(pApp->winRect.right - pApp->winRect.left),
                                        .Height = float(pApp->winRect.bottom - pApp->winRect.top),
                                        .MinDepth = 0.f,
                                        .MaxDepth = 1.f};
                render(viewPort, pApp->d3dRes, pApp->options);
                return 0;
            }

            return DefWindowProc(hwnd, message, wparam, lparam);
        };

        if (RegisterClass(&app.wc) < 0)
        {
            std::cout << "Failed to register window";
            return -1;
        }

        app.hwnd = CreateWindow(className, L"A d3d gui sample", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, module, &app);
        if (nullptr == app.hwnd)
        {
            auto errorCode = GetLastError();
            std::cout << "Failed to create window: " << errorCode << std::endl;
            return -1;
        }
        ::GetClientRect(app.hwnd, &app.winRect);

        DisplayOptions displayOptions{};
        displayOptions.tearingSupported = tearingSupported();

        app.d3dRes.device = makeDevice(getAdapter(debug), debug);
        app.d3dRes.commandQueue =
            makeCommandQueue(app.d3dRes.device, D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT);
        app.d3dRes.swapChain = makeSwapChain(app.hwnd, app.d3dRes.commandQueue, D3DResources::BACK_BUFFER_CNT,
                                             displayOptions.tearingSupported, debug);
        app.d3dRes.currentBufferIdx = app.d3dRes.swapChain->GetCurrentBackBufferIndex();
        app.d3dRes.rtvHeap = makeDescriptorHeap(
            app.d3dRes.device, D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_RTV, app.d3dRes.BACK_BUFFER_CNT);

        // app.d3dRes.dsvHeap =
        // makeDescriptorHeap(app.d3dRes.device,D3D12_DESCRIPTOR_HEAP_TYPE::D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);

        for (auto& cmdAllocator : app.d3dRes.commandAllocators)
        {
            cmdAllocator =
                makeCommandAllocator(app.d3dRes.device, D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT);
        }

        app.d3dRes.commandList = asComPtr<ID3D12GraphicsCommandList, &ID3D12Device::CreateCommandList>(
            *app.d3dRes.device, 0, D3D12_COMMAND_LIST_TYPE::D3D12_COMMAND_LIST_TYPE_DIRECT,
            app.d3dRes.commandAllocators[0].get(), nullptr);

        if (!app.d3dRes.commandList)
        {
            throw std::runtime_error("Failed to create command list");
        }
        if (S_OK != app.d3dRes.commandList->Close())
        {
            throw std::runtime_error("Failed to close command list");
        }

        app.d3dRes.fence = makeFence(app.d3dRes.device);

        // Setup shaders pipeline
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;

        {
            auto globalSession = slangy::valueOrThrow(makeGlobalSession());
            slang::SessionDesc sd{};
            slang::TargetDesc td{};
            td.format = SLANG_DXIL;
            sd.targetCount = 1;
            sd.targets = &td;
            td.profile = globalSession->findProfile("sm_6_3");

            auto asID3DBlob = [](const std::vector<std::byte>& shaderProgram)
            {
                ComPtr<ID3DBlob> result;
                if (S_OK != D3DCreateBlob(shaderProgram.size(), result.put()))
                {
                    throw std::runtime_error("Failed to create D3DBlob");
                }
                std::copy_n(reinterpret_cast<const char*>(shaderProgram.data()), shaderProgram.size(),
                            reinterpret_cast<char*>(result->GetBufferPointer()));
                return result;
            };

            vertexShader = asID3DBlob(slangy::valueOrThrow(
                buildShaderProgram(sd, globalSession, std::string("vertex_shader"), std::string("vertexMain"),
                                   shaders::VERTEX_SHADER_SLANG_MODULE))[0]);

            pixelShader = asID3DBlob(slangy::valueOrThrow(buildShaderProgram(
                sd, globalSession, "pixel_shader", "pixelMain", shaders::PIXEL_SHADER_SLANG_MODULE))[0]);
        }

        if (vertexShader)
        {
            std::cout << "Vertex shader built" << std::endl;
        }

        if (pixelShader)
        {
            std::cout << "Pixel shader built" << std::endl;
        }

        std::array inputLayout = {
            D3D12_INPUT_ELEMENT_DESC{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                                     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            D3D12_INPUT_ELEMENT_DESC{"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
                                     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

        D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                                                        | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                                                        | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                                                        | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        struct Vertex
        {
            XMFLOAT3 Position;
            XMFLOAT3 Color;
        };

        auto uploadBuffer = [](D3DResources& d3dRes, const ComPtr<ID3D12Resource>& sourceBuffer,
                               const ComPtr<ID3D12Resource>& destBuffer)
        {
            // Reuse the swap chain command list and allocator.
            auto& currentBufferIdx = d3dRes.currentBufferIdx;
            auto& cmdAllocator = d3dRes.commandAllocators[currentBufferIdx];

            cmdAllocator->Reset();
            d3dRes.commandList->Reset(cmdAllocator.get(), nullptr);

            {
                // The buffer fails to be initialized to D3D12_RESOURCE_STATE_COPY_DEST by CreateCommittedResource,
                // so we transition it here.
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE::D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAGS::D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = destBuffer.get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                d3dRes.commandList->ResourceBarrier(1, &barrier);
            }

            d3dRes.commandList->CopyResource(destBuffer.get(), sourceBuffer.get());

            auto& backBuffer = d3dRes.backBuffers[currentBufferIdx];

            {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE::D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAGS::D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = destBuffer.get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
                d3dRes.commandList->ResourceBarrier(1, &barrier);
            }

            if (S_OK != d3dRes.commandList->Close())
            {
                throw std::runtime_error("Failed to close command list");
            }

            std::array<ID3D12CommandList* const, 1> commandLists = {d3dRes.commandList.get()};
            d3dRes.commandQueue->ExecuteCommandLists(commandLists.size(), commandLists.data());

            d3dRes.frameFenceValues[currentBufferIdx] =
                signalAndUpdate(d3dRes.commandQueue, d3dRes.fence, d3dRes.fenceValue);

            currentBufferIdx = d3dRes.swapChain->GetCurrentBackBufferIndex();
            waitForFence(d3dRes.fence, d3dRes.fenceValue, d3dRes.fenceEvent);
        };

        ComPtr<ID3D12Resource> vertexBuffer;
        {
            constexpr std::array vertices = {Vertex{XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, 0.0f)},  // 0
                                             Vertex{XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f)},   // 1
                                             Vertex{XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(1.0f, 1.0f, 0.0f)},    // 2
                                             Vertex{XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f)},   // 3
                                             Vertex{XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f)},   // 4
                                             Vertex{XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 1.0f)},    // 5
                                             Vertex{XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(1.0f, 1.0f, 1.0f)},     // 6
                                             Vertex{XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 1.0f)}};   // 7

            vertexBuffer =
                makeBufferResource(app.d3dRes.device, sizeof(Vertex) * vertices.size(), D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATE_COMMON);  // Will be transitioned to
                                                                  // D3D12_RESOURCE_STATE_COPY_DEST by upload buffer

            if (!vertexBuffer)
            {
                throw std::runtime_error("Failed to create vertex buffer");
            }

            auto vertexUploadBuffer = makeBufferResource(app.d3dRes.device, sizeof(Vertex) * vertices.size(),
                                                         D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!vertexUploadBuffer)
            {
                throw std::runtime_error("Failed to create vertex upload buffer");
            }

            Vertex* mappedVertexData = nullptr;
            vertexUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedVertexData));
            std::ranges::copy(vertices, mappedVertexData);
            vertexUploadBuffer->Unmap(0, nullptr);

            uploadBuffer(app.d3dRes, vertexUploadBuffer, vertexBuffer);
            app.d3dRes.vertexBufferView = {.BufferLocation = vertexBuffer->GetGPUVirtualAddress(),
                                           .SizeInBytes = vertices.size() * sizeof(Vertex),
                                           .StrideInBytes = sizeof(Vertex)};
        }

        ComPtr<ID3D12Resource> indexBuffer;
        {
            constexpr std::array indexData = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 4, 5, 1, 4, 1, 0,
                                              3, 2, 6, 3, 6, 7, 1, 5, 6, 1, 6, 2, 4, 0, 3, 4, 3, 7};

            indexBuffer =
                makeBufferResource(app.d3dRes.device, sizeof(int) * indexData.size(), D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_STATE_COMMON);  // Will be transitioned to
                                                                  // D3D12_RESOURCE_STATE_COPY_DEST by upload buffer

            if (!indexBuffer)
            {
                throw std::runtime_error("Failed to create vertex buffer");
            }

            auto indexUploadBuffer = makeBufferResource(app.d3dRes.device, sizeof(int) * indexData.size(),
                                                        D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!indexUploadBuffer)
            {
                throw std::runtime_error("Failed to create vertex upload buffer");
            }

            int* mappedIndexData = nullptr;
            indexUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mappedIndexData));
            std::ranges::copy(indexData, mappedIndexData);
            indexUploadBuffer->Unmap(0, nullptr);

            uploadBuffer(app.d3dRes, indexUploadBuffer, indexBuffer);
            app.d3dRes.indexBufferView = {.BufferLocation = indexBuffer->GetGPUVirtualAddress(),
                                          .SizeInBytes = indexData.size() * sizeof(int),
                                          .Format = DXGI_FORMAT_R32_UINT};
            app.d3dRes.faceCount = indexData.size();
        }

        // root signature
        {
            std::array<D3D12_ROOT_PARAMETER1, 1> rootParameters;
            rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
            rootParameters[0].Constants =
                D3D12_ROOT_CONSTANTS{.ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = 16};

            D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDescription{};
            rootSignatureDescription.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            rootSignatureDescription.Desc_1_1 = D3D12_ROOT_SIGNATURE_DESC1{rootParameters.size(), rootParameters.data(),
                                                                           0, nullptr, rootSignatureFlags};

            ComPtr<ID3DBlob> rootSignatureBlob;
            ComPtr<ID3DBlob> diagnosticBlob;

            if (S_OK
                != D3D12SerializeVersionedRootSignature(&rootSignatureDescription, rootSignatureBlob.put(),
                                                        diagnosticBlob.put()))
            {
                std::string diagnoticMsg{};
                if (diagnosticBlob)
                {
                    std::string diagnoticMsg(
                        reinterpret_cast<char*>(diagnosticBlob->GetBufferPointer()),
                        reinterpret_cast<char*>(diagnosticBlob->GetBufferPointer()) + diagnosticBlob->GetBufferSize());
                    std::cout << "Failed to serialize root signature: " << diagnoticMsg << std::endl;
                }

                throw std::runtime_error("Failed to serialize root signature: " + diagnoticMsg);
            }

            app.d3dRes.rootSignature = asComPtr<ID3D12RootSignature, &ID3D12Device::CreateRootSignature>(
                *app.d3dRes.device, 0, rootSignatureBlob->GetBufferPointer(), rootSignatureBlob->GetBufferSize());

            if (!app.d3dRes.rootSignature)
            {
                throw std::runtime_error("Failed to create root signature");
            }
        }

        {
            struct PipelineStateStream
            {
                d3d::psoRootSignature rootSignature;
                d3d::psoInputLayout inputLayout;
                d3d::psoPrimitiveTopology topology;
                d3d::psoVS vertexShader;
                d3d::psoPS pixelShader;
                // d3d::psoDepthStencilFormat dsvFormat;
                d3d::psoRenderTargetFormats rtvFormats;
            } pipelineStateStream{};

            pipelineStateStream.rootSignature.value = app.d3dRes.rootSignature.get();
            pipelineStateStream.inputLayout.value = {.pInputElementDescs = inputLayout.data(),
                                                     .NumElements = inputLayout.size()};
            pipelineStateStream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipelineStateStream.vertexShader.value.pShaderBytecode = vertexShader->GetBufferPointer();
            pipelineStateStream.vertexShader.value.BytecodeLength = vertexShader->GetBufferSize();
            pipelineStateStream.pixelShader.value.pShaderBytecode = pixelShader->GetBufferPointer();
            pipelineStateStream.pixelShader.value.BytecodeLength = pixelShader->GetBufferSize();
            // pipelineStateStream.dsvFormat.value = DXGI_FORMAT_D32_FLOAT;
            DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
            if (S_OK != app.d3dRes.swapChain->GetDesc1(&swapChainDesc))
            {
                throw std::runtime_error("Failed to get swapChainDesc");
            }
            pipelineStateStream.rtvFormats.value = {.RTFormats{swapChainDesc.Format}, .NumRenderTargets = 1};

            D3D12_PIPELINE_STATE_STREAM_DESC pssDesc{sizeof(pipelineStateStream), &pipelineStateStream};
            app.d3dRes.pipelineState = asComPtr<ID3D12PipelineState, &ID3D12Device2::CreatePipelineState>(
                *app.d3dRes.device.as<ID3D12Device2>(), &pssDesc);

            if (!app.d3dRes.pipelineState)
            {
                throw std::runtime_error("Failed to create pipeline");
            }
        }

        updateRenderTargetViews(app.d3dRes);

        app.options.readyToDraw = true;
        ShowWindow(app.hwnd, nCmdShow);

        MSG msg{};
        while (msg.message != WM_QUIT)
        {
            if (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
            }
        }

        flushQueue(app.d3dRes.commandQueue, app.d3dRes.fence, app.d3dRes.fenceValue, app.d3dRes.fenceEvent);
        ::CloseHandle(app.d3dRes.fenceEvent);

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cout << e.what() << std::endl;
        return -1;
    }
    catch (...)
    {
        std::cout << "Failed with an unknown error" << std::endl;
        return -1;
    }
}
