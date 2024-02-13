// Copyright 2020-2023 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <string.h>
#include <assert.h>

#include <d3d12.h>
#include <d3dx12.h>
#include <D3DCompiler.h>

#include <dxgi1_6.h>

#include <dmsdk/dlib/vmath.h>

#include <dlib/array.h>
#include <dlib/dstrings.h>
#include <dlib/log.h>
#include <dlib/math.h>
#include <dlib/profile.h>

#include <platform/platform_window.h>

#include <graphics/glfw/glfw_native.h>

#include "../graphics_private.h"
#include "../graphics_native.h"
#include "../graphics_adapter.h"

#include "graphics_dx12_private.h"


namespace dmGraphics
{
    static GraphicsAdapterFunctionTable DX12RegisterFunctionTable();
    static bool                         DX12IsSupported();
    static bool                         DX12Initialize(HContext _context);
    static const int8_t    g_dx12_adapter_priority = 0;
    static GraphicsAdapter g_dx12_adapter(ADAPTER_FAMILY_DIRECTX);
    static DX12Context*    g_DX12Context = 0x0;

    DM_REGISTER_GRAPHICS_ADAPTER(GraphicsAdapterDX12, &g_dx12_adapter, DX12IsSupported, DX12RegisterFunctionTable, g_dx12_adapter_priority);

    #define CHECK_HR_ERROR(result) \
    { \
        if(g_DX12Context->m_VerifyGraphicsCalls && result != S_OK) { \
            dmLogError("DX Error (%s:%d) code: %d", __FILE__, __LINE__, HRESULT_CODE(result)); \
            assert(0); \
        } \
    }

    DX12Context::DX12Context(const ContextParams& params)
    {
        memset(this, 0, sizeof(*this));
        m_NumFramesInFlight       = MAX_FRAMES_IN_FLIGHT;
        m_DefaultTextureMinFilter = params.m_DefaultTextureMinFilter;
        m_DefaultTextureMagFilter = params.m_DefaultTextureMagFilter;
        m_VerifyGraphicsCalls     = params.m_VerifyGraphicsCalls;
        m_PrintDeviceInfo         = params.m_PrintDeviceInfo;
        m_Window                  = params.m_Window;
        m_Width                   = params.m_Width;
        m_Height                  = params.m_Height;
        m_UseValidationLayers     = params.m_UseValidationLayers;

        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_LUMINANCE;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_LUMINANCE_ALPHA;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGB;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGBA;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGB_16BPP;
        m_TextureFormatSupport |= 1 << TEXTURE_FORMAT_RGBA_16BPP;

        assert(dmPlatform::GetWindowStateParam(m_Window, dmPlatform::WINDOW_STATE_OPENED));
    }

    static HContext DX12NewContext(const ContextParams& params)
    {
        if (!g_DX12Context)
        {
            g_DX12Context = new DX12Context(params);

            if (DX12Initialize(g_DX12Context))
            {
                return g_DX12Context;
            }

            DeleteContext(g_DX12Context);
        }
        return 0x0;
    }

    static IDXGIAdapter1* CreateDeviceAdapter(IDXGIFactory4* dxgiFactory)
    {
        IDXGIAdapter1* adapter = 0;
        int adapterIndex = 0;

        // find first hardware gpu that supports d3d 12
        while (dxgiFactory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                adapterIndex++;
                continue;
            }

            // we want a device that is compatible with direct3d 12 (feature level 11 or higher)
            HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), NULL);
            if (SUCCEEDED(hr))
            {
                break;
            }

            adapterIndex++;
        }

        return adapter;
    }

    static IDXGIFactory4* CreateDXGIFactory()
    {
        IDXGIFactory4* factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr))
        {
            return 0;
        }
        return factory;
    }

    static bool DX12IsSupported()
    {
        IDXGIAdapter1* adapter = CreateDeviceAdapter(CreateDXGIFactory());
        if (adapter)
        {
            adapter->Release();
            return true;
        }
        return false;
    }

    static void DX12DeleteContext(HContext _context)
    {
        assert(_context);
        if (g_DX12Context)
        {
            DX12Context* context = (DX12Context*) _context;

            // SAFE_RELEASE(pipelineStateObject);
            // SAFE_RELEASE(rootSignature);
            // SAFE_RELEASE(vertexBuffer);

            delete (DX12Context*) context;
            g_DX12Context = 0x0;
        }
    }

    static void CreateRootSignature(DX12Context* context, CD3DX12_ROOT_SIGNATURE_DESC* desc, DX12ShaderProgram* program)
    {
        ID3DBlob* signature;
        HRESULT hr = D3D12SerializeRootSignature(desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, NULL);
        CHECK_HR_ERROR(hr);

        hr = context->m_Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&program->m_RootSignature));
        CHECK_HR_ERROR(hr);
    }

    static void SetupMainRenderTarget(DX12Context* context, DXGI_SAMPLE_DESC sample_desc)
    {
        // Initialize the dummy rendertarget for the main framebuffer
        // The m_Framebuffer construct will be rotated sequentially
        // with the framebuffer objects created per swap chain.
        DX12RenderTarget* rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_MainRenderTarget);
        assert(rt == 0x0);

        rt               = new DX12RenderTarget();
        rt->m_Id         = DM_RENDERTARGET_BACKBUFFER_ID;
        rt->m_Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
        rt->m_SampleDesc = sample_desc;

        context->m_MainRenderTarget    = StoreAssetInContainer(context->m_AssetHandleContainer, rt, ASSET_TYPE_RENDER_TARGET);
        context->m_CurrentRenderTarget = context->m_MainRenderTarget;

        // rt->m_Handle.m_RenderPass  = context->m_MainRenderPass;
        // rt->m_Handle.m_Framebuffer = context->m_MainFrameBuffers[0];
        // rt->m_Extent               = context->m_SwapChain->m_ImageExtent;
        // rt->m_ColorAttachmentCount = 1;
    }

    void DX12ScratchBuffer::Initialize(DX12Context* context, uint32_t frame_index)
    {
        uint32_t pool_block_count = MAX_BLOCK_SIZE / BLOCK_STEP_SIZE;
        m_MemoryPools.SetCapacity(pool_block_count);
        m_MemoryPools.SetSize(pool_block_count);

        m_FrameIndex = frame_index;

        for (int i = 0; i < m_MemoryPools.Size(); ++i)
        {
            m_MemoryPools[i].m_BlockSize        = (i+1) * BLOCK_STEP_SIZE;
            m_MemoryPools[i].m_DescriptorCursor = 0;
            m_MemoryPools[i].m_MemoryCursor     = 0;

            D3D12_DESCRIPTOR_HEAP_DESC heap_Desc = {};
            heap_Desc.NumDescriptors             = DESCRIPTORS_PER_POOL;
            heap_Desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            heap_Desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

            HRESULT hr = context->m_Device->CreateDescriptorHeap(&heap_Desc, IID_PPV_ARGS(&m_MemoryPools[i].m_DescriptorHeap));
            CHECK_HR_ERROR(hr);

            const uint32_t memory_heap_alignment = 1024 * 64;
            const uint32_t memory_heap_size      = memory_heap_alignment; // TODO: Some other memory metric here

            hr = context->m_Device->CreateCommittedResource(
                &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // this heap will be used to upload the constant buffer data
                D3D12_HEAP_FLAG_NONE,                             // no flags
                &CD3DX12_RESOURCE_DESC::Buffer(memory_heap_size), // size of the resource heap. Must be a multiple of 64KB for single-textures and constant buffers
                D3D12_RESOURCE_STATE_GENERIC_READ,                // will be data that is read from so we keep it in the generic read state
                NULL,                                             // we do not have use an optimized clear value for constant buffers
                IID_PPV_ARGS(&m_MemoryPools[i].m_MemoryHeap));
            CHECK_HR_ERROR(hr);

            for (int j = 0; j < DESCRIPTORS_PER_POOL; ++j)
            {
                D3D12_CONSTANT_BUFFER_VIEW_DESC view_desc = {};
                view_desc.BufferLocation = m_MemoryPools[i].m_MemoryHeap->GetGPUVirtualAddress() + i * m_MemoryPools[i].m_BlockSize;
                view_desc.SizeInBytes    = m_MemoryPools[i].m_BlockSize;

                CD3DX12_CPU_DESCRIPTOR_HANDLE view_handle(m_MemoryPools[i].m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart(), i, context->m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
                context->m_Device->CreateConstantBufferView(&view_desc, view_handle);
            }

            hr = m_MemoryPools[i].m_MemoryHeap->Map(0, 0, &m_MemoryPools[i].m_MappedDataPtr);
            CHECK_HR_ERROR(hr);

            // m_MemoryPools[i].m_DescriptorCursor++;
        }
    }

    void* DX12ScratchBuffer::AllocateConstantBuffer(DX12Context* context, uint32_t non_aligned_byte_size)
    {
        assert(non_aligned_byte_size < MAX_BLOCK_SIZE);
        uint32_t pool_index = non_aligned_byte_size / BLOCK_STEP_SIZE;
        uint32_t cursor = m_MemoryPools[pool_index].m_MemoryCursor;

        uint8_t* base_ptr = ((uint8_t*) m_MemoryPools[pool_index].m_MappedDataPtr) + cursor;
        m_MemoryPools[pool_index].m_MemoryCursor += BLOCK_STEP_SIZE;

        // dmLogInfo("AllocateConstantBuffer: ptr: %p, frame: %d, pool: %d, descriptor: %d, offset: %d", base_ptr, m_FrameIndex, pool_index, m_MemoryPools[pool_index].m_DescriptorCursor, cursor);

        return (void*) base_ptr;
    }

    void DX12ScratchBuffer::Reset(DX12Context* context)
    {
        for (int i = 0; i < m_MemoryPools.Size(); ++i)
        {
            m_MemoryPools[i].m_DescriptorCursor = 0;
            m_MemoryPools[i].m_MemoryCursor     = 0;
        }
    }

    void DX12ScratchBuffer::Bind(DX12Context* context)
    {
        // TODO: multiple heaps needs to be bound here
        ID3D12DescriptorHeap* heaps[] = { m_MemoryPools[0].m_DescriptorHeap };
        context->m_CommandList->SetDescriptorHeaps(1, heaps);

        /*
        void SetGraphicsRootConstantBufferView(
          [in] UINT                      RootParameterIndex,
          [in] D3D12_GPU_VIRTUAL_ADDRESS BufferLocation
        );
        */

        // set the root descriptor table 0 to the constant buffer descriptor heap
        context->m_CommandList->SetGraphicsRootConstantBufferView(0, m_MemoryPools[0].m_MemoryHeap->GetGPUVirtualAddress());
        context->m_CommandList->SetGraphicsRootConstantBufferView(1, m_MemoryPools[0].m_MemoryHeap->GetGPUVirtualAddress() + 256);
    }

    static bool DX12Initialize(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;

        HRESULT hr = S_OK;

        // This needs to be created before the device
        // if (context->m_UseValidationLayers)
        if (true)
        {
            hr = D3D12GetDebugInterface(IID_PPV_ARGS(&context->m_DebugInterface));
            CHECK_HR_ERROR(hr);

            context->m_DebugInterface->EnableDebugLayer(); // TODO: Release
        }

        IDXGIFactory4* factory = CreateDXGIFactory();
        IDXGIAdapter1* adapter = CreateDeviceAdapter(factory);

        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&context->m_Device));
        CHECK_HR_ERROR(hr);

        D3D12_COMMAND_QUEUE_DESC cmd_queue_desc = {};
        hr = context->m_Device->CreateCommandQueue(&cmd_queue_desc, IID_PPV_ARGS(&context->m_CommandQueue));
        CHECK_HR_ERROR(hr);

        // Create swapchain
        DXGI_MODE_DESC back_buffer_desc = {};
        back_buffer_desc.Width          = context->m_Width;
        back_buffer_desc.Height         = context->m_Height;
        back_buffer_desc.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;

        DXGI_SAMPLE_DESC sample_desc = {};
        sample_desc.Count            = 1;

        DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
        swap_chain_desc.BufferCount          = MAX_FRAMEBUFFERS;
        swap_chain_desc.BufferDesc           = back_buffer_desc;
        swap_chain_desc.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc.SwapEffect           = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_chain_desc.OutputWindow         = glfwGetWindowsHWND();
        swap_chain_desc.SampleDesc           = sample_desc;
        swap_chain_desc.Windowed             = true;

        IDXGISwapChain* swap_chain_tmp = 0;
        factory->CreateSwapChain(context->m_CommandQueue, &swap_chain_desc, &swap_chain_tmp);
        context->m_SwapChain = static_cast<IDXGISwapChain3*>(swap_chain_tmp);

        // frameIndex = swapChain->GetCurrentBackBufferIndex();

        // this heap is a render target view heap
        D3D12_DESCRIPTOR_HEAP_DESC rt_view_heap_desc = {};
        rt_view_heap_desc.NumDescriptors             = MAX_FRAMEBUFFERS;
        rt_view_heap_desc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rt_view_heap_desc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        hr = context->m_Device->CreateDescriptorHeap(&rt_view_heap_desc, IID_PPV_ARGS(&context->m_RtvDescriptorHeap));
        CHECK_HR_ERROR(hr);

        context->m_RtvDescriptorSize = context->m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // get a handle to the first descriptor in the descriptor heap. a handle is basically a pointer,
        // but we cannot literally use it like a c++ pointer.
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_handle(context->m_RtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

        for (int i = 0; i < MAX_FRAMEBUFFERS; i++)
        {
            // first we get the n'th buffer in the swap chain and store it in the n'th
            // position of our ID3D12Resource array
            hr = context->m_SwapChain->GetBuffer(i, IID_PPV_ARGS(&context->m_FrameResources[i].m_RenderTarget.m_Resource));
            CHECK_HR_ERROR(hr);

            // the we "create" a render target view which binds the swap chain buffer (ID3D12Resource[n]) to the rtv handle
            context->m_Device->CreateRenderTargetView(context->m_FrameResources[i].m_RenderTarget.m_Resource, NULL, rtv_handle);

            // we increment the rtv handle by the rtv descriptor size we got above
            rtv_handle.Offset(1, context->m_RtvDescriptorSize);

            hr = context->m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&context->m_FrameResources[i].m_CommandAllocator));
            CHECK_HR_ERROR(hr);

            // Create the frame fence that will be signaled when we can render to this frame
            hr = context->m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&context->m_FrameResources[i].m_Fence));
            CHECK_HR_ERROR(hr);

            context->m_FrameResources[i].m_FenceValue = RENDER_CONTEXT_STATE_FREE;
            context->m_FrameResources[i].m_ScratchBuffer.Initialize(context, i);
        }


        context->m_FenceEvent = CreateEvent(NULL, false, false, NULL);
        if (!context->m_FenceEvent)
        {
            dmLogFatal("Unable to create fence event");
            return false;
        }

        // command buffer / command list
        // TODO: We should create one of these for every thread we have that are recording commands
        hr = context->m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context->m_FrameResources[0].m_CommandAllocator, NULL, IID_PPV_ARGS(&context->m_CommandList));
        CHECK_HR_ERROR(hr);

        context->m_CommandList->Close();

        SetupMainRenderTarget(context, sample_desc);

        context->m_PipelineState = GetDefaultPipelineState();

        if (context->m_PrintDeviceInfo)
        {
            dmLogInfo("Device: DirectX 12");
        }
        return true;
    }

    static void DX12Finalize()
    {

    }

    static void DX12CloseWindow(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;

        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
        }
    }

    static void DX12RunApplicationLoop(void* user_data, WindowStepMethod step_method, WindowIsRunning is_running)
    {
    }

    static dmPlatform::HWindow DX12GetWindow(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        return context->m_Window;
    }

    static uint32_t DX12GetDisplayDpi(HContext context)
    {
        assert(context);
        return 0;
    }

    static uint32_t DX12GetWidth(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        return context->m_Width;
    }

    static uint32_t DX12GetHeight(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        return context->m_Height;
    }

    /*
    void VulkanSetWindowSize(HContext _context, uint32_t width, uint32_t height)
    {
        VulkanContext* context = (VulkanContext*) _context;

        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
            context->m_Width  = width;
            context->m_Height = height;

            dmPlatform::SetWindowSize(context->m_Window, width, height);

            context->m_WindowWidth  = dmPlatform::GetWindowWidth(context->m_Window);
            context->m_WindowHeight = dmPlatform::GetWindowHeight(context->m_Window);

            SwapChainChanged(g_VulkanContext, &context->m_WindowWidth, &context->m_WindowHeight, 0, 0);
        }
    }

    /*
    HRESULT ResizeBuffers(
  UINT        BufferCount,
  UINT        Width,
  UINT        Height,
  DXGI_FORMAT NewFormat,
  UINT        SwapChainFlags
);
*/
    /*
    void VulkanResizeWindow(HContext _context, uint32_t width, uint32_t height)
    {
        VulkanContext* context = (VulkanContext*) _context;
        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
            VulkanSetWindowSize(_context, width, height);
        }
    }
    */

    static void DX12SetWindowSize(HContext _context, uint32_t width, uint32_t height)
    {
        assert(_context);
        DX12Context* context = (DX12Context*) _context;
        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
            dmPlatform::SetWindowSize(context->m_Window, width, height);
        }
    }

    static void DX12ResizeWindow(HContext _context, uint32_t width, uint32_t height)
    {
        assert(_context);
        DX12Context* context = (DX12Context*) _context;
        if (dmPlatform::GetWindowStateParam(context->m_Window, dmPlatform::WINDOW_STATE_OPENED))
        {
            dmPlatform::SetWindowSize(context->m_Window, width, height);
        }
    }

    static void DX12GetDefaultTextureFilters(HContext _context, TextureFilter& out_min_filter, TextureFilter& out_mag_filter)
    {
        DX12Context* context = (DX12Context*) _context;
        out_min_filter = context->m_DefaultTextureMinFilter;
        out_mag_filter = context->m_DefaultTextureMagFilter;
    }

    static void DX12Clear(HContext _context, uint32_t flags, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha, float depth, uint32_t stencil)
    {
        DX12Context* context = (DX12Context*) _context;

        const float r = ((float)red)/255.0f;
        const float g = ((float)green)/255.0f;
        const float b = ((float)blue)/255.0f;
        const float a = ((float)alpha)/255.0f;
        const float cc[] = { r, g, b, a };
        context->m_CommandList->ClearRenderTargetView(context->m_RtvHandle, cc, 0, NULL);
    }

    static void SyncronizeFrame(DX12Context* context)
    {
        // swap the current rtv buffer index so we draw on the correct buffer
        context->m_CurrentFrameIndex = context->m_SwapChain->GetCurrentBackBufferIndex();

        DX12FrameResource& current_frame_resource = context->m_FrameResources[context->m_CurrentFrameIndex];

        // if the current fence value is still less than "fenceValue", then we know the GPU has not finished executing
        // the command queue since it has not reached the "commandQueue->Signal(fence, fenceValue)" command

        HRESULT hr = S_OK;

        if (current_frame_resource.m_Fence->GetCompletedValue() < current_frame_resource.m_FenceValue)
        {
            // we have the fence create an event which is signaled once the fence's current value is "fenceValue"
            hr = current_frame_resource.m_Fence->SetEventOnCompletion(current_frame_resource.m_FenceValue, context->m_FenceEvent);
            CHECK_HR_ERROR(hr);

            // We will wait until the fence has triggered the event that it's current value has reached "fenceValue". once it's value
            // has reached "fenceValue", we know the command queue has finished executing
            WaitForSingleObject(context->m_FenceEvent, INFINITE);
        }

        // increment fenceValue for next frame
        current_frame_resource.m_FenceValue++;
    }

    static bool EndRenderPass(DX12Context* context)
    {
        DX12RenderTarget* current_rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_CurrentRenderTarget);

        if (!current_rt->m_IsBound)
        {
            return false;
        }

        if (current_rt->m_Id == DM_RENDERTARGET_BACKBUFFER_ID)
        {
            context->m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(current_rt->m_Resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        }

        current_rt->m_IsBound = 0;
        return true;
    }

    static void BeginRenderPass(DX12Context* context, HRenderTarget render_target)
    {
        DX12RenderTarget* current_rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_CurrentRenderTarget);
        DX12RenderTarget* rt         = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, render_target);

        if (current_rt->m_Id == rt->m_Id &&
            current_rt->m_IsBound)
        {
            return;
        }

        if (current_rt->m_IsBound)
        {
            EndRenderPass(context);
        }

        if (current_rt->m_Id == DM_RENDERTARGET_BACKBUFFER_ID)
        {
            context->m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(current_rt->m_Resource, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
        }

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(context->m_RtvDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), context->m_CurrentFrameIndex, context->m_RtvDescriptorSize);
        context->m_RtvHandle = rtvHandle;
        context->m_CommandList->OMSetRenderTargets(1, &context->m_RtvHandle, false, NULL);

        rt->m_IsBound = 1;

        context->m_CurrentRenderTarget = render_target;
    }

    static void DX12BeginFrame(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        SyncronizeFrame(context);

        DX12FrameResource& current_frame_resource = context->m_FrameResources[context->m_CurrentFrameIndex];

        HRESULT hr = current_frame_resource.m_CommandAllocator->Reset();
        CHECK_HR_ERROR(hr);

        DX12RenderTarget* rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_MainRenderTarget);
        rt->m_Resource = current_frame_resource.m_RenderTarget.m_Resource;

        // Enter "record" mode
        hr = context->m_CommandList->Reset(current_frame_resource.m_CommandAllocator, NULL); // Second argument is a pipeline object (TODO)
        CHECK_HR_ERROR(hr);

        current_frame_resource.m_ScratchBuffer.Reset(context);

        context->m_FrameBegun = 1;

        BeginRenderPass(context, context->m_MainRenderTarget);
    }

    static void DX12Flip(HContext _context)
    {
        DX12Context* context = (DX12Context*) _context;
        EndRenderPass(context);

        DX12FrameResource& current_frame_resource = context->m_FrameResources[context->m_CurrentFrameIndex];

        // Close the command list for recording
        HRESULT hr = context->m_CommandList->Close();

        // create an array of command lists (only one command list here)
        ID3D12CommandList* execute_cmd_lists[] = { context->m_CommandList };

        // execute the array of command lists
        context->m_CommandQueue->ExecuteCommandLists(DM_ARRAY_SIZE(execute_cmd_lists), execute_cmd_lists);

        hr = context->m_CommandQueue->Signal(current_frame_resource.m_Fence, current_frame_resource.m_FenceValue);
        CHECK_HR_ERROR(hr);

        hr = context->m_SwapChain->Present(0, 0);
        CHECK_HR_ERROR(hr);

        context->m_FrameBegun = 0;
    }

    static void DeviceBufferUploadHelper(DX12Context* context, DX12DeviceBuffer* device_buffer, const void* data, uint32_t data_size)
    {
        if (data == 0 || data_size == 0)
            return;

        // create upload heap
        // upload heaps are used to upload data to the GPU. CPU can write to it, GPU can read from it
        // We will upload the vertex buffer using this heap to the default heap
        ID3D12Resource* upload_heap;
        HRESULT hr = context->m_Device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),   // upload heap
            D3D12_HEAP_FLAG_NONE,                               // no flags
            &CD3DX12_RESOURCE_DESC::Buffer(data_size),               // resource description for a buffer
            D3D12_RESOURCE_STATE_GENERIC_READ,                  // GPU will read from this buffer and copy its contents to the default heap
            NULL,
            IID_PPV_ARGS(&upload_heap));
        CHECK_HR_ERROR(hr);

        upload_heap->SetName(L"Vertex Buffer Upload Resource Heap");

        // store vertex buffer in upload heap
        D3D12_SUBRESOURCE_DATA vx_data = {};
        vx_data.pData      = data; // pointer to our vertex array
        vx_data.RowPitch   = data_size; // size of all our vertex data
        vx_data.SlicePitch = data_size; // also the size of our vertex data

        if (!context->m_FrameBegun)
        {
            hr = context->m_CommandList->Reset(context->m_FrameResources[0].m_CommandAllocator, NULL); // Second argument is a pipeline object (TODO)
            CHECK_HR_ERROR(hr);
        }

        UpdateSubresources(context->m_CommandList, device_buffer->m_Resource, upload_heap, 0, 0, 1, &vx_data);

        // transition the vertex buffer data from copy destination state to vertex buffer state
        context->m_CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(device_buffer->m_Resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

        if (!context->m_FrameBegun)
        {
            context->m_CommandList->Close(); // THis might be wrong!
            ID3D12CommandList* execute_cmd_lists[] = { context->m_CommandList };
            context->m_CommandQueue->ExecuteCommandLists(DM_ARRAY_SIZE(execute_cmd_lists), execute_cmd_lists);
        }

        device_buffer->m_DataSize = data_size;
    }

    static void CreateDeviceBuffer(DX12Context* context, DX12DeviceBuffer* device_buffer, uint32_t size)
    {
        assert(device_buffer->m_Resource == 0x0);

        // create default heap
        // default heap is memory on the GPU. Only the GPU has access to this memory
        // To get data into this heap, we will have to upload the data using
        // an upload heap
        HRESULT hr = context->m_Device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), // a default heap
            D3D12_HEAP_FLAG_NONE,                              // no flags
            &CD3DX12_RESOURCE_DESC::Buffer(size),              // resource description for a buffer
            D3D12_RESOURCE_STATE_COPY_DEST,                    // we will start this heap in the copy destination state since we will copy data from the upload heap to this heap
            NULL,                                              // optimized clear value must be null for this type of resource. used for render targets and depth/stencil buffers
            IID_PPV_ARGS(&device_buffer->m_Resource));
        CHECK_HR_ERROR(hr);

        device_buffer->m_Resource->SetName(L"Vertex Buffer Resource Heap");
    }

    static HVertexBuffer DX12NewVertexBuffer(HContext _context, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        DX12Context* context        = (DX12Context*) _context;
        DX12VertexBuffer* vx_buffer = new DX12VertexBuffer();
        memset(vx_buffer, 0, sizeof(DX12VertexBuffer));

        if (size > 0)
        {
            CreateDeviceBuffer(context, &vx_buffer->m_DeviceBuffer, size);
            DeviceBufferUploadHelper(context, &vx_buffer->m_DeviceBuffer, data, size);
        }

        return (HVertexBuffer) vx_buffer;
    }

    static void DX12DeleteVertexBuffer(HVertexBuffer buffer)
    {
    }

    static void DX12SetVertexBufferData(HVertexBuffer _buffer, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        DM_PROFILE(__FUNCTION__);

        if (size == 0)
        {
            return;
        }

        DX12VertexBuffer* vx_buffer = (DX12VertexBuffer*) _buffer;
        if (vx_buffer->m_DeviceBuffer.m_Resource == 0x0)
        {
            CreateDeviceBuffer(g_DX12Context, &vx_buffer->m_DeviceBuffer, size);
        }

        DeviceBufferUploadHelper(g_DX12Context, &vx_buffer->m_DeviceBuffer, data, size);
    }

    static void DX12SetVertexBufferSubData(HVertexBuffer buffer, uint32_t offset, uint32_t size, const void* data)
    {
    }

    static uint32_t DX12GetMaxElementsVertices(HContext context)
    {
        return 65536;
    }

    static HIndexBuffer DX12NewIndexBuffer(HContext _context, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        DX12Context* context       = (DX12Context*) _context;
        DX12IndexBuffer* ix_buffer = new DX12IndexBuffer();
        memset(ix_buffer, 0, sizeof(DX12IndexBuffer));

        if (size > 0)
        {
            CreateDeviceBuffer(context, &ix_buffer->m_DeviceBuffer, size);
            DeviceBufferUploadHelper(context, &ix_buffer->m_DeviceBuffer, data, size);
        }

        return (HIndexBuffer) ix_buffer;
    }

    static void DX12DeleteIndexBuffer(HIndexBuffer buffer)
    {
    }

    static void DX12SetIndexBufferData(HIndexBuffer buffer, uint32_t size, const void* data, BufferUsage buffer_usage)
    {
        DM_PROFILE(__FUNCTION__);

        if (size == 0)
        {
            return;
        }

        DX12IndexBuffer* ix_buffer = (DX12IndexBuffer*) buffer;
        if (ix_buffer->m_DeviceBuffer.m_Resource == 0x0)
        {
            CreateDeviceBuffer(g_DX12Context, &ix_buffer->m_DeviceBuffer, size);
        }

        DeviceBufferUploadHelper(g_DX12Context, &ix_buffer->m_DeviceBuffer, data, size);
    }

    static void DX12SetIndexBufferSubData(HIndexBuffer buffer, uint32_t offset, uint32_t size, const void* data)
    {
    }

    static bool DX12IsIndexBufferFormatSupported(HContext context, IndexBufferFormat format)
    {
        return true;
    }

    static uint32_t DX12GetMaxElementsIndices(HContext context)
    {
        return 65536;
    }

    static HVertexDeclaration DX12NewVertexDeclaration(HContext context, HVertexStreamDeclaration stream_declaration)
    {
        VertexDeclaration* vd = new VertexDeclaration;
        memset(vd, 0, sizeof(VertexDeclaration));

        vd->m_Stride = 0;
        for (uint32_t i=0; i<stream_declaration->m_StreamCount; i++)
        {
            vd->m_Streams[i].m_NameHash  = stream_declaration->m_Streams[i].m_NameHash;
            vd->m_Streams[i].m_Location  = -1;
            vd->m_Streams[i].m_Size      = stream_declaration->m_Streams[i].m_Size;
            vd->m_Streams[i].m_Type      = stream_declaration->m_Streams[i].m_Type;
            vd->m_Streams[i].m_Normalize = stream_declaration->m_Streams[i].m_Normalize;
            vd->m_Streams[i].m_Offset    = vd->m_Stride;

            vd->m_Stride += stream_declaration->m_Streams[i].m_Size * GetTypeSize(stream_declaration->m_Streams[i].m_Type);
        }
        vd->m_StreamCount = stream_declaration->m_StreamCount;

        return vd;
    }

    static HVertexDeclaration DX12NewVertexDeclarationStride(HContext context, HVertexStreamDeclaration stream_declaration, uint32_t stride)
    {
        HVertexDeclaration vd = DX12NewVertexDeclaration(context, stream_declaration);
        vd->m_Stride = stride;
        return vd;
    }

    static void DX12EnableVertexBuffer(HContext _context, HVertexBuffer vertex_buffer, uint32_t binding_index)
    {
        DX12Context* context                          = (DX12Context*) _context;
        context->m_CurrentVertexBuffer[binding_index] = (DX12VertexBuffer*) vertex_buffer;
    }

    static void DX12DisableVertexBuffer(HContext _context, HVertexBuffer vertex_buffer)
    {
        DX12Context* context = (DX12Context*) _context;
        for (int i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        {
            if (context->m_CurrentVertexBuffer[i] == (DX12VertexBuffer*) vertex_buffer)
                context->m_CurrentVertexBuffer[i] = 0;
        }
    }

    static void DX12EnableVertexDeclaration(HContext _context, HVertexDeclaration vertex_declaration, uint32_t binding_index, HProgram program)
    {
        DX12Context* context            = (DX12Context*) _context;
        DX12ShaderProgram* program_ptr  = (DX12ShaderProgram*) program;
        DX12ShaderModule* vertex_shader = program_ptr->m_VertexModule;

        context->m_MainVertexDeclaration[binding_index]                = {};
        context->m_MainVertexDeclaration[binding_index].m_Stride       = vertex_declaration->m_Stride;
        context->m_MainVertexDeclaration[binding_index].m_StepFunction = vertex_declaration->m_StepFunction;
        context->m_MainVertexDeclaration[binding_index].m_PipelineHash = vertex_declaration->m_PipelineHash;

        context->m_CurrentVertexDeclaration[binding_index]             = &context->m_MainVertexDeclaration[binding_index];

        uint32_t stream_ix = 0;
        uint32_t num_inputs = vertex_shader->m_Inputs.Size();

        for (int i = 0; i < vertex_declaration->m_StreamCount; ++i)
        {
            for (int j = 0; j < num_inputs; ++j)
            {
                ShaderResourceBinding& input = vertex_shader->m_Inputs[j];

                if (input.m_NameHash == vertex_declaration->m_Streams[i].m_NameHash)
                {
                    VertexDeclaration::Stream& stream = context->m_MainVertexDeclaration[binding_index].m_Streams[stream_ix];
                    stream.m_NameHash  = input.m_NameHash;
                    stream.m_Location  = input.m_Binding;
                    stream.m_Type      = vertex_declaration->m_Streams[i].m_Type;
                    stream.m_Offset    = vertex_declaration->m_Streams[i].m_Offset;
                    stream.m_Size      = vertex_declaration->m_Streams[i].m_Size;
                    stream.m_Normalize = vertex_declaration->m_Streams[i].m_Normalize;
                    stream_ix++;

                    context->m_MainVertexDeclaration[binding_index].m_StreamCount++;
                    break;
                }
            }
        }
    }

    static void DX12DisableVertexDeclaration(HContext _context, HVertexDeclaration vertex_declaration)
    {
        DX12Context* context = (DX12Context*) _context;
        for (int i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        {
            if (context->m_CurrentVertexDeclaration[i] == vertex_declaration)
                context->m_CurrentVertexDeclaration[i] = 0;
        }
    }

    static inline D3D_PRIMITIVE_TOPOLOGY GetPrimitiveTopology(PrimitiveType prim_type)
    {
        switch(prim_type)
        {
            case PRIMITIVE_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case PRIMITIVE_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            case PRIMITIVE_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            default:break;
        }
        return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }

    static inline DXGI_FORMAT GetVertexAttributeFormat(Type type, uint16_t size, bool normalized)
    {
        /*
        // undefined formats:
        // DXGI_FORMAT_R8G8B8_SNORM
        // DXGI_FORMAT_R8G8B8_SINT
        // DXGI_FORMAT_R8G8B8_UNORM
        // DXGI_FORMAT_R8G8B8_UINT
        // DXGI_FORMAT_R16G16B16_SNORM
        // DXGI_FORMAT_R16G16B16_SINT
        // DXGI_FORMAT_R16G16B16_UNORM
        // DXGI_FORMAT_R16G16B16_UINT
        */
        if (type == TYPE_FLOAT)
        {
            switch(size)
            {
                case 1: return DXGI_FORMAT_R32_FLOAT;
                case 2: return DXGI_FORMAT_R32G32_FLOAT;
                case 3: return DXGI_FORMAT_R32G32B32_FLOAT;
                case 4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
                default:break;
            }
        }
        else if (type == TYPE_INT)
        {
            switch(size)
            {
                case 1: return DXGI_FORMAT_R32_SINT;
                case 2: return DXGI_FORMAT_R32G32_SINT;
                case 3: return DXGI_FORMAT_R32G32B32_SINT;
                case 4: return DXGI_FORMAT_R32G32B32A32_SINT;
                default:break;
            }
        }
        else if (type == TYPE_UNSIGNED_INT)
        {
            switch(size)
            {
                case 1: return DXGI_FORMAT_R32_UINT;
                case 2: return DXGI_FORMAT_R32G32_UINT;
                case 3: return DXGI_FORMAT_R32G32B32_UINT;
                case 4: return DXGI_FORMAT_R32G32B32A32_UINT;
                default:break;
            }
        }
        else if (type == TYPE_BYTE)
        {
            switch(size)
            {
                case 1: return normalized ? DXGI_FORMAT_R8_SNORM : DXGI_FORMAT_R8_SINT;
                case 2: return normalized ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R8G8_SINT;
                // case 3: return normalized ? DXGI_FORMAT_R8G8B8_SNORM : DXGI_FORMAT_R8G8B8_SINT;
                case 4: return normalized ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_R8G8B8A8_SINT;
                default:break;
            }
        }
        else if (type == TYPE_UNSIGNED_BYTE)
        {
            switch(size)
            {
                case 1: return normalized ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8_UINT;
                case 2: return normalized ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8G8_UINT;
                // case 3: return normalized ? DXGI_FORMAT_R8G8B8_UNORM : DXGI_FORMAT_R8G8B8_UINT;
                case 4: return normalized ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UINT;
                default:break;
            }
        }
        else if (type == TYPE_SHORT)
        {
            switch(size)
            {
                case 1: return normalized ? DXGI_FORMAT_R16_SNORM : DXGI_FORMAT_R16_SINT;
                case 2: return normalized ? DXGI_FORMAT_R16G16_SNORM : DXGI_FORMAT_R16G16_SINT;
                //case 3: return normalized ? DXGI_FORMAT_R16G16B16_SNORM : DXGI_FORMAT_R16G16B16_SINT;
                case 4: return normalized ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R16G16B16A16_SINT;
                default:break;
            }
        }
        else if (type == TYPE_UNSIGNED_SHORT)
        {
            switch(size)
            {
                case 1: return normalized ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16_UINT;
                case 2: return normalized ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16G16_UINT;
                //case 3: return normalized ? DXGI_FORMAT_R16G16B16_UNORM : DXGI_FORMAT_R16G16B16_UINT;
                case 4: return normalized ? DXGI_FORMAT_R16G16B16A16_UNORM : DXGI_FORMAT_R16G16B16A16_UINT;
                default:break;
            }
        }
        else if (type == TYPE_FLOAT_MAT4)
        {
            return DXGI_FORMAT_R32_FLOAT;
        }
        else if (type == TYPE_FLOAT_VEC4)
        {
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        }

        assert(0 && "Unable to deduce type from dmGraphics::Type");
        return DXGI_FORMAT_UNKNOWN;
    }

    static inline D3D12_CULL_MODE GetCullMode(const PipelineState& state)
    {
        if (state.m_CullFaceEnabled)
        {
            if (state.m_CullFaceType == FACE_TYPE_BACK)
                return D3D12_CULL_MODE_BACK;
            else if (state.m_CullFaceType == FACE_TYPE_FRONT)
                return D3D12_CULL_MODE_FRONT;
            // FRONT_AND_BACK not supported
        }
        return D3D12_CULL_MODE_NONE;
    }

    static void CreatePipeline(DX12Context* context, DX12RenderTarget* rt, DX12Pipeline* pipeline)
    {
        D3D12_SHADER_BYTECODE vs_byte_code = {};
        vs_byte_code.BytecodeLength        = context->m_CurrentProgram->m_VertexModule->m_ShaderBlob->GetBufferSize();
        vs_byte_code.pShaderBytecode       = context->m_CurrentProgram->m_VertexModule->m_ShaderBlob->GetBufferPointer();

        D3D12_SHADER_BYTECODE fs_byte_code = {};
        fs_byte_code.BytecodeLength        = context->m_CurrentProgram->m_FragmentModule->m_ShaderBlob->GetBufferSize();
        fs_byte_code.pShaderBytecode       = context->m_CurrentProgram->m_FragmentModule->m_ShaderBlob->GetBufferPointer();

        uint32_t stream_count = 0;
        D3D12_INPUT_ELEMENT_DESC input_layout[MAX_VERTEX_STREAM_COUNT];

        for (int i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        {
            if (context->m_CurrentVertexDeclaration[i])
            {
                for (int j = 0; j < context->m_CurrentVertexDeclaration[i]->m_StreamCount; ++j)
                {
                    VertexDeclaration::Stream& stream = context->m_CurrentVertexDeclaration[i]->m_Streams[j];
                    D3D12_INPUT_ELEMENT_DESC& desc    = input_layout[stream_count];

                    desc.SemanticName         = "TEXCOORD";
                    desc.SemanticIndex        = stream.m_Location;
                    desc.Format               = GetVertexAttributeFormat(stream.m_Type, stream.m_Size, stream.m_Normalize);
                    desc.InputSlot            = i;
                    desc.AlignedByteOffset    = stream.m_Offset;
                    desc.InputSlotClass       = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
                    desc.InstanceDataStepRate = 0;

                    stream_count++;
                }
            }
        }

        D3D12_INPUT_LAYOUT_DESC inputLayoutDesc = {};
        inputLayoutDesc.NumElements             = stream_count;
        inputLayoutDesc.pInputElementDescs      = input_layout;

        CD3DX12_RASTERIZER_DESC rasterizerState = CD3DX12_RASTERIZER_DESC(
            D3D12_FILL_MODE_SOLID,
            GetCullMode(context->m_PipelineState),
            true,                                       // FrontCounterClockwise
            D3D12_DEFAULT_DEPTH_BIAS,
            D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
            true,                                       // DepthClipEnable
            false,                                      // MultisampleEnable
            false,                                      // AntialiasedLineEnable: TODO
            0,                                          // forcedSampleCount
            D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF); // conservativeRaster



        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {}; // a structure to define a pso
        psoDesc.InputLayout           = inputLayoutDesc; // the structure describing our input layout
        psoDesc.pRootSignature        = context->m_CurrentProgram->m_RootSignature;
        psoDesc.VS                    = vs_byte_code;
        psoDesc.PS                    = fs_byte_code;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; // SHould we support points?
        psoDesc.RTVFormats[0]         = rt->m_Format;
        psoDesc.SampleDesc            = rt->m_SampleDesc; // must be the same sample description as the swapchain and depth/stencil buffer
        psoDesc.SampleMask            = 0xffffffff; // TODO: sample mask has to do with multi-sampling. 0xffffffff means point sampling is done
        psoDesc.RasterizerState       = rasterizerState;
        psoDesc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT); // TODO
        psoDesc.NumRenderTargets      = 1; // TODO

        HRESULT hr = context->m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(pipeline));
        CHECK_HR_ERROR(hr);
    }

    static DX12Pipeline* GetOrCreatePipeline(DX12Context* context, DX12RenderTarget* current_rt)
    {
        HashState64 pipeline_hash_state;
        dmHashInit64(&pipeline_hash_state, false);
        //dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_CurrentProgram->m_Hash, sizeof(context->m_CurrentProgram->m_Hash));
        dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_PipelineState,                   sizeof(context->m_PipelineState));
        dmHashUpdateBuffer64(&pipeline_hash_state, &current_rt->m_Id,                           sizeof(current_rt->m_Id));
        dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_CurrentProgram->m_RootSignature, sizeof(context->m_CurrentProgram->m_RootSignature));
        // dmHashUpdateBuffer64(&pipeline_hash_state, &vk_sample_count,  sizeof(vk_sample_count));

        // for (int i = 0; i < vertexDeclarationCount; ++i)
        // {
        //     dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_CurrentVertexDeclaration[i]->m_PipelineHash, sizeof(context->m_CurrentVertexDeclaration[i]->m_PipelineHash));
        //     dmHashUpdateBuffer64(&pipeline_hash_state, &context->m_CurrentVertexDeclaration[i]->m_StepFunction, sizeof(context->m_CurrentVertexDeclaration[i]->m_StepFunction));
        // }

        dmhash_t pipeline_hash = dmHashFinal64(&pipeline_hash_state);
        DX12Pipeline* cached_pipeline = context->m_PipelineCache.Get(pipeline_hash);

        if (!cached_pipeline)
        {
            if (context->m_PipelineCache.Full())
            {
                context->m_PipelineCache.SetCapacity(32, context->m_PipelineCache.Capacity() + 4);
            }

            context->m_PipelineCache.Put(pipeline_hash, {});
            cached_pipeline = context->m_PipelineCache.Get(pipeline_hash);
            CreatePipeline(context, current_rt, cached_pipeline);

            dmLogDebug("Created new DX12 Pipeline with hash %llu", (unsigned long long) pipeline_hash);
        }

        return cached_pipeline;
    }

    static inline void SetViewportAndScissorHelper(DX12Context* context, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        D3D12_VIEWPORT viewport;
        D3D12_RECT scissor;

        viewport.TopLeftX = (float) x;
        viewport.TopLeftY = (float) y;
        viewport.Width    = (float) width;
        viewport.Height   = (float) height;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        scissor.left   = (float) x;
        scissor.top    = (float) y;
        scissor.right  = (float) width;
        scissor.bottom = (float) height;

        context->m_CommandList->RSSetViewports(1, &viewport);
        context->m_CommandList->RSSetScissorRects(1, &scissor);
    }

    static void CommitUniforms(DX12Context* context, DX12FrameResource& frame_resources)
    {
        DX12ShaderProgram* program = context->m_CurrentProgram;

        for (int set = 0; set < program->m_MaxSet; ++set)
        {
            for (int binding = 0; binding < program->m_MaxBinding; ++binding)
            {
                DX12ShaderProgram::ProgramResourceBinding& res = program->m_ResourceBindings[set][binding];

                if (res.m_Res == 0x0)
                    continue;

                const uint32_t uniform_size_nonalign = res.m_Res->m_DataSize;
                void* gpu_mapped_memory = frame_resources.m_ScratchBuffer.AllocateConstantBuffer(context, uniform_size_nonalign);
                memcpy(gpu_mapped_memory, &program->m_UniformData[res.m_DataOffset], uniform_size_nonalign);
            }
        }
    }

    static void DrawSetup(DX12Context* context, PrimitiveType prim_type)
    {
        assert(context->m_CurrentProgram);

        DX12FrameResource& frame_resources = context->m_FrameResources[context->m_CurrentFrameIndex];

        DX12RenderTarget* current_rt = GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, context->m_CurrentRenderTarget);

        D3D12_VERTEX_BUFFER_VIEW vx_buffer_views[MAX_VERTEX_BUFFERS];
        uint32_t num_vx_buffers = 0;

        for (int i = 0; i < MAX_VERTEX_BUFFERS; ++i)
        {
            if (context->m_CurrentVertexBuffer[i] && context->m_CurrentVertexDeclaration[i])
            {
                vx_buffer_views[num_vx_buffers].BufferLocation = context->m_CurrentVertexBuffer[i]->m_DeviceBuffer.m_Resource->GetGPUVirtualAddress();
                vx_buffer_views[num_vx_buffers].SizeInBytes    = context->m_CurrentVertexBuffer[i]->m_DeviceBuffer.m_DataSize;
                vx_buffer_views[num_vx_buffers].StrideInBytes  = context->m_CurrentVertexDeclaration[i]->m_Stride;
                num_vx_buffers++;
            }
        }

        // Update the viewport
        if (context->m_ViewportChanged)
        {
            DX12Viewport& vp = context->m_CurrentViewport;
            SetViewportAndScissorHelper(context, vp.m_X, vp.m_Y, vp.m_W, vp.m_H);
            context->m_ViewportChanged = 0;
        }

        CommitUniforms(context, frame_resources);

        DX12Pipeline* pipeline = GetOrCreatePipeline(context, current_rt);

        context->m_CommandList->SetGraphicsRootSignature(context->m_CurrentProgram->m_RootSignature);
        context->m_CommandList->SetPipelineState(*pipeline);
        context->m_CommandList->IASetPrimitiveTopology(GetPrimitiveTopology(prim_type));
        context->m_CommandList->IASetVertexBuffers(0, num_vx_buffers, vx_buffer_views); // set the vertex buffer (using the vertex buffer view)

        frame_resources.m_ScratchBuffer.Bind(context);
    }

    static void DX12DrawElements(HContext _context, PrimitiveType prim_type, uint32_t first, uint32_t count, Type type, HIndexBuffer index_buffer)
    {
        DX12Context* context = (DX12Context*) _context;
        DrawSetup(context, prim_type);

        DX12IndexBuffer* ix_buffer   = (DX12IndexBuffer*) index_buffer;
        D3D12_INDEX_BUFFER_VIEW view = {};
        view.BufferLocation          = ix_buffer->m_DeviceBuffer.m_Resource->GetGPUVirtualAddress();
        view.SizeInBytes             = ix_buffer->m_DeviceBuffer.m_DataSize;
        view.Format                  = type == dmGraphics::TYPE_UNSIGNED_SHORT ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        uint32_t index_offset        = first / (type == TYPE_UNSIGNED_SHORT ? 2 : 4);

        context->m_CommandList->IASetIndexBuffer(&view);
        context->m_CommandList->DrawIndexedInstanced(count, 1, index_offset, 0, 0); // draw first quad
    }

    static void DX12Draw(HContext _context, PrimitiveType prim_type, uint32_t first, uint32_t count)
    {
        DX12Context* context = (DX12Context*) _context;
        DrawSetup(context, prim_type);

        context->m_CommandList->DrawInstanced(count, 1, first, 0);
    }

    static HComputeProgram DX12NewComputeProgram(HContext context, ShaderDesc::Shader* ddf)
    {
        return 0;
    }

    static HProgram DX12NewProgramFromCompute(HContext context, HComputeProgram compute_program)
    {
        return (HProgram) 0;
    }

    static void DX12DeleteComputeProgram(HComputeProgram prog)
    {
    }

    static bool DX12ReloadProgramCompute(HContext context, HProgram program, HComputeProgram compute_program)
    {
        return true;
    }

    static bool DX12ReloadComputeProgram(HComputeProgram prog, ShaderDesc::Shader* ddf)
    {
        return true;
    }

    static void FillProgramResourceBindings(
        DX12ShaderProgram* program,
        DX12ShaderModule* module,
        DX12ShaderProgram::ModuleType module_type,
        uint32_t& buffer_count,
        uint32_t& sampler_count,
        uint32_t& uniform_count,
        uint32_t& data_size,
        uint32_t& data_size_aligned,
        uint32_t& max_set,
        uint32_t& max_binding)
    {
        for (int i = 0; i < module->m_Uniforms.Size(); ++i)
        {
            ShaderResourceBinding& res = module->m_Uniforms[i];
            DX12ShaderProgram::ProgramResourceBinding& program_resource_binding = program->m_ResourceBindings[res.m_Set][res.m_Binding];

            if (program_resource_binding.m_Res == 0)
            {
                program_resource_binding.m_DataOffset = data_size;
                program_resource_binding.m_Res        = &res;

                if (!IsUniformTextureSampler(res))
                {
                    program_resource_binding.m_DataOffset         = data_size;
                    program_resource_binding.m_DynamicOffsetIndex = buffer_count;
                    buffer_count++;
                    uniform_count += res.m_BlockMembers.Size();

                    data_size         += res.m_DataSize;
                    data_size_aligned += DM_ALIGN(res.m_DataSize, 256); // Constant buffers needs 256 byte alignment
                }
                else
                {
                    // TODO
                }

                max_set     = dmMath::Max(max_set, (uint32_t) (res.m_Set + 1));
                max_binding = dmMath::Max(max_binding, (uint32_t) (res.m_Binding + 1));

            #if 0
                dmLogInfo("    name=%s, set=%d, binding=%d, data_offset=%d", res.m_Name, res.m_Set, res.m_Binding, program_resource_binding.m_DataOffset);
            #endif
            }

            program_resource_binding.m_StageFlags |= (int) module_type;
        }
    }

    static HProgram DX12NewProgram(HContext context, HVertexProgram vertex_program, HFragmentProgram fragment_program)
    {
        DX12ShaderProgram* program = new DX12ShaderProgram();
        program->m_VertexModule    = (DX12ShaderModule*) vertex_program;
        program->m_FragmentModule  = (DX12ShaderModule*) fragment_program;
        program->m_ComputeModule   = 0;

        uint32_t buffer_count      = 0;
        uint32_t sampler_count     = 0;
        uint32_t uniform_count     = 0;
        uint32_t data_size         = 0;
        uint32_t data_size_aligned = 0;
        uint32_t max_set           = 0;
        uint32_t max_binding       = 0;

        uint32_t num_buffers = program->m_VertexModule->m_UniformBufferCount + program->m_FragmentModule->m_UniformBufferCount;
        if (num_buffers > 0)
        {
            FillProgramResourceBindings(program, program->m_VertexModule, DX12ShaderProgram::MODULE_TYPE_VERTEX, buffer_count, sampler_count, uniform_count, data_size, data_size_aligned, max_set, max_binding);
            FillProgramResourceBindings(program, program->m_FragmentModule, DX12ShaderProgram::MODULE_TYPE_FRAGMENT, buffer_count, sampler_count, uniform_count, data_size, data_size_aligned, max_set, max_binding);

            program->m_UniformData = new uint8_t[data_size];
            memset(program->m_UniformData, 0, data_size);
        }

        /*
        // TODO: We should hash the data needed to generate this signature
        D3D12_DESCRIPTOR_RANGE desc_range;
        desc_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        desc_range.NumDescriptors                    = num_buffers;
        desc_range.BaseShaderRegister                = 0;
        desc_range.RegisterSpace                     = 0;
        desc_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_DESCRIPTOR_TABLE desc_table;
        desc_table.NumDescriptorRanges = 1;
        desc_table.pDescriptorRanges   = &desc_range;

        D3D12_ROOT_PARAMETER  root_param;
        root_param.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_param.DescriptorTable  = desc_table;
        root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        */

        // D3D12_ROOT_DESCRIPTOR_TABLE desc_table;
        // desc_table.NumDescriptorRanges = 1;
        // desc_table.pDescriptorRanges   = &desc_range;

        D3D12_ROOT_PARAMETER  root_params[2];
        root_params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[0].Descriptor.ShaderRegister = 0;
        root_params[0].Descriptor.RegisterSpace  = 0;

        root_params[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        root_params[1].Descriptor.ShaderRegister = 0;
        root_params[1].Descriptor.RegisterSpace  = 0;

        CD3DX12_ROOT_SIGNATURE_DESC root_signature_desc;
        root_signature_desc.Init(2, root_params,
            0, nullptr, // TODO: samplers
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | // we can deny shader stages here for better performance
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

        CreateRootSignature((DX12Context*) context, &root_signature_desc, program);

        program->m_UniformBufferCount  = buffer_count;
        program->m_TextureSamplerCount = sampler_count;
        program->m_TotalUniformCount   = uniform_count; // num buffer members + samplers
        program->m_TotalResourcesCount = buffer_count + sampler_count; // num actual descriptors
        program->m_MaxSet              = max_set;
        program->m_MaxBinding          = max_binding;

        return (HProgram) program;
    }

    static void DX12DeleteProgram(HContext context, HProgram program)
    {
    }

    static HRESULT CreateShaderModule(DX12Context* context, const char* target, void* data, uint32_t data_size, DX12ShaderModule* shader)
    {
        ID3DBlob* error_blob;
        uint32_t flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;

        HRESULT hr = D3DCompile(data, data_size, NULL, NULL, NULL, "main", target, flags, 0, &shader->m_ShaderBlob, &error_blob);
        if (FAILED(hr))
        {
            dmLogError("%s", error_blob->GetBufferPointer());
            return hr;
        }
        return S_OK;
    }

    static void CreateShaderResourceBindings(DX12ShaderModule* shader, ShaderDesc::Shader* ddf)
    {
        if (ddf->m_Resources.m_Count > 0)
        {
            shader->m_Uniforms.SetCapacity(ddf->m_Resources.m_Count);
            shader->m_Uniforms.SetSize(ddf->m_Resources.m_Count);
            memset(shader->m_Uniforms.Begin(), 0, sizeof(ShaderResourceBinding) * ddf->m_Resources.m_Count);

            uint32_t texture_sampler_count = 0;
            uint32_t uniform_buffer_count  = 0;
            uint32_t total_uniform_count   = 0;

            for (uint32_t i=0; i < ddf->m_Resources.m_Count; i++)
            {
                ShaderResourceBinding& res = shader->m_Uniforms[i];
                res.m_Binding              = ddf->m_Resources[i].m_Binding;
                res.m_Set                  = ddf->m_Resources[i].m_Set;
                res.m_Type                 = ddf->m_Resources[i].m_Type;
                res.m_ElementCount         = ddf->m_Resources[i].m_ElementCount;
                res.m_Name                 = strdup(ddf->m_Resources[i].m_Name);
                res.m_NameHash             = ddf->m_Resources[i].m_NameHash;

                if (IsUniformTextureSampler(res))
                {
                    res.m_TextureUnit = texture_sampler_count;
                    texture_sampler_count++;
                }
                else
                {
                    res.m_BlockMembers.SetCapacity(ddf->m_Resources[i].m_Bindings.m_Count);
                    res.m_BlockMembers.SetSize(ddf->m_Resources[i].m_Bindings.m_Count);

                    uint32_t uniform_size = 0;
                    for (uint32_t j = 0; j < ddf->m_Resources[i].m_Bindings.m_Count; ++j)
                    {
                        ShaderDesc::ResourceBinding& member  = ddf->m_Resources[i].m_Bindings[j];
                        res.m_BlockMembers[j].m_Name         = strdup(member.m_Name);
                        res.m_BlockMembers[j].m_NameHash     = member.m_NameHash;
                        res.m_BlockMembers[j].m_Type         = member.m_Type;
                        res.m_BlockMembers[j].m_ElementCount = member.m_ElementCount;
                        res.m_BlockMembers[j].m_Offset       = uniform_size;
                        uniform_size += GetShaderTypeSize(member.m_Type) * member.m_ElementCount;
                    }

                    res.m_UniformDataIndex = uniform_buffer_count;
                    res.m_DataSize         = uniform_size;
                    uniform_buffer_count++;
                }

                total_uniform_count += ddf->m_Resources[i].m_Bindings.m_Count;
            }

            shader->m_UniformBufferCount  = uniform_buffer_count;
            shader->m_TextureSamplerCount = texture_sampler_count;
            shader->m_TotalUniformCount   = total_uniform_count;
        }

        if (ddf->m_Inputs.m_Count > 0)
        {
            shader->m_Inputs.SetCapacity(ddf->m_Inputs.m_Count);
            shader->m_Inputs.SetSize(ddf->m_Inputs.m_Count);

            for (uint32_t i=0; i < ddf->m_Inputs.m_Count; i++)
            {
                ShaderResourceBinding& res = shader->m_Inputs[i];
                res.m_Binding              = ddf->m_Inputs[i].m_Binding;
                res.m_Set                  = ddf->m_Inputs[i].m_Set;
                res.m_Type                 = ddf->m_Inputs[i].m_Type;
                res.m_NameHash             = ddf->m_Inputs[i].m_NameHash;
                res.m_ElementCount         = ddf->m_Inputs[i].m_ElementCount;
                res.m_Name                 = strdup(ddf->m_Inputs[i].m_Name);
            }
        }
    }

    static HVertexProgram DX12NewVertexProgram(HContext _context, ShaderDesc::Shader* ddf)
    {
        DX12Context* context = (DX12Context*) _context;
        DX12ShaderModule* shader = new DX12ShaderModule;
        memset(shader, 0, sizeof(*shader));

        HRESULT hr = CreateShaderModule(context, "vs_5_0", ddf->m_Source.m_Data, ddf->m_Source.m_Count, shader);
        CHECK_HR_ERROR(hr);

        CreateShaderResourceBindings(shader, ddf);
        return (HVertexProgram) shader;
    }

    static HFragmentProgram DX12NewFragmentProgram(HContext _context, ShaderDesc::Shader* ddf)
    {
        DX12Context* context = (DX12Context*) _context;
        DX12ShaderModule* shader = new DX12ShaderModule;
        memset(shader, 0, sizeof(*shader));

        HRESULT hr = CreateShaderModule(context, "ps_5_0", ddf->m_Source.m_Data, ddf->m_Source.m_Count, shader);
        CHECK_HR_ERROR(hr);
        CreateShaderResourceBindings(shader, ddf);
        return (HVertexProgram) shader;
    }

    static bool DX12ReloadVertexProgram(HVertexProgram prog, ShaderDesc::Shader* ddf)
    {
        return 0;
    }

    static bool DX12ReloadFragmentProgram(HFragmentProgram prog, ShaderDesc::Shader* ddf)
    {
        return 0;
    }

    static void DX12DeleteVertexProgram(HVertexProgram program)
    {
    }

    static void DX12DeleteFragmentProgram(HFragmentProgram program)
    {
    }

    static ShaderDesc::Language DX12GetProgramLanguage(HProgram program)
    {
        return ShaderDesc::LANGUAGE_HLSL;
    }

    static ShaderDesc::Language DX12GetShaderProgramLanguage(HContext context, ShaderDesc::ShaderClass shader_class)
    {
        return ShaderDesc::LANGUAGE_HLSL;
    }

    static void DX12EnableProgram(HContext context, HProgram program)
    {
        ((DX12Context*) context)->m_CurrentProgram = (DX12ShaderProgram*) program;
    }

    static void DX12DisableProgram(HContext context)
    {
        ((DX12Context*) context)->m_CurrentProgram = 0;
    }

    static bool DX12ReloadProgramGraphics(HContext context, HProgram program, HVertexProgram vert_program, HFragmentProgram frag_program)
    {
        return true;
    }

    static uint32_t DX12GetAttributeCount(HProgram prog)
    {
        DX12ShaderProgram* program_ptr = (DX12ShaderProgram*) prog;
        return program_ptr->m_VertexModule->m_Inputs.Size();
    }

    static void DX12GetAttribute(HProgram prog, uint32_t index, dmhash_t* name_hash, Type* type, uint32_t* element_count, uint32_t* num_values, int32_t* location)
    {
        DX12ShaderProgram* program_ptr = (DX12ShaderProgram*) prog;
        assert(index < program_ptr->m_VertexModule->m_Inputs.Size());
        ShaderResourceBinding& attr = program_ptr->m_VertexModule->m_Inputs[index];

        *name_hash     = attr.m_NameHash;
        *type          = ShaderDataTypeToGraphicsType(attr.m_Type);
        *num_values    = attr.m_ElementCount;
        *location      = attr.m_Binding;
        *element_count = GetShaderTypeSize(attr.m_Type) / sizeof(float);
    }

    static uint32_t DX12GetUniformCount(HProgram prog)
    {
        DX12ShaderProgram* program_ptr = (DX12ShaderProgram*) prog;
        return program_ptr->m_TotalUniformCount;
    }

    static uint32_t DX12GetUniformName(HProgram prog, uint32_t index, char* buffer, uint32_t buffer_size, Type* type, int32_t* size)
    {
        assert(prog);
        DX12ShaderProgram* program = (DX12ShaderProgram*) prog;
        uint32_t search_index = 0;

        for (int set = 0; set < program->m_MaxSet; ++set)
        {
            for (int binding = 0; binding < program->m_MaxBinding; ++binding)
            {
                DX12ShaderProgram::ProgramResourceBinding& program_resource_binding = program->m_ResourceBindings[set][binding];

                if (program_resource_binding.m_Res == 0x0)
                    continue;

                if (IsUniformTextureSampler(*program_resource_binding.m_Res))
                {
                    if (search_index == index)
                    {
                        ShaderResourceBinding* res = program_resource_binding.m_Res;
                        *type = ShaderDataTypeToGraphicsType(res->m_Type);
                        *size = res->m_ElementCount;
                        return (uint32_t)dmStrlCpy(buffer, res->m_Name, buffer_size);
                    }
                    search_index++;
                }
                else
                {
                    for (int i = 0; i < program_resource_binding.m_Res->m_BlockMembers.Size(); ++i)
                    {
                        if (search_index == index)
                        {
                            UniformBlockMember& member = program_resource_binding.m_Res->m_BlockMembers[i];
                            *type = ShaderDataTypeToGraphicsType(member.m_Type);
                            *size = member.m_ElementCount;
                            return (uint32_t) dmStrlCpy(buffer, member.m_Name, buffer_size);
                        }

                        search_index++;
                    }
                }
            }
        }

        assert(0);
        return 0;
    }

    static HUniformLocation DX12GetUniformLocation(HProgram prog, const char* name)
    {
        assert(prog);
        DX12ShaderProgram* program_ptr = (DX12ShaderProgram*) prog;
        dmhash_t name_hash = dmHashString64(name);

        for (int set = 0; set < program_ptr->m_MaxSet; ++set)
        {
            for (int binding = 0; binding < program_ptr->m_MaxBinding; ++binding)
            {
                DX12ShaderProgram::ProgramResourceBinding& pgm_res = program_ptr->m_ResourceBindings[set][binding];

                if (pgm_res.m_Res == 0x0)
                    continue;

                if (pgm_res.m_Res->m_NameHash == name_hash)
                {
                    return set | binding << 16;
                }
                else
                {
                    for (uint32_t i = 0; i < pgm_res.m_Res->m_BlockMembers.Size(); ++i)
                    {
                        if (pgm_res.m_Res->m_BlockMembers[i].m_NameHash == name_hash)
                        {
                            return set | binding << 16 | ((uint64_t) i) << 32;
                        }
                    }
                }
            }
        }

        return INVALID_UNIFORM_LOCATION;
    }

    static inline void WriteConstantData(uint32_t offset, uint8_t* uniform_data_ptr, uint8_t* data_ptr, uint32_t data_size)
    {
        memcpy(&uniform_data_ptr[offset], data_ptr, data_size);
    }

    static void DX12SetConstantV4(HContext _context, const dmVMath::Vector4* data, int count, HUniformLocation base_location)
    {
        DX12Context* context = (DX12Context*) _context;
        assert(context->m_CurrentProgram);
        assert(base_location != INVALID_UNIFORM_LOCATION);

        DX12ShaderProgram* program_ptr = (DX12ShaderProgram*) context->m_CurrentProgram;
        uint32_t set                   = UNIFORM_LOCATION_GET_VS(base_location);
        uint32_t binding               = UNIFORM_LOCATION_GET_VS_MEMBER(base_location);
        uint32_t member                = UNIFORM_LOCATION_GET_FS(base_location);
        assert(!(set == UNIFORM_LOCATION_MAX && binding == UNIFORM_LOCATION_MAX));

        DX12ShaderProgram::ProgramResourceBinding& pgm_res = program_ptr->m_ResourceBindings[set][binding];
        WriteConstantData(
            pgm_res.m_DataOffset + pgm_res.m_Res->m_BlockMembers[member].m_Offset,
            program_ptr->m_UniformData,
            (uint8_t*) data,
            sizeof(dmVMath::Vector4) * count);
    }

    static void DX12SetConstantM4(HContext _context, const dmVMath::Vector4* data, int count, HUniformLocation base_location)
    {
        DX12Context* context = (DX12Context*) _context;
        assert(context->m_CurrentProgram);
        assert(base_location != INVALID_UNIFORM_LOCATION);

        DX12ShaderProgram* program_ptr = (DX12ShaderProgram*) context->m_CurrentProgram;
        uint32_t set                   = UNIFORM_LOCATION_GET_VS(base_location);
        uint32_t binding               = UNIFORM_LOCATION_GET_VS_MEMBER(base_location);
        uint32_t member                = UNIFORM_LOCATION_GET_FS(base_location);
        assert(!(set == UNIFORM_LOCATION_MAX && binding == UNIFORM_LOCATION_MAX));

        DX12ShaderProgram::ProgramResourceBinding& pgm_res = program_ptr->m_ResourceBindings[set][binding];
        WriteConstantData(
            pgm_res.m_DataOffset + pgm_res.m_Res->m_BlockMembers[member].m_Offset,
            program_ptr->m_UniformData,
            (uint8_t*) data,
            sizeof(dmVMath::Vector4) * 4 * count);
     }

    static void DX12SetSampler(HContext context, HUniformLocation location, int32_t unit)
    {
    }


    static HRenderTarget DX12NewRenderTarget(HContext _context, uint32_t buffer_type_flags, const RenderTargetCreationParams params)
    {
        return 0;
    }

    static void DX12DeleteRenderTarget(HRenderTarget render_target)
    {
    }

    static void DX12SetRenderTarget(HContext _context, HRenderTarget render_target, uint32_t transient_buffer_types)
    {
    }

    static HTexture DX12GetRenderTargetTexture(HRenderTarget render_target, BufferType buffer_type)
    {
        return 0;
    }

    static void DX12GetRenderTargetSize(HRenderTarget render_target, BufferType buffer_type, uint32_t& width, uint32_t& height)
    {
    }

    static void DX12SetRenderTargetSize(HRenderTarget render_target, uint32_t width, uint32_t height)
    {
    }

    static bool DX12IsTextureFormatSupported(HContext _context, TextureFormat format)
    {
        DX12Context* context = (DX12Context*) _context;
        return (context->m_TextureFormatSupport & (1 << format)) != 0;
    }

    static uint32_t DX12GetMaxTextureSize(HContext context)
    {
        return 1024;
    }

    static HTexture DX12NewTexture(HContext _context, const TextureCreationParams& params)
    {
        DX12Context* context = (DX12Context*) _context;

        DX12Texture* tex = new DX12Texture;
        // InitializeVulkanTexture(tex);

        tex->m_Type        = params.m_Type;
        tex->m_Width       = params.m_Width;
        tex->m_Height      = params.m_Height;
        tex->m_Depth       = params.m_Depth;
        tex->m_MipMapCount = params.m_MipMapCount;
        // tex->m_UsageFlags  = GetVulkanUsageFromHints(params.m_UsageHintBits);

        if (params.m_OriginalWidth == 0)
        {
            tex->m_OriginalWidth  = params.m_Width;
            tex->m_OriginalHeight = params.m_Height;
        }
        else
        {
            tex->m_OriginalWidth  = params.m_OriginalWidth;
            tex->m_OriginalHeight = params.m_OriginalHeight;
        }
        return StoreAssetInContainer(context->m_AssetHandleContainer, tex, ASSET_TYPE_TEXTURE);
    }

    static void DX12DeleteTexture(HTexture texture)
    {
    }

    static HandleResult DX12GetTextureHandle(HTexture texture, void** out_handle)
    {

        return HANDLE_RESULT_OK;
    }

    static void DX12SetTextureParams(HTexture texture, TextureFilter minfilter, TextureFilter magfilter, TextureWrap uwrap, TextureWrap vwrap, float max_anisotropy)
    {
    }

    static void DX12SetTexture(HTexture texture, const TextureParams& params)
    {

    }

    static uint32_t DX12GetTextureResourceSize(HTexture texture)
    {
        return 0;
    }

    static uint16_t DX12GetTextureWidth(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_Width;
    }

    static uint16_t DX12GetTextureHeight(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_Height;
    }

    static uint16_t DX12GetOriginalTextureWidth(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_OriginalWidth;
    }

    static uint16_t DX12GetOriginalTextureHeight(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_OriginalHeight;
    }

    static void DX12EnableTexture(HContext _context, uint32_t unit, uint8_t value_index, HTexture texture)
    {
    }

    static void DX12DisableTexture(HContext context, uint32_t unit, HTexture texture)
    {
    }

    static void DX12ReadPixels(HContext context, void* buffer, uint32_t buffer_size)
    {
    }

    static void DX12SetViewport(HContext _context, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        DX12Context* context = (DX12Context*) _context;

        DX12Viewport& viewport = context->m_CurrentViewport;
        viewport.m_X = (uint16_t) x;
        viewport.m_Y = (uint16_t) y;
        viewport.m_W = (uint16_t) width;
        viewport.m_H = (uint16_t) height;
        context->m_ViewportChanged = 1;
    }

    static void DX12EnableState(HContext context, State state)
    {
        SetPipelineStateValue(g_DX12Context->m_PipelineState, state, 1);
    }

    static void DX12DisableState(HContext context, State state)
    {
        SetPipelineStateValue(g_DX12Context->m_PipelineState, state, 0);
    }

    static void DX12SetBlendFunc(HContext _context, BlendFactor source_factor, BlendFactor destinaton_factor)
    {
        g_DX12Context->m_PipelineState.m_BlendSrcFactor = source_factor;
        g_DX12Context->m_PipelineState.m_BlendDstFactor = destinaton_factor;
    }

    static void DX12SetColorMask(HContext context, bool red, bool green, bool blue, bool alpha)
    {
        assert(context);
        uint8_t write_mask = red   ? DM_GRAPHICS_STATE_WRITE_R : 0;
        write_mask        |= green ? DM_GRAPHICS_STATE_WRITE_G : 0;
        write_mask        |= blue  ? DM_GRAPHICS_STATE_WRITE_B : 0;
        write_mask        |= alpha ? DM_GRAPHICS_STATE_WRITE_A : 0;

        g_DX12Context->m_PipelineState.m_WriteColorMask = write_mask;
    }

    static void DX12SetDepthMask(HContext context, bool mask)
    {
        g_DX12Context->m_PipelineState.m_WriteDepth = mask;
    }

    static void DX12SetDepthFunc(HContext context, CompareFunc func)
    {
        g_DX12Context->m_PipelineState.m_DepthTestFunc = func;
    }

    static void DX12SetScissor(HContext _context, int32_t x, int32_t y, int32_t width, int32_t height)
    {
    }

    static void DX12SetStencilMask(HContext context, uint32_t mask)
    {
        g_DX12Context->m_PipelineState.m_StencilWriteMask = mask;
    }

    static void DX12SetStencilFunc(HContext _context, CompareFunc func, uint32_t ref, uint32_t mask)
    {
        g_DX12Context->m_PipelineState.m_StencilFrontTestFunc = (uint8_t) func;
        g_DX12Context->m_PipelineState.m_StencilBackTestFunc  = (uint8_t) func;
        g_DX12Context->m_PipelineState.m_StencilReference     = (uint8_t) ref;
        g_DX12Context->m_PipelineState.m_StencilCompareMask   = (uint8_t) mask;
    }

    static void DX12SetStencilOp(HContext _context, StencilOp sfail, StencilOp dpfail, StencilOp dppass)
    {
        g_DX12Context->m_PipelineState.m_StencilFrontOpFail      = sfail;
        g_DX12Context->m_PipelineState.m_StencilFrontOpDepthFail = dpfail;
        g_DX12Context->m_PipelineState.m_StencilFrontOpPass      = dppass;
        g_DX12Context->m_PipelineState.m_StencilBackOpFail       = sfail;
        g_DX12Context->m_PipelineState.m_StencilBackOpDepthFail  = dpfail;
        g_DX12Context->m_PipelineState.m_StencilBackOpPass       = dppass;
    }

    static void DX12SetStencilFuncSeparate(HContext _context, FaceType face_type, CompareFunc func, uint32_t ref, uint32_t mask)
    {
        if (face_type == FACE_TYPE_BACK)
        {
            g_DX12Context->m_PipelineState.m_StencilBackTestFunc  = (uint8_t) func;
        }
        else
        {
            g_DX12Context->m_PipelineState.m_StencilFrontTestFunc = (uint8_t) func;
        }
        g_DX12Context->m_PipelineState.m_StencilReference     = (uint8_t) ref;
        g_DX12Context->m_PipelineState.m_StencilCompareMask   = (uint8_t) mask;
    }

    static void DX12SetStencilOpSeparate(HContext _context, FaceType face_type, StencilOp sfail, StencilOp dpfail, StencilOp dppass)
    {
        if (face_type == FACE_TYPE_BACK)
        {
            g_DX12Context->m_PipelineState.m_StencilBackOpFail       = sfail;
            g_DX12Context->m_PipelineState.m_StencilBackOpDepthFail  = dpfail;
            g_DX12Context->m_PipelineState.m_StencilBackOpPass       = dppass;
        }
        else
        {
            g_DX12Context->m_PipelineState.m_StencilFrontOpFail      = sfail;
            g_DX12Context->m_PipelineState.m_StencilFrontOpDepthFail = dpfail;
            g_DX12Context->m_PipelineState.m_StencilFrontOpPass      = dppass;
        }
    }

    static void DX12SetFaceWinding(HContext context, FaceWinding face_winding)
    {
        // TODO: Add this to the DX12 pipeline handle aswell, for now it's a NOP
    }

    static void DX12SetCullFace(HContext context, FaceType face_type)
    {
        g_DX12Context->m_PipelineState.m_CullFaceType = face_type;
        g_DX12Context->m_CullFaceChanged              = true;
    }

    static void DX12SetPolygonOffset(HContext context, float factor, float units)
    {
        // TODO: Add this to the DX12 pipeline handle aswell, for now it's a NOP
    }

    static PipelineState DX12GetPipelineState(HContext context)
    {
        return ((DX12Context*) context)->m_PipelineState;
    }

    static void DX12SetTextureAsync(HTexture texture, const TextureParams& params)
    {
        // TODO
    }

    static uint32_t DX12GetTextureStatusFlags(HTexture texture)
    {
        return TEXTURE_STATUS_OK;
    }

    static bool DX12IsExtensionSupported(HContext context, const char* extension)
    {
        return true;
    }

    static TextureType DX12GetTextureType(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_Type;
    }

    static uint32_t DX12GetNumSupportedExtensions(HContext context)
    {
        return 0;
    }

    static const char* DX12GetSupportedExtension(HContext context, uint32_t index)
    {
        return "";
    }

    static uint8_t DX12GetNumTextureHandles(HTexture texture)
    {
        return 1;
    }

    static bool DX12IsContextFeatureSupported(HContext context, ContextFeature feature)
    {
        return true;
    }

    static uint16_t DX12GetTextureDepth(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_Depth;
    }

    static uint8_t DX12GetTextureMipmapCount(HTexture texture)
    {
        return GetAssetFromContainer<DX12Texture>(g_DX12Context->m_AssetHandleContainer, texture)->m_MipMapCount;
    }

    static bool DX12IsAssetHandleValid(HContext _context, HAssetHandle asset_handle)
    {
        assert(_context);
        if (asset_handle == 0)
        {
            return false;
        }
        DX12Context* context = (DX12Context*) _context;
        AssetType type       = GetAssetType(asset_handle);
        if (type == ASSET_TYPE_TEXTURE)
        {
            return GetAssetFromContainer<DX12Texture>(context->m_AssetHandleContainer, asset_handle) != 0;
        }
        else if (type == ASSET_TYPE_RENDER_TARGET)
        {
            return GetAssetFromContainer<DX12RenderTarget>(context->m_AssetHandleContainer, asset_handle) != 0;
        }
        return false;
    }

    static GraphicsAdapterFunctionTable DX12RegisterFunctionTable()
    {
        GraphicsAdapterFunctionTable fn_table = {};
        DM_REGISTER_GRAPHICS_FUNCTION_TABLE(fn_table, DX12);
        return fn_table;
    }
}

