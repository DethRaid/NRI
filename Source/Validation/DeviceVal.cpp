// © 2021 NVIDIA Corporation

#include "SharedExternal.h"
#include "SharedVal.h"

#include "AccelerationStructureVal.h"
#include "BufferVal.h"
#include "CommandAllocatorVal.h"
#include "CommandBufferVal.h"
#include "CommandQueueVal.h"
#include "DescriptorPoolVal.h"
#include "DescriptorVal.h"
#include "DeviceVal.h"
#include "FenceVal.h"
#include "MemoryVal.h"
#include "PipelineLayoutVal.h"
#include "PipelineVal.h"
#include "QueryPoolVal.h"
#include "SwapChainVal.h"
#include "TextureVal.h"

using namespace nri;

static inline bool IsShaderStageValid(StageBits shaderStages, uint32_t& uniqueShaderStages, StageBits allowedStages) {
    uint32_t x = (uint32_t)(shaderStages & allowedStages);
    uint32_t n = 0;
    while (x) {
        n += x & 1;
        x >>= 1;
    }

    x = (uint32_t)shaderStages;
    bool isUnique = (uniqueShaderStages & x) == 0;
    uniqueShaderStages |= x;

    return n == 1 && isUnique;
}

void ConvertGeometryObjectsVal(GeometryObject* destObjects, const GeometryObject* sourceObjects, uint32_t objectNum);
QueryType GetQueryTypeVK(uint32_t queryTypeVK);

DeviceVal::DeviceVal(const CallbackInterface& callbacks, const StdAllocator<uint8_t>& stdAllocator, DeviceBase& device)
    : DeviceBase(callbacks, stdAllocator)
    , m_Device(*(Device*)&device)
    , m_Name(GetStdAllocator())
    , m_MemoryTypeMap(GetStdAllocator()) {
}

DeviceVal::~DeviceVal() {
    for (size_t i = 0; i < m_CommandQueues.size(); i++)
        Destroy(GetStdAllocator(), m_CommandQueues[i]);
    ((DeviceBase*)&m_Device)->Destruct();
}

bool DeviceVal::Create() {
    const DeviceBase& deviceBase = (DeviceBase&)m_Device;

    if (deviceBase.FillFunctionTable(m_CoreAPI) != Result::SUCCESS) {
        REPORT_ERROR(this, "Failed to get 'CoreInterface' interface");
        return false;
    }

    if (deviceBase.FillFunctionTable(m_HelperAPI) != Result::SUCCESS) {
        REPORT_ERROR(this, "Failed to get 'HelperInterface' interface");
        return false;
    }

    if (deviceBase.FillFunctionTable(m_StreamerAPI) != Result::SUCCESS) {
        REPORT_ERROR(this, "Failed to get 'StreamerInterface' interface");
        return false;
    }

    if (deviceBase.FillFunctionTable(m_ResourceAllocatorAPI) != Result::SUCCESS) {
        REPORT_ERROR(this, "Failed to get 'ResourceAllocatorInterface' interface");
        return false;
    }

    m_IsLowLatencySupported = deviceBase.FillFunctionTable(m_LowLatencyAPI) == Result::SUCCESS;
    m_IsMeshShaderSupported = deviceBase.FillFunctionTable(m_MeshShaderAPI) == Result::SUCCESS;
    m_IsRayTracingSupported = deviceBase.FillFunctionTable(m_RayTracingAPI) == Result::SUCCESS;
    m_IsSwapChainSupported = deviceBase.FillFunctionTable(m_SwapChainAPI) == Result::SUCCESS;
    m_IsWrapperD3D11Supported = deviceBase.FillFunctionTable(m_WrapperD3D11API) == Result::SUCCESS;
    m_IsWrapperD3D12Supported = deviceBase.FillFunctionTable(m_WrapperD3D12API) == Result::SUCCESS;
    m_IsWrapperVKSupported = deviceBase.FillFunctionTable(m_WrapperVKAPI) == Result::SUCCESS;

    return true;
}

void DeviceVal::RegisterMemoryType(MemoryType memoryType, MemoryLocation memoryLocation) {
    ExclusiveScope lockScope(m_Lock);
    m_MemoryTypeMap[memoryType] = memoryLocation;
}

Result DeviceVal::CreateSwapChain(const SwapChainDesc& swapChainDesc, SwapChain*& swapChain) {
    RETURN_ON_FAILURE(this, swapChainDesc.commandQueue != nullptr, Result::INVALID_ARGUMENT, "'swapChainDesc.commandQueue' is NULL");
    RETURN_ON_FAILURE(this, swapChainDesc.width != 0, Result::INVALID_ARGUMENT, "'swapChainDesc.width' is 0");
    RETURN_ON_FAILURE(this, swapChainDesc.height != 0, Result::INVALID_ARGUMENT, "'swapChainDesc.height' is 0");
    RETURN_ON_FAILURE(this, swapChainDesc.textureNum > 0, Result::INVALID_ARGUMENT, "'swapChainDesc.textureNum' is invalid");
    RETURN_ON_FAILURE(this, swapChainDesc.format < SwapChainFormat::MAX_NUM, Result::INVALID_ARGUMENT, "'swapChainDesc.format' is invalid");

    auto swapChainDescImpl = swapChainDesc;
    swapChainDescImpl.commandQueue = NRI_GET_IMPL(CommandQueue, swapChainDesc.commandQueue);

    SwapChain* swapChainImpl;
    Result result = m_SwapChainAPI.CreateSwapChain(m_Device, swapChainDescImpl, swapChainImpl);

    if (result == Result::SUCCESS)
        swapChain = (SwapChain*)Allocate<SwapChainVal>(GetStdAllocator(), *this, swapChainImpl, swapChainDesc);

    return result;
}

void DeviceVal::DestroySwapChain(SwapChain& swapChain) {
    m_SwapChainAPI.DestroySwapChain(*NRI_GET_IMPL(SwapChain, &swapChain));
    Destroy(GetStdAllocator(), (SwapChainVal*)&swapChain);
}

void DeviceVal::SetDebugName(const char* name) {
    m_Name = name;
    m_CoreAPI.SetDeviceDebugName(m_Device, name);
}

const DeviceDesc& DeviceVal::GetDesc() const {
    return ((DeviceBase&)m_Device).GetDesc();
}

Result DeviceVal::GetCommandQueue(CommandQueueType commandQueueType, CommandQueue*& commandQueue) {
    RETURN_ON_FAILURE(this, commandQueueType < CommandQueueType::MAX_NUM, Result::INVALID_ARGUMENT, "'commandQueueType' is invalid");

    CommandQueue* commandQueueImpl;
    Result result = m_CoreAPI.GetCommandQueue(m_Device, commandQueueType, commandQueueImpl);

    if (result == Result::SUCCESS) {
        const uint32_t index = (uint32_t)commandQueueType;
        if (!m_CommandQueues[index])
            m_CommandQueues[index] = Allocate<CommandQueueVal>(GetStdAllocator(), *this, commandQueueImpl);

        commandQueue = (CommandQueue*)m_CommandQueues[index];
    }

    return result;
}

Result DeviceVal::CreateCommandAllocator(const CommandQueue& commandQueue, CommandAllocator*& commandAllocator) {
    auto commandQueueImpl = NRI_GET_IMPL(CommandQueue, &commandQueue);

    CommandAllocator* commandAllocatorImpl = nullptr;
    Result result = m_CoreAPI.CreateCommandAllocator(*commandQueueImpl, commandAllocatorImpl);

    if (result == Result::SUCCESS)
        commandAllocator = (CommandAllocator*)Allocate<CommandAllocatorVal>(GetStdAllocator(), *this, commandAllocatorImpl);

    return result;
}

Result DeviceVal::CreateDescriptorPool(const DescriptorPoolDesc& descriptorPoolDesc, DescriptorPool*& descriptorPool) {
    DescriptorPool* descriptorPoolImpl = nullptr;
    Result result = m_CoreAPI.CreateDescriptorPool(m_Device, descriptorPoolDesc, descriptorPoolImpl);

    if (result == Result::SUCCESS)
        descriptorPool = (DescriptorPool*)Allocate<DescriptorPoolVal>(GetStdAllocator(), *this, descriptorPoolImpl, descriptorPoolDesc);

    return result;
}

Result DeviceVal::CreateBuffer(const BufferDesc& bufferDesc, Buffer*& buffer) {
    RETURN_ON_FAILURE(this, bufferDesc.size != 0, Result::INVALID_ARGUMENT, "'bufferDesc.size' is 0");

    Buffer* bufferImpl = nullptr;
    Result result = m_CoreAPI.CreateBuffer(m_Device, bufferDesc, bufferImpl);

    if (result == Result::SUCCESS)
        buffer = (Buffer*)Allocate<BufferVal>(GetStdAllocator(), *this, bufferImpl, false);

    return result;
}

Result DeviceVal::AllocateBuffer(const AllocateBufferDesc& bufferDesc, Buffer*& buffer) {
    RETURN_ON_FAILURE(this, bufferDesc.desc.size != 0, Result::INVALID_ARGUMENT, "'bufferDesc.size' is 0");

    Buffer* bufferImpl = nullptr;
    Result result = m_ResourceAllocatorAPI.AllocateBuffer(m_Device, bufferDesc, bufferImpl);

    if (result == Result::SUCCESS)
        buffer = (Buffer*)Allocate<BufferVal>(GetStdAllocator(), *this, bufferImpl, true);

    return result;
}

static inline Mip_t GetMaxMipNum(uint16_t w, uint16_t h, uint16_t d) {
    Mip_t mipNum = 1;

    while (w > 1 || h > 1 || d > 1) {
        if (w > 1)
            w >>= 1;

        if (h > 1)
            h >>= 1;

        if (d > 1)
            d >>= 1;

        mipNum++;
    }

    return mipNum;
}

Result DeviceVal::CreateTexture(const TextureDesc& textureDesc, Texture*& texture) {
    Mip_t maxMipNum = GetMaxMipNum(textureDesc.width, textureDesc.height, textureDesc.depth);

    RETURN_ON_FAILURE(this, textureDesc.format > Format::UNKNOWN && textureDesc.format < Format::MAX_NUM, Result::INVALID_ARGUMENT, "'textureDesc.format' is invalid");
    RETURN_ON_FAILURE(this, textureDesc.width != 0, Result::INVALID_ARGUMENT, "'textureDesc.width' is 0");
    RETURN_ON_FAILURE(this, textureDesc.height != 0, Result::INVALID_ARGUMENT, "'textureDesc.height' is 0");
    RETURN_ON_FAILURE(this, textureDesc.depth != 0, Result::INVALID_ARGUMENT, "'textureDesc.depth' is 0");
    RETURN_ON_FAILURE(this, textureDesc.mipNum != 0, Result::INVALID_ARGUMENT, "'textureDesc.mipNum' is 0");
    RETURN_ON_FAILURE(this, textureDesc.mipNum <= maxMipNum, Result::INVALID_ARGUMENT, "'textureDesc.mipNum = %u' can't be > %u", textureDesc.mipNum, maxMipNum);
    RETURN_ON_FAILURE(this, textureDesc.layerNum != 0, Result::INVALID_ARGUMENT, "'textureDesc.layerNum' is 0");
    RETURN_ON_FAILURE(this, textureDesc.sampleNum != 0, Result::INVALID_ARGUMENT, "'textureDesc.sampleNum' is 0");

    Texture* textureImpl = nullptr;
    Result result = m_CoreAPI.CreateTexture(m_Device, textureDesc, textureImpl);

    if (result == Result::SUCCESS)
        texture = (Texture*)Allocate<TextureVal>(GetStdAllocator(), *this, textureImpl, false);

    return result;
}

Result DeviceVal::AllocateTexture(const AllocateTextureDesc& textureDesc, Texture*& texture) {
    Mip_t maxMipNum = GetMaxMipNum(textureDesc.desc.width, textureDesc.desc.height, textureDesc.desc.depth);

    RETURN_ON_FAILURE(this, textureDesc.desc.format > Format::UNKNOWN && textureDesc.desc.format < Format::MAX_NUM, Result::INVALID_ARGUMENT, "'textureDesc.format' is invalid");
    RETURN_ON_FAILURE(this, textureDesc.desc.width != 0, Result::INVALID_ARGUMENT, "'textureDesc.width' is 0");
    RETURN_ON_FAILURE(this, textureDesc.desc.height != 0, Result::INVALID_ARGUMENT, "'textureDesc.height' is 0");
    RETURN_ON_FAILURE(this, textureDesc.desc.depth != 0, Result::INVALID_ARGUMENT, "'textureDesc.depth' is 0");
    RETURN_ON_FAILURE(this, textureDesc.desc.mipNum != 0, Result::INVALID_ARGUMENT, "'textureDesc.mipNum' is 0");
    RETURN_ON_FAILURE(this, textureDesc.desc.mipNum <= maxMipNum, Result::INVALID_ARGUMENT, "'textureDesc.mipNum = %u' can't be > %u", textureDesc.desc.mipNum, maxMipNum);
    RETURN_ON_FAILURE(this, textureDesc.desc.layerNum != 0, Result::INVALID_ARGUMENT, "'textureDesc.layerNum' is 0");
    RETURN_ON_FAILURE(this, textureDesc.desc.sampleNum != 0, Result::INVALID_ARGUMENT, "'textureDesc.sampleNum' is 0");

    Texture* textureImpl = nullptr;
    Result result = m_ResourceAllocatorAPI.AllocateTexture(m_Device, textureDesc, textureImpl);

    if (result == Result::SUCCESS)
        texture = (Texture*)Allocate<TextureVal>(GetStdAllocator(), *this, textureImpl, true);

    return result;
}

Result DeviceVal::CreateDescriptor(const BufferViewDesc& bufferViewDesc, Descriptor*& bufferView) {
    RETURN_ON_FAILURE(this, bufferViewDesc.buffer != nullptr, Result::INVALID_ARGUMENT, "'bufferViewDesc.buffer' is NULL");
    RETURN_ON_FAILURE(this, bufferViewDesc.format < Format::MAX_NUM, Result::INVALID_ARGUMENT, "'bufferViewDesc.format' is invalid");
    RETURN_ON_FAILURE(this, bufferViewDesc.viewType < BufferViewType::MAX_NUM, Result::INVALID_ARGUMENT, "'bufferViewDesc.viewType' is invalid");

    const BufferDesc& bufferDesc = ((BufferVal*)bufferViewDesc.buffer)->GetDesc();

    RETURN_ON_FAILURE(this, bufferViewDesc.offset < bufferDesc.size, Result::INVALID_ARGUMENT,
        "'bufferViewDesc.offset' is invalid. (bufferViewDesc.offset=%llu, bufferDesc.size=%llu)", bufferViewDesc.offset, bufferDesc.size);

    RETURN_ON_FAILURE(this, bufferViewDesc.offset + bufferViewDesc.size <= bufferDesc.size, Result::INVALID_ARGUMENT,
        "'bufferViewDesc.size' is invalid. (bufferViewDesc.offset=%llu, bufferViewDesc.size=%llu, bufferDesc.size=%llu)", bufferViewDesc.offset,
        bufferViewDesc.size, bufferDesc.size);

    auto bufferViewDescImpl = bufferViewDesc;
    bufferViewDescImpl.buffer = NRI_GET_IMPL(Buffer, bufferViewDesc.buffer);

    Descriptor* descriptorImpl = nullptr;
    Result result = m_CoreAPI.CreateBufferView(bufferViewDescImpl, descriptorImpl);

    if (result == Result::SUCCESS)
        bufferView = (Descriptor*)Allocate<DescriptorVal>(GetStdAllocator(), *this, descriptorImpl, bufferViewDesc);

    return result;
}

Result DeviceVal::CreateDescriptor(const Texture1DViewDesc& textureViewDesc, Descriptor*& textureView) {
    RETURN_ON_FAILURE(this, textureViewDesc.texture != nullptr, Result::INVALID_ARGUMENT, "'textureViewDesc.texture' is NULL");
    RETURN_ON_FAILURE(this, textureViewDesc.viewType < Texture1DViewType::MAX_NUM, Result::INVALID_ARGUMENT, "'textureViewDesc.viewType' is invalid");

    RETURN_ON_FAILURE(this, textureViewDesc.format > Format::UNKNOWN && textureViewDesc.format < Format::MAX_NUM, Result::INVALID_ARGUMENT,
        "'textureViewDesc.format' is invalid");

    const TextureDesc& textureDesc = ((TextureVal*)textureViewDesc.texture)->GetDesc();

    RETURN_ON_FAILURE(this, textureViewDesc.mipOffset < textureDesc.mipNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.mipOffset' is invalid (textureViewDesc.mipOffset=%hu, textureDesc.mipNum=%hu)", textureViewDesc.mipOffset, textureDesc.mipNum);

    RETURN_ON_FAILURE(this, textureViewDesc.mipOffset + textureViewDesc.mipNum <= textureDesc.mipNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.mipNum' is invalid (textureViewDesc.mipOffset=%hu, textureViewDesc.mipNum=%hu, textureDesc.mipNum=%hu)", textureViewDesc.mipOffset,
        textureViewDesc.mipNum, textureDesc.mipNum);

    RETURN_ON_FAILURE(this, textureViewDesc.layerOffset < textureDesc.layerNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.layerOffset' is invalid (textureViewDesc.layerOffset=%hu, textureDesc.layerNum=%hu)", textureViewDesc.layerOffset,
        textureDesc.layerNum);

    RETURN_ON_FAILURE(this, textureViewDesc.layerOffset + textureViewDesc.layerNum <= textureDesc.layerNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.layerNum' is invalid (textureViewDesc.layerOffset=%hu, textureViewDesc.layerNum=%hu, textureDesc.layerNum=%hu)",
        textureViewDesc.layerOffset, textureViewDesc.layerNum, textureDesc.layerNum);

    auto textureViewDescImpl = textureViewDesc;
    textureViewDescImpl.texture = NRI_GET_IMPL(Texture, textureViewDesc.texture);

    Descriptor* descriptorImpl = nullptr;
    Result result = m_CoreAPI.CreateTexture1DView(textureViewDescImpl, descriptorImpl);

    if (result == Result::SUCCESS)
        textureView = (Descriptor*)Allocate<DescriptorVal>(GetStdAllocator(), *this, descriptorImpl, textureViewDesc);

    return result;
}

Result DeviceVal::CreateDescriptor(const Texture2DViewDesc& textureViewDesc, Descriptor*& textureView) {
    RETURN_ON_FAILURE(this, textureViewDesc.texture != nullptr, Result::INVALID_ARGUMENT, "'textureViewDesc.texture' is NULL");
    RETURN_ON_FAILURE(this, textureViewDesc.viewType < Texture2DViewType::MAX_NUM, Result::INVALID_ARGUMENT, "'textureViewDesc.viewType' is invalid");

    RETURN_ON_FAILURE(this, textureViewDesc.format > Format::UNKNOWN && textureViewDesc.format < Format::MAX_NUM, Result::INVALID_ARGUMENT,
        "'textureViewDesc.format' is invalid");

    const TextureDesc& textureDesc = ((TextureVal*)textureViewDesc.texture)->GetDesc();

    RETURN_ON_FAILURE(this, textureViewDesc.mipOffset < textureDesc.mipNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.mipOffset' is invalid. "
        "(textureViewDesc.mipOffset=%hu, textureDesc.mipNum=%hu)",
        textureViewDesc.mipOffset, textureDesc.mipNum);

    RETURN_ON_FAILURE(this, textureViewDesc.mipOffset + textureViewDesc.mipNum <= textureDesc.mipNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.mipNum' is invalid. "
        "(textureViewDesc.mipOffset=%hu, textureViewDesc.mipNum=%hu, textureDesc.mipNum=%hu)",
        textureViewDesc.mipOffset, textureViewDesc.mipNum, textureDesc.mipNum);

    RETURN_ON_FAILURE(this, textureViewDesc.layerOffset < textureDesc.layerNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.layerOffset' is invalid. "
        "(textureViewDesc.layerOffset=%hu, textureDesc.layerNum=%hu)",
        textureViewDesc.layerOffset, textureDesc.layerNum);

    RETURN_ON_FAILURE(this, textureViewDesc.layerOffset + textureViewDesc.layerNum <= textureDesc.layerNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.layerNum' is invalid. "
        "(textureViewDesc.layerOffset=%hu, textureViewDesc.layerNum=%hu, textureDesc.layerNum=%hu)",
        textureViewDesc.layerOffset, textureViewDesc.layerNum, textureDesc.layerNum);

    auto textureViewDescImpl = textureViewDesc;
    textureViewDescImpl.texture = NRI_GET_IMPL(Texture, textureViewDesc.texture);

    Descriptor* descriptorImpl = nullptr;
    Result result = m_CoreAPI.CreateTexture2DView(textureViewDescImpl, descriptorImpl);

    if (result == Result::SUCCESS)
        textureView = (Descriptor*)Allocate<DescriptorVal>(GetStdAllocator(), *this, descriptorImpl, textureViewDesc);

    return result;
}

Result DeviceVal::CreateDescriptor(const Texture3DViewDesc& textureViewDesc, Descriptor*& textureView) {
    RETURN_ON_FAILURE(this, textureViewDesc.texture != nullptr, Result::INVALID_ARGUMENT, "'textureViewDesc.texture' is NULL");
    RETURN_ON_FAILURE(this, textureViewDesc.viewType < Texture3DViewType::MAX_NUM, Result::INVALID_ARGUMENT, "'textureViewDesc.viewType' is invalid");

    RETURN_ON_FAILURE(this, textureViewDesc.format > Format::UNKNOWN && textureViewDesc.format < Format::MAX_NUM, Result::INVALID_ARGUMENT,
        "'textureViewDesc.format' is invalid");

    const TextureDesc& textureDesc = ((TextureVal*)textureViewDesc.texture)->GetDesc();

    RETURN_ON_FAILURE(this, textureViewDesc.mipOffset < textureDesc.mipNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.mipOffset' is invalid. "
        "(textureViewDesc.mipOffset=%hu, textureViewDesc.mipOffset=%hu)",
        textureViewDesc.mipOffset, textureDesc.mipNum);

    RETURN_ON_FAILURE(this, textureViewDesc.mipOffset + textureViewDesc.mipNum <= textureDesc.mipNum, Result::INVALID_ARGUMENT,
        "'textureViewDesc.mipNum' is invalid. "
        "(textureViewDesc.mipOffset=%hu, textureViewDesc.mipNum=%hu, textureDesc.mipNum=%hu)",
        textureViewDesc.mipOffset, textureViewDesc.mipNum, textureDesc.mipNum);

    RETURN_ON_FAILURE(this, textureViewDesc.sliceOffset < textureDesc.depth, Result::INVALID_ARGUMENT,
        "'textureViewDesc.layerOffset' is invalid. "
        "(textureViewDesc.sliceOffset=%hu, textureDesc.depth=%hu)",
        textureViewDesc.sliceOffset, textureDesc.depth);

    RETURN_ON_FAILURE(this, textureViewDesc.sliceOffset + textureViewDesc.sliceNum <= textureDesc.depth, Result::INVALID_ARGUMENT,
        "'textureViewDesc.layerNum' is invalid. "
        "(textureViewDesc.sliceOffset=%hu, textureViewDesc.sliceNum=%hu, textureDesc.depth=%hu)",
        textureViewDesc.sliceOffset, textureViewDesc.sliceNum, textureDesc.depth);

    auto textureViewDescImpl = textureViewDesc;
    textureViewDescImpl.texture = NRI_GET_IMPL(Texture, textureViewDesc.texture);

    Descriptor* descriptorImpl = nullptr;
    Result result = m_CoreAPI.CreateTexture3DView(textureViewDescImpl, descriptorImpl);

    if (result == Result::SUCCESS)
        textureView = (Descriptor*)Allocate<DescriptorVal>(GetStdAllocator(), *this, descriptorImpl, textureViewDesc);

    return result;
}

Result DeviceVal::CreateDescriptor(const SamplerDesc& samplerDesc, Descriptor*& sampler) {
    RETURN_ON_FAILURE(this, samplerDesc.filters.mag < Filter::MAX_NUM, Result::INVALID_ARGUMENT, "'samplerDesc.filters.mag' is invalid");
    RETURN_ON_FAILURE(this, samplerDesc.filters.min < Filter::MAX_NUM, Result::INVALID_ARGUMENT, "'samplerDesc.filters.min' is invalid");
    RETURN_ON_FAILURE(this, samplerDesc.filters.mip < Filter::MAX_NUM, Result::INVALID_ARGUMENT, "'samplerDesc.filters.mip' is invalid");
    RETURN_ON_FAILURE(this, samplerDesc.filters.ext < FilterExt::MAX_NUM, Result::INVALID_ARGUMENT, "'samplerDesc.filters.ext' is invalid");
    RETURN_ON_FAILURE(this, samplerDesc.addressModes.u < AddressMode::MAX_NUM, Result::INVALID_ARGUMENT, "'samplerDesc.addressModes.u' is invalid");
    RETURN_ON_FAILURE(this, samplerDesc.addressModes.v < AddressMode::MAX_NUM, Result::INVALID_ARGUMENT, "'samplerDesc.addressModes.v' is invalid");
    RETURN_ON_FAILURE(this, samplerDesc.addressModes.w < AddressMode::MAX_NUM, Result::INVALID_ARGUMENT, "'samplerDesc.addressModes.w' is invalid");
    RETURN_ON_FAILURE(this, samplerDesc.compareFunc < CompareFunc::MAX_NUM, Result::INVALID_ARGUMENT, "'samplerDesc.compareFunc' is invalid");
    RETURN_ON_FAILURE(this, samplerDesc.borderColor < BorderColor::MAX_NUM, Result::INVALID_ARGUMENT, "'samplerDesc.borderColor' is invalid");

    if (!GetDesc().isTextureFilterMinMaxSupported)
        RETURN_ON_FAILURE(this, samplerDesc.filters.ext == FilterExt::NONE, Result::INVALID_ARGUMENT, "'isTextureFilterMinMaxSupported' is unsupported");

    Descriptor* samplerImpl = nullptr;
    Result result = m_CoreAPI.CreateSampler(m_Device, samplerDesc, samplerImpl);

    if (result == Result::SUCCESS)
        sampler = (Descriptor*)Allocate<DescriptorVal>(GetStdAllocator(), *this, samplerImpl);

    return result;
}

Result DeviceVal::CreatePipelineLayout(const PipelineLayoutDesc& pipelineLayoutDesc, PipelineLayout*& pipelineLayout) {
    const bool isGraphics = pipelineLayoutDesc.shaderStages & StageBits::GRAPHICS_SHADERS;
    const bool isCompute = pipelineLayoutDesc.shaderStages & StageBits::COMPUTE_SHADER;
    const bool isRayTracing = pipelineLayoutDesc.shaderStages & StageBits::RAY_TRACING_SHADERS;
    const uint32_t supportedTypes = (uint32_t)isGraphics + (uint32_t)isCompute + (uint32_t)isRayTracing;

    RETURN_ON_FAILURE(this, pipelineLayoutDesc.shaderStages != StageBits::NONE, Result::INVALID_ARGUMENT, "'pipelineLayoutDesc.shaderStages' can't be NONE");
    RETURN_ON_FAILURE(this, supportedTypes > 0, Result::INVALID_ARGUMENT, "'pipelineLayoutDesc.shaderStages' doesn't include any shader stages");
    RETURN_ON_FAILURE(this, supportedTypes == 1, Result::INVALID_ARGUMENT,
        "'pipelineLayoutDesc.shaderStages' is invalid, it can't be compatible with more than one type of pipeline");

    for (uint32_t i = 0; i < pipelineLayoutDesc.descriptorSetNum; i++) {
        const DescriptorSetDesc& descriptorSetDesc = pipelineLayoutDesc.descriptorSets[i];

        for (uint32_t j = 0; j < descriptorSetDesc.rangeNum; j++) {
            const DescriptorRangeDesc& range = descriptorSetDesc.ranges[j];

            RETURN_ON_FAILURE(this, !range.isDescriptorNumVariable || range.isArray, Result::INVALID_ARGUMENT,
                "'pipelineLayoutDesc.descriptorSets[%u].ranges[%u]' is invalid, "
                "'isArray' can't be false if 'isDescriptorNumVariable' is true",
                i, j);

            RETURN_ON_FAILURE(this, range.descriptorNum > 0, Result::INVALID_ARGUMENT, "'pipelineLayoutDesc.descriptorSets[%u].ranges[%u].descriptorNum' is 0", i, j);
            RETURN_ON_FAILURE(this, range.descriptorType < DescriptorType::MAX_NUM, Result::INVALID_ARGUMENT, "'pipelineLayoutDesc.descriptorSets[%u].ranges[%u].descriptorType' is invalid", i, j);

            if (range.shaderStages != StageBits::ALL) {
                const uint32_t filteredVisibilityMask = range.shaderStages & pipelineLayoutDesc.shaderStages;

                RETURN_ON_FAILURE(this, (uint32_t)range.shaderStages == filteredVisibilityMask, Result::INVALID_ARGUMENT,
                    "'pipelineLayoutDesc.descriptorSets[%u].ranges[%u].shaderStages' is not "
                    "compatible with 'pipelineLayoutDesc.shaderStages'",
                    i, j);
            }
        }
    }

    PipelineLayout* pipelineLayoutImpl = nullptr;
    Result result = m_CoreAPI.CreatePipelineLayout(m_Device, pipelineLayoutDesc, pipelineLayoutImpl);

    if (result == Result::SUCCESS)
        pipelineLayout = (PipelineLayout*)Allocate<PipelineLayoutVal>(GetStdAllocator(), *this, pipelineLayoutImpl, pipelineLayoutDesc);

    return result;
}

Result DeviceVal::CreatePipeline(const GraphicsPipelineDesc& graphicsPipelineDesc, Pipeline*& pipeline) {
    RETURN_ON_FAILURE(this, graphicsPipelineDesc.pipelineLayout != nullptr, Result::INVALID_ARGUMENT, "'graphicsPipelineDesc.pipelineLayout' is NULL");
    RETURN_ON_FAILURE(this, graphicsPipelineDesc.shaders != nullptr, Result::INVALID_ARGUMENT, "'graphicsPipelineDesc.shaders' is NULL");
    RETURN_ON_FAILURE(this, graphicsPipelineDesc.shaderNum > 0, Result::INVALID_ARGUMENT, "'graphicsPipelineDesc.shaderNum' is 0");

    const PipelineLayoutVal& pipelineLayout = *(PipelineLayoutVal*)graphicsPipelineDesc.pipelineLayout;
    const StageBits shaderStages = pipelineLayout.GetPipelineLayoutDesc().shaderStages;
    bool hasEntryPoint = false;
    uint32_t uniqueShaderStages = 0;
    for (uint32_t i = 0; i < graphicsPipelineDesc.shaderNum; i++) {
        const ShaderDesc* shaderDesc = graphicsPipelineDesc.shaders + i;
        if (shaderDesc->stage == StageBits::VERTEX_SHADER || shaderDesc->stage == StageBits::MESH_CONTROL_SHADER)
            hasEntryPoint = true;

        RETURN_ON_FAILURE(this, shaderDesc->stage & shaderStages, Result::INVALID_ARGUMENT, "'graphicsPipelineDesc.shaders[%u].stage' is not enabled in the pipeline layout", i);
        RETURN_ON_FAILURE(this, shaderDesc->bytecode != nullptr, Result::INVALID_ARGUMENT, "'graphicsPipelineDesc.shaders[%u].bytecode' is invalid", i);
        RETURN_ON_FAILURE(this, shaderDesc->size != 0, Result::INVALID_ARGUMENT, "'graphicsPipelineDesc.shaders[%u].size' is 0", i);
        RETURN_ON_FAILURE(this, IsShaderStageValid(shaderDesc->stage, uniqueShaderStages, StageBits::GRAPHICS_SHADERS), Result::INVALID_ARGUMENT,
            "'graphicsPipelineDesc.shaders[%u].stage' must include only 1 graphics shader stage, unique for the entire pipeline", i);
    }
    RETURN_ON_FAILURE(this, hasEntryPoint, Result::INVALID_ARGUMENT, "a VERTEX or MESH_CONTROL shader is not provided");

    for (uint32_t i = 0; i < graphicsPipelineDesc.outputMerger.colorNum; i++) {
        const ColorAttachmentDesc* color = graphicsPipelineDesc.outputMerger.color + i;
        RETURN_ON_FAILURE(this, color->format > Format::UNKNOWN && color->format < Format::BC1_RGBA_UNORM, Result::INVALID_ARGUMENT, "'graphicsPipelineDesc.outputMerger->color[%u].format = %u' is invalid", i, color->format);
    }

    if (graphicsPipelineDesc.vertexInput) {
        for (uint32_t i = 0; i < graphicsPipelineDesc.vertexInput->attributeNum; i++) {
            const VertexAttributeDesc* attribute = graphicsPipelineDesc.vertexInput->attributes + i;
            uint32_t size = GetFormatProps(attribute->format).stride;
            uint32_t stride = graphicsPipelineDesc.vertexInput->streams[attribute->streamIndex].stride;
            RETURN_ON_FAILURE(this, attribute->offset + size <= stride, Result::INVALID_ARGUMENT,
                "'graphicsPipelineDesc.inputAssembly->attributes[%u]' is out of bounds of 'graphicsPipelineDesc.inputAssembly->streams[%u]' (stride = %u)", i, attribute->streamIndex, stride);
        }
    }

    auto graphicsPipelineDescImpl = graphicsPipelineDesc;
    graphicsPipelineDescImpl.pipelineLayout = NRI_GET_IMPL(PipelineLayout, graphicsPipelineDesc.pipelineLayout);

    Pipeline* pipelineImpl = nullptr;
    Result result = m_CoreAPI.CreateGraphicsPipeline(m_Device, graphicsPipelineDescImpl, pipelineImpl);

    if (result == Result::SUCCESS)
        pipeline = (Pipeline*)Allocate<PipelineVal>(GetStdAllocator(), *this, pipelineImpl, graphicsPipelineDesc);

    return result;
}

Result DeviceVal::CreatePipeline(const ComputePipelineDesc& computePipelineDesc, Pipeline*& pipeline) {
    RETURN_ON_FAILURE(this, computePipelineDesc.pipelineLayout != nullptr, Result::INVALID_ARGUMENT, "'computePipelineDesc.pipelineLayout' is NULL");
    RETURN_ON_FAILURE(this, computePipelineDesc.shader.size != 0, Result::INVALID_ARGUMENT, "'computePipelineDesc.shader.size' is 0");
    RETURN_ON_FAILURE(this, computePipelineDesc.shader.bytecode != nullptr, Result::INVALID_ARGUMENT, "'computePipelineDesc.shader.bytecode' is NULL");
    RETURN_ON_FAILURE(this, computePipelineDesc.shader.stage == StageBits::COMPUTE_SHADER, Result::INVALID_ARGUMENT, "'computePipelineDesc.shader.stage' must be 'StageBits::COMPUTE_SHADER'");

    auto computePipelineDescImpl = computePipelineDesc;
    computePipelineDescImpl.pipelineLayout = NRI_GET_IMPL(PipelineLayout, computePipelineDesc.pipelineLayout);

    Pipeline* pipelineImpl = nullptr;
    Result result = m_CoreAPI.CreateComputePipeline(m_Device, computePipelineDescImpl, pipelineImpl);

    if (result == Result::SUCCESS)
        pipeline = (Pipeline*)Allocate<PipelineVal>(GetStdAllocator(), *this, pipelineImpl, computePipelineDesc);

    return result;
}

Result DeviceVal::CreateQueryPool(const QueryPoolDesc& queryPoolDesc, QueryPool*& queryPool) {
    RETURN_ON_FAILURE(this, queryPoolDesc.queryType < QueryType::MAX_NUM, Result::INVALID_ARGUMENT, "'queryPoolDesc.queryType' is invalid");
    RETURN_ON_FAILURE(this, queryPoolDesc.capacity > 0, Result::INVALID_ARGUMENT, "'queryPoolDesc.capacity' is 0");

    QueryPool* queryPoolImpl = nullptr;
    Result result = m_CoreAPI.CreateQueryPool(m_Device, queryPoolDesc, queryPoolImpl);

    if (result == Result::SUCCESS)
        queryPool = (QueryPool*)Allocate<QueryPoolVal>(GetStdAllocator(), *this, queryPoolImpl, queryPoolDesc.queryType, queryPoolDesc.capacity);

    return result;
}

Result DeviceVal::CreateFence(uint64_t initialValue, Fence*& fence) {
    Fence* fenceImpl;
    Result result = m_CoreAPI.CreateFence(m_Device, initialValue, fenceImpl);

    if (result == Result::SUCCESS)
        fence = (Fence*)Allocate<FenceVal>(GetStdAllocator(), *this, fenceImpl);

    return result;
}

void DeviceVal::DestroyCommandBuffer(CommandBuffer& commandBuffer) {
    m_CoreAPI.DestroyCommandBuffer(*NRI_GET_IMPL(CommandBuffer, &commandBuffer));
    Destroy(GetStdAllocator(), (CommandBufferVal*)&commandBuffer);
}

void DeviceVal::DestroyCommandAllocator(CommandAllocator& commandAllocator) {
    m_CoreAPI.DestroyCommandAllocator(*NRI_GET_IMPL(CommandAllocator, &commandAllocator));
    Destroy(GetStdAllocator(), (CommandAllocatorVal*)&commandAllocator);
}

void DeviceVal::DestroyDescriptorPool(DescriptorPool& descriptorPool) {
    m_CoreAPI.DestroyDescriptorPool(*NRI_GET_IMPL(DescriptorPool, &descriptorPool));
    Destroy(GetStdAllocator(), (DescriptorPoolVal*)&descriptorPool);
}

void DeviceVal::DestroyBuffer(Buffer& buffer) {
    m_CoreAPI.DestroyBuffer(*NRI_GET_IMPL(Buffer, &buffer));
    Destroy(GetStdAllocator(), (BufferVal*)&buffer);
}

void DeviceVal::DestroyTexture(Texture& texture) {
    m_CoreAPI.DestroyTexture(*NRI_GET_IMPL(Texture, &texture));
    Destroy(GetStdAllocator(), (TextureVal*)&texture);
}

void DeviceVal::DestroyDescriptor(Descriptor& descriptor) {
    m_CoreAPI.DestroyDescriptor(*NRI_GET_IMPL(Descriptor, &descriptor));
    Destroy(GetStdAllocator(), (DescriptorVal*)&descriptor);
}

void DeviceVal::DestroyPipelineLayout(PipelineLayout& pipelineLayout) {
    m_CoreAPI.DestroyPipelineLayout(*NRI_GET_IMPL(PipelineLayout, &pipelineLayout));
    Destroy(GetStdAllocator(), (PipelineLayoutVal*)&pipelineLayout);
}

void DeviceVal::DestroyPipeline(Pipeline& pipeline) {
    m_CoreAPI.DestroyPipeline(*NRI_GET_IMPL(Pipeline, &pipeline));
    Destroy(GetStdAllocator(), (PipelineVal*)&pipeline);
}

void DeviceVal::DestroyQueryPool(QueryPool& queryPool) {
    m_CoreAPI.DestroyQueryPool(*NRI_GET_IMPL(QueryPool, &queryPool));
    Destroy(GetStdAllocator(), (QueryPoolVal*)&queryPool);
}

void DeviceVal::DestroyFence(Fence& fence) {
    m_CoreAPI.DestroyFence(*NRI_GET_IMPL(Fence, &fence));
    Destroy(GetStdAllocator(), (FenceVal*)&fence);
}

Result DeviceVal::AllocateMemory(const AllocateMemoryDesc& allocateMemoryDesc, Memory*& memory) {
    RETURN_ON_FAILURE(this, allocateMemoryDesc.size > 0, Result::INVALID_ARGUMENT, "'allocateMemoryDesc.size' is 0");
    RETURN_ON_FAILURE(this, allocateMemoryDesc.priority >= -1.0f && allocateMemoryDesc.priority <= 1.0f, Result::INVALID_ARGUMENT, "'allocateMemoryDesc.priority' outside of [-1; 1] range");

    std::unordered_map<MemoryType, MemoryLocation>::iterator it;
    std::unordered_map<MemoryType, MemoryLocation>::iterator end;
    {
        ExclusiveScope lockScope(m_Lock);
        it = m_MemoryTypeMap.find(allocateMemoryDesc.type);
        end = m_MemoryTypeMap.end();
    }

    RETURN_ON_FAILURE(this, it != end, Result::FAILURE, "'memoryType' is invalid");

    Memory* memoryImpl;
    Result result = m_CoreAPI.AllocateMemory(m_Device, allocateMemoryDesc, memoryImpl);

    if (result == Result::SUCCESS)
        memory = (Memory*)Allocate<MemoryVal>(GetStdAllocator(), *this, memoryImpl, allocateMemoryDesc.size, it->second);

    return result;
}

Result DeviceVal::BindBufferMemory(const BufferMemoryBindingDesc* memoryBindingDescs, uint32_t memoryBindingDescNum) {
    RETURN_ON_FAILURE(this, memoryBindingDescs != nullptr, Result::INVALID_ARGUMENT, "'memoryBindingDescs' is NULL");

    BufferMemoryBindingDesc* memoryBindingDescsImpl = StackAlloc(BufferMemoryBindingDesc, memoryBindingDescNum);

    for (uint32_t i = 0; i < memoryBindingDescNum; i++) {
        BufferMemoryBindingDesc& destDesc = memoryBindingDescsImpl[i];
        const BufferMemoryBindingDesc& srcDesc = memoryBindingDescs[i];

        RETURN_ON_FAILURE(this, srcDesc.buffer != nullptr, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].buffer' is NULL", i);
        RETURN_ON_FAILURE(this, srcDesc.memory != nullptr, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].memory' is NULL", i);

        MemoryVal& memory = (MemoryVal&)*srcDesc.memory;
        BufferVal& buffer = (BufferVal&)*srcDesc.buffer;

        RETURN_ON_FAILURE(this, !buffer.IsBoundToMemory(), Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].buffer' is already bound to memory", i);

        destDesc = srcDesc;
        destDesc.memory = memory.GetImpl();
        destDesc.buffer = buffer.GetImpl();

        // Skip validation if memory has been created from GAPI object using a wrapper extension
        if (memory.GetMemoryLocation() == MemoryLocation::MAX_NUM)
            continue;

        MemoryDesc memoryDesc = {};
        GetCoreInterface().GetBufferMemoryDesc(GetImpl(), buffer.GetDesc(), memory.GetMemoryLocation(), memoryDesc);

        RETURN_ON_FAILURE(this, !memoryDesc.mustBeDedicated || srcDesc.offset == 0, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].offset' must be zero for dedicated allocation", i);
        RETURN_ON_FAILURE(this, memoryDesc.alignment != 0, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].alignment' is 0", i);
        RETURN_ON_FAILURE(this, srcDesc.offset % memoryDesc.alignment == 0, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].offset' is misaligned", i);

        const uint64_t rangeMax = srcDesc.offset + memoryDesc.size;
        const bool memorySizeIsUnknown = memory.GetSize() == 0;

        RETURN_ON_FAILURE(this, memorySizeIsUnknown || rangeMax <= memory.GetSize(), Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].offset' is invalid", i);
    }

    Result result = m_CoreAPI.BindBufferMemory(m_Device, memoryBindingDescsImpl, memoryBindingDescNum);

    if (result == Result::SUCCESS) {
        for (uint32_t i = 0; i < memoryBindingDescNum; i++) {
            MemoryVal& memory = *(MemoryVal*)memoryBindingDescs[i].memory;
            memory.BindBuffer(*(BufferVal*)memoryBindingDescs[i].buffer);
        }
    }

    return result;
}

Result DeviceVal::BindTextureMemory(const TextureMemoryBindingDesc* memoryBindingDescs, uint32_t memoryBindingDescNum) {
    RETURN_ON_FAILURE(this, memoryBindingDescs != nullptr, Result::INVALID_ARGUMENT, "'memoryBindingDescs' is a NULL");

    TextureMemoryBindingDesc* memoryBindingDescsImpl = StackAlloc(TextureMemoryBindingDesc, memoryBindingDescNum);

    for (uint32_t i = 0; i < memoryBindingDescNum; i++) {
        TextureMemoryBindingDesc& destDesc = memoryBindingDescsImpl[i];
        const TextureMemoryBindingDesc& srcDesc = memoryBindingDescs[i];

        RETURN_ON_FAILURE(this, srcDesc.texture != nullptr, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].texture' is NULL", i);
        RETURN_ON_FAILURE(this, srcDesc.memory != nullptr, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].memory' is NULL", i);

        MemoryVal& memory = (MemoryVal&)*srcDesc.memory;
        TextureVal& texture = (TextureVal&)*srcDesc.texture;

        RETURN_ON_FAILURE(this, !texture.IsBoundToMemory(), Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].texture' is already bound to memory", i);

        destDesc = srcDesc;
        destDesc.memory = memory.GetImpl();
        destDesc.texture = texture.GetImpl();

        // Skip validation if memory has been created from GAPI object using a wrapper extension
        if (memory.GetMemoryLocation() == MemoryLocation::MAX_NUM)
            continue;

        MemoryDesc memoryDesc = {};
        GetCoreInterface().GetTextureMemoryDesc(GetImpl(), texture.GetDesc(), memory.GetMemoryLocation(), memoryDesc);

        RETURN_ON_FAILURE(this, !memoryDesc.mustBeDedicated || srcDesc.offset == 0, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].offset' must be zero for dedicated allocation", i);
        RETURN_ON_FAILURE(this, memoryDesc.alignment != 0, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].alignment' is 0", i);
        RETURN_ON_FAILURE(this, srcDesc.offset % memoryDesc.alignment == 0, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].offset' is misaligned", i);

        const uint64_t rangeMax = srcDesc.offset + memoryDesc.size;
        const bool memorySizeIsUnknown = memory.GetSize() == 0;

        RETURN_ON_FAILURE(this, memorySizeIsUnknown || rangeMax <= memory.GetSize(), Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].offset' is invalid", i);
    }

    Result result = m_CoreAPI.BindTextureMemory(m_Device, memoryBindingDescsImpl, memoryBindingDescNum);

    if (result == Result::SUCCESS) {
        for (uint32_t i = 0; i < memoryBindingDescNum; i++) {
            MemoryVal& memory = *(MemoryVal*)memoryBindingDescs[i].memory;
            memory.BindTexture(*(TextureVal*)memoryBindingDescs[i].texture);
        }
    }

    return result;
}

void DeviceVal::FreeMemory(Memory& memory) {
    MemoryVal& memoryVal = (MemoryVal&)memory;

    if (memoryVal.HasBoundResources()) {
        memoryVal.ReportBoundResources();
        REPORT_ERROR(this, "FreeMemory: some resources are still bound to the memory");
        return;
    }

    m_CoreAPI.FreeMemory(*NRI_GET_IMPL(Memory, &memory));
    Destroy(GetStdAllocator(), (MemoryVal*)&memory);
}

FormatSupportBits DeviceVal::GetFormatSupport(Format format) const {
    return m_CoreAPI.GetFormatSupport(m_Device, format);
}

#if NRI_USE_VULKAN

Result DeviceVal::CreateCommandQueue(const CommandQueueVKDesc& commandQueueVKDesc, CommandQueue*& commandQueue) {
    RETURN_ON_FAILURE(this, commandQueueVKDesc.vkQueue != 0, Result::INVALID_ARGUMENT, "'commandQueueVKDesc.vkQueue' is NULL");
    RETURN_ON_FAILURE(this, commandQueueVKDesc.commandQueueType < CommandQueueType::MAX_NUM, Result::INVALID_ARGUMENT, "'commandQueueVKDesc.commandQueueType' is invalid");

    CommandQueue* commandQueueImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateCommandQueueVK(m_Device, commandQueueVKDesc, commandQueueImpl);

    if (result == Result::SUCCESS)
        commandQueue = (CommandQueue*)Allocate<CommandQueueVal>(GetStdAllocator(), *this, commandQueueImpl);

    return result;
}

Result DeviceVal::CreateCommandAllocator(const CommandAllocatorVKDesc& commandAllocatorVKDesc, CommandAllocator*& commandAllocator) {
    RETURN_ON_FAILURE(this, commandAllocatorVKDesc.vkCommandPool != 0, Result::INVALID_ARGUMENT, "'commandAllocatorVKDesc.vkCommandPool' is NULL");
    RETURN_ON_FAILURE(this, commandAllocatorVKDesc.commandQueueType < CommandQueueType::MAX_NUM, Result::INVALID_ARGUMENT, "'commandAllocatorVKDesc.commandQueueType' is invalid");

    CommandAllocator* commandAllocatorImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateCommandAllocatorVK(m_Device, commandAllocatorVKDesc, commandAllocatorImpl);

    if (result == Result::SUCCESS)
        commandAllocator = (CommandAllocator*)Allocate<CommandAllocatorVal>(GetStdAllocator(), *this, commandAllocatorImpl);

    return result;
}

Result DeviceVal::CreateCommandBuffer(const CommandBufferVKDesc& commandBufferVKDesc, CommandBuffer*& commandBuffer) {
    RETURN_ON_FAILURE(this, commandBufferVKDesc.vkCommandBuffer != 0, Result::INVALID_ARGUMENT, "'commandBufferVKDesc.vkCommandBuffer' is NULL");
    RETURN_ON_FAILURE(this, commandBufferVKDesc.commandQueueType < CommandQueueType::MAX_NUM, Result::INVALID_ARGUMENT, "'commandBufferVKDesc.commandQueueType' is invalid");

    CommandBuffer* commandBufferImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateCommandBufferVK(m_Device, commandBufferVKDesc, commandBufferImpl);

    if (result == Result::SUCCESS)
        commandBuffer = (CommandBuffer*)Allocate<CommandBufferVal>(GetStdAllocator(), *this, commandBufferImpl, true);

    return result;
}

Result DeviceVal::CreateDescriptorPool(const DescriptorPoolVKDesc& descriptorPoolVKDesc, DescriptorPool*& descriptorPool) {
    RETURN_ON_FAILURE(this, descriptorPoolVKDesc.vkDescriptorPool != 0, Result::INVALID_ARGUMENT, "'vkDescriptorPool' is NULL");
    RETURN_ON_FAILURE(this, descriptorPoolVKDesc.descriptorSetMaxNum != 0, Result::INVALID_ARGUMENT, "'descriptorSetMaxNum' is 0");

    DescriptorPool* descriptorPoolImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateDescriptorPoolVK(m_Device, descriptorPoolVKDesc, descriptorPoolImpl);

    if (result == Result::SUCCESS)
        descriptorPool = (DescriptorPool*)Allocate<DescriptorPoolVal>(GetStdAllocator(), *this, descriptorPoolImpl, descriptorPoolVKDesc.descriptorSetMaxNum);

    return result;
}

Result DeviceVal::CreateBuffer(const BufferVKDesc& bufferDesc, Buffer*& buffer) {
    RETURN_ON_FAILURE(this, bufferDesc.vkBuffer != 0, Result::INVALID_ARGUMENT, "'bufferDesc.vkBuffer' is NULL");
    RETURN_ON_FAILURE(this, bufferDesc.size > 0, Result::INVALID_ARGUMENT, "'bufferDesc.bufferSize' is 0");

    Buffer* bufferImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateBufferVK(m_Device, bufferDesc, bufferImpl);

    if (result == Result::SUCCESS)
        buffer = (Buffer*)Allocate<BufferVal>(GetStdAllocator(), *this, bufferImpl, true);

    return result;
}

Result DeviceVal::CreateTexture(const TextureVKDesc& textureVKDesc, Texture*& texture) {
    RETURN_ON_FAILURE(this, textureVKDesc.vkImage != 0, Result::INVALID_ARGUMENT, "'textureVKDesc.vkImage' is NULL");
    RETURN_ON_FAILURE(this, nriConvertVKFormatToNRI(textureVKDesc.vkFormat) != Format::UNKNOWN, Result::INVALID_ARGUMENT, "'textureVKDesc.sampleNum' is 0");
    RETURN_ON_FAILURE(this, textureVKDesc.sampleNum > 0, Result::INVALID_ARGUMENT, "'textureVKDesc.sampleNum' is 0");
    RETURN_ON_FAILURE(this, textureVKDesc.layerNum > 0, Result::INVALID_ARGUMENT, "'textureVKDesc.layerNum' is 0");
    RETURN_ON_FAILURE(this, textureVKDesc.mipNum > 0, Result::INVALID_ARGUMENT, "'textureVKDesc.mipNum' is 0");

    Texture* textureImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateTextureVK(m_Device, textureVKDesc, textureImpl);

    if (result == Result::SUCCESS)
        texture = (Texture*)Allocate<TextureVal>(GetStdAllocator(), *this, textureImpl, true);

    return result;
}

Result DeviceVal::CreateMemory(const MemoryVKDesc& memoryVKDesc, Memory*& memory) {
    RETURN_ON_FAILURE(this, memoryVKDesc.vkDeviceMemory != 0, Result::INVALID_ARGUMENT, "'memoryVKDesc.vkDeviceMemory' is NULL");
    RETURN_ON_FAILURE(this, memoryVKDesc.size > 0, Result::INVALID_ARGUMENT, "'memoryVKDesc.size' is 0");

    Memory* memoryImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateMemoryVK(m_Device, memoryVKDesc, memoryImpl);

    if (result == Result::SUCCESS)
        memory = (Memory*)Allocate<MemoryVal>(GetStdAllocator(), *this, memoryImpl, memoryVKDesc.size, MemoryLocation::MAX_NUM);

    return result;
}

Result DeviceVal::CreateGraphicsPipeline(VKNonDispatchableHandle vkPipeline, Pipeline*& pipeline) {
    RETURN_ON_FAILURE(this, vkPipeline != 0, Result::INVALID_ARGUMENT, "'vkPipeline' is NULL");

    Pipeline* pipelineImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateGraphicsPipelineVK(m_Device, vkPipeline, pipelineImpl);

    if (result == Result::SUCCESS)
        pipeline = (Pipeline*)Allocate<PipelineVal>(GetStdAllocator(), *this, pipelineImpl);

    return result;
}

Result DeviceVal::CreateComputePipeline(VKNonDispatchableHandle vkPipeline, Pipeline*& pipeline) {
    RETURN_ON_FAILURE(this, vkPipeline != 0, Result::INVALID_ARGUMENT, "'vkPipeline' is NULL");

    Pipeline* pipelineImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateComputePipelineVK(m_Device, vkPipeline, pipelineImpl);

    if (result == Result::SUCCESS)
        pipeline = (Pipeline*)Allocate<PipelineVal>(GetStdAllocator(), *this, pipelineImpl);

    return result;
}

Result DeviceVal::CreateQueryPool(const QueryPoolVKDesc& queryPoolVKDesc, QueryPool*& queryPool) {
    RETURN_ON_FAILURE(this, queryPoolVKDesc.vkQueryPool != 0, Result::INVALID_ARGUMENT, "'queryPoolVKDesc.vkQueryPool' is NULL");

    QueryPool* queryPoolImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateQueryPoolVK(m_Device, queryPoolVKDesc, queryPoolImpl);

    if (result == Result::SUCCESS) {
        QueryType queryType = GetQueryTypeVK(queryPoolVKDesc.vkQueryType);
        queryPool = (QueryPool*)Allocate<QueryPoolVal>(GetStdAllocator(), *this, queryPoolImpl, queryType, 0);
    }

    return result;
}

Result DeviceVal::CreateAccelerationStructure(const AccelerationStructureVKDesc& accelerationStructureDesc, AccelerationStructure*& accelerationStructure) {
    RETURN_ON_FAILURE(this, accelerationStructureDesc.vkAccelerationStructure != 0, Result::INVALID_ARGUMENT, "'accelerationStructureDesc.vkAccelerationStructure' is NULL");

    AccelerationStructure* accelerationStructureImpl = nullptr;
    Result result = m_WrapperVKAPI.CreateAccelerationStructureVK(m_Device, accelerationStructureDesc, accelerationStructureImpl);

    if (result == Result::SUCCESS) {
        MemoryDesc memoryDesc = {};
        accelerationStructure = (AccelerationStructure*)Allocate<AccelerationStructureVal>(GetStdAllocator(), *this, accelerationStructureImpl, true, memoryDesc);
    }

    return result;
}

#endif

#if NRI_USE_D3D11

Result DeviceVal::CreateCommandBuffer(const CommandBufferD3D11Desc& commandBufferDesc, CommandBuffer*& commandBuffer) {
    RETURN_ON_FAILURE(this, commandBufferDesc.d3d11DeviceContext != nullptr, Result::INVALID_ARGUMENT, "'commandBufferDesc.d3d11DeviceContext' is NULL");

    CommandBuffer* commandBufferImpl = nullptr;
    Result result = m_WrapperD3D11API.CreateCommandBufferD3D11(m_Device, commandBufferDesc, commandBufferImpl);

    if (result == Result::SUCCESS)
        commandBuffer = (CommandBuffer*)Allocate<CommandBufferVal>(GetStdAllocator(), *this, commandBufferImpl, true);

    return result;
}

Result DeviceVal::CreateBuffer(const BufferD3D11Desc& bufferDesc, Buffer*& buffer) {
    RETURN_ON_FAILURE(this, bufferDesc.d3d11Resource != nullptr, Result::INVALID_ARGUMENT, "'bufferDesc.d3d11Resource' is NULL");

    Buffer* bufferImpl = nullptr;
    Result result = m_WrapperD3D11API.CreateBufferD3D11(m_Device, bufferDesc, bufferImpl);

    if (result == Result::SUCCESS)
        buffer = (Buffer*)Allocate<BufferVal>(GetStdAllocator(), *this, bufferImpl, true);

    return result;
}

Result DeviceVal::CreateTexture(const TextureD3D11Desc& textureDesc, Texture*& texture) {
    RETURN_ON_FAILURE(this, textureDesc.d3d11Resource != nullptr, Result::INVALID_ARGUMENT, "'textureDesc.d3d11Resource' is NULL");

    Texture* textureImpl = nullptr;
    Result result = m_WrapperD3D11API.CreateTextureD3D11(m_Device, textureDesc, textureImpl);

    if (result == Result::SUCCESS)
        texture = (Texture*)Allocate<TextureVal>(GetStdAllocator(), *this, textureImpl, true);

    return result;
}

#endif

#if NRI_USE_D3D12

Result DeviceVal::CreateCommandBuffer(const CommandBufferD3D12Desc& commandBufferDesc, CommandBuffer*& commandBuffer) {
    RETURN_ON_FAILURE(this, commandBufferDesc.d3d12CommandAllocator != nullptr, Result::INVALID_ARGUMENT, "'commandBufferDesc.d3d12CommandAllocator' is NULL");
    RETURN_ON_FAILURE(this, commandBufferDesc.d3d12CommandList != nullptr, Result::INVALID_ARGUMENT, "'commandBufferDesc.d3d12CommandList' is NULL");

    CommandBuffer* commandBufferImpl = nullptr;
    Result result = m_WrapperD3D12API.CreateCommandBufferD3D12(m_Device, commandBufferDesc, commandBufferImpl);

    if (result == Result::SUCCESS)
        commandBuffer = (CommandBuffer*)Allocate<CommandBufferVal>(GetStdAllocator(), *this, commandBufferImpl, true);

    return result;
}

Result DeviceVal::CreateDescriptorPool(const DescriptorPoolD3D12Desc& descriptorPoolD3D12Desc, DescriptorPool*& descriptorPool) {
    RETURN_ON_FAILURE(this, descriptorPoolD3D12Desc.d3d12ResourceDescriptorHeap || descriptorPoolD3D12Desc.d3d12SamplerDescriptorHeap,
        Result::INVALID_ARGUMENT, "'descriptorPoolD3D12Desc.d3d12ResourceDescriptorHeap' and 'descriptorPoolD3D12Desc.d3d12ResourceDescriptorHeap' are both NULL");

    DescriptorPool* descriptorPoolImpl = nullptr;
    Result result = m_WrapperD3D12API.CreateDescriptorPoolD3D12(m_Device, descriptorPoolD3D12Desc, descriptorPoolImpl);

    if (result == Result::SUCCESS)
        descriptorPool = (DescriptorPool*)Allocate<DescriptorPoolVal>(GetStdAllocator(), *this, descriptorPoolImpl, descriptorPoolD3D12Desc.descriptorSetMaxNum);

    return result;
}

Result DeviceVal::CreateBuffer(const BufferD3D12Desc& bufferDesc, Buffer*& buffer) {
    RETURN_ON_FAILURE(this, bufferDesc.d3d12Resource != nullptr, Result::INVALID_ARGUMENT, "'bufferDesc.d3d12Resource' is NULL");

    Buffer* bufferImpl = nullptr;
    Result result = m_WrapperD3D12API.CreateBufferD3D12(m_Device, bufferDesc, bufferImpl);

    if (result == Result::SUCCESS)
        buffer = (Buffer*)Allocate<BufferVal>(GetStdAllocator(), *this, bufferImpl, true);

    return result;
}

Result DeviceVal::CreateTexture(const TextureD3D12Desc& textureDesc, Texture*& texture) {
    RETURN_ON_FAILURE(this, textureDesc.d3d12Resource != nullptr, Result::INVALID_ARGUMENT, "'textureDesc.d3d12Resource' is NULL");

    Texture* textureImpl = nullptr;
    Result result = m_WrapperD3D12API.CreateTextureD3D12(m_Device, textureDesc, textureImpl);

    if (result == Result::SUCCESS)
        texture = (Texture*)Allocate<TextureVal>(GetStdAllocator(), *this, textureImpl, true);

    return result;
}

Result DeviceVal::CreateMemory(const MemoryD3D12Desc& memoryDesc, Memory*& memory) {
    RETURN_ON_FAILURE(this, memoryDesc.d3d12Heap != nullptr, Result::INVALID_ARGUMENT, "'memoryDesc.d3d12Heap' is NULL");

    Memory* memoryImpl = nullptr;
    Result result = m_WrapperD3D12API.CreateMemoryD3D12(m_Device, memoryDesc, memoryImpl);

    const uint64_t size = GetMemorySizeD3D12(memoryDesc);

    if (result == Result::SUCCESS)
        memory = (Memory*)Allocate<MemoryVal>(GetStdAllocator(), *this, memoryImpl, size, MemoryLocation::MAX_NUM);

    return result;
}

Result DeviceVal::CreateAccelerationStructure(const AccelerationStructureD3D12Desc& accelerationStructureDesc, AccelerationStructure*& accelerationStructure) {
    RETURN_ON_FAILURE(this, accelerationStructureDesc.d3d12Resource != nullptr, Result::INVALID_ARGUMENT, "'accelerationStructureDesc.d3d12Resource' is NULL");

    AccelerationStructure* accelerationStructureImpl = nullptr;
    Result result = m_WrapperD3D12API.CreateAccelerationStructureD3D12(m_Device, accelerationStructureDesc, accelerationStructureImpl);

    if (result == Result::SUCCESS) {
        MemoryDesc memoryDesc = {};
        accelerationStructure = (AccelerationStructure*)Allocate<AccelerationStructureVal>(GetStdAllocator(), *this, accelerationStructureImpl, true, memoryDesc);
    }

    return result;
}

#endif

uint32_t DeviceVal::CalculateAllocationNumber(const ResourceGroupDesc& resourceGroupDesc) const {
    RETURN_ON_FAILURE(this, resourceGroupDesc.memoryLocation < MemoryLocation::MAX_NUM, 0, "'resourceGroupDesc.memoryLocation' is invalid");
    RETURN_ON_FAILURE(this, resourceGroupDesc.bufferNum == 0 || resourceGroupDesc.buffers != nullptr, 0, "'resourceGroupDesc.buffers' is NULL");
    RETURN_ON_FAILURE(this, resourceGroupDesc.textureNum == 0 || resourceGroupDesc.textures != nullptr, 0, "'resourceGroupDesc.textures' is NULL");

    Buffer** buffersImpl = StackAlloc(Buffer*, resourceGroupDesc.bufferNum);

    for (uint32_t i = 0; i < resourceGroupDesc.bufferNum; i++) {
        RETURN_ON_FAILURE(this, resourceGroupDesc.buffers[i] != nullptr, 0, "'resourceGroupDesc.buffers[%u]' is NULL", i);

        BufferVal& bufferVal = *(BufferVal*)resourceGroupDesc.buffers[i];
        buffersImpl[i] = bufferVal.GetImpl();
    }

    Texture** texturesImpl = StackAlloc(Texture*, resourceGroupDesc.textureNum);

    for (uint32_t i = 0; i < resourceGroupDesc.textureNum; i++) {
        RETURN_ON_FAILURE(this, resourceGroupDesc.textures[i] != nullptr, 0, "'resourceGroupDesc.textures[%u]' is NULL", i);

        TextureVal& textureVal = *(TextureVal*)resourceGroupDesc.textures[i];
        texturesImpl[i] = textureVal.GetImpl();
    }

    ResourceGroupDesc resourceGroupDescImpl = resourceGroupDesc;
    resourceGroupDescImpl.buffers = buffersImpl;
    resourceGroupDescImpl.textures = texturesImpl;

    return m_HelperAPI.CalculateAllocationNumber(m_Device, resourceGroupDescImpl);
}

Result DeviceVal::AllocateAndBindMemory(const ResourceGroupDesc& resourceGroupDesc, Memory** allocations) {
    RETURN_ON_FAILURE(this, allocations != nullptr, Result::INVALID_ARGUMENT, "'allocations' is NULL");
    RETURN_ON_FAILURE(this, resourceGroupDesc.memoryLocation < MemoryLocation::MAX_NUM, Result::INVALID_ARGUMENT, "'resourceGroupDesc.memoryLocation' is invalid");
    RETURN_ON_FAILURE(this, resourceGroupDesc.bufferNum == 0 || resourceGroupDesc.buffers != nullptr, Result::INVALID_ARGUMENT, "'resourceGroupDesc.buffers' is NULL");
    RETURN_ON_FAILURE(this, resourceGroupDesc.textureNum == 0 || resourceGroupDesc.textures != nullptr, Result::INVALID_ARGUMENT, "'resourceGroupDesc.textures' is NULL");

    Buffer** buffersImpl = StackAlloc(Buffer*, resourceGroupDesc.bufferNum);

    for (uint32_t i = 0; i < resourceGroupDesc.bufferNum; i++) {
        RETURN_ON_FAILURE(this, resourceGroupDesc.buffers[i] != nullptr, Result::INVALID_ARGUMENT, "'resourceGroupDesc.buffers[%u]' is NULL", i);

        BufferVal& bufferVal = *(BufferVal*)resourceGroupDesc.buffers[i];
        buffersImpl[i] = bufferVal.GetImpl();
    }

    Texture** texturesImpl = StackAlloc(Texture*, resourceGroupDesc.textureNum);

    for (uint32_t i = 0; i < resourceGroupDesc.textureNum; i++) {
        RETURN_ON_FAILURE(this, resourceGroupDesc.textures[i] != nullptr, Result::INVALID_ARGUMENT, "'resourceGroupDesc.textures[%u]' is NULL", i);

        TextureVal& textureVal = *(TextureVal*)resourceGroupDesc.textures[i];
        texturesImpl[i] = textureVal.GetImpl();
    }

    const size_t allocationNum = CalculateAllocationNumber(resourceGroupDesc);

    ResourceGroupDesc resourceGroupDescImpl = resourceGroupDesc;
    resourceGroupDescImpl.buffers = buffersImpl;
    resourceGroupDescImpl.textures = texturesImpl;

    Result result = m_HelperAPI.AllocateAndBindMemory(m_Device, resourceGroupDescImpl, allocations);

    if (result == Result::SUCCESS) {
        for (uint32_t i = 0; i < resourceGroupDesc.bufferNum; i++) {
            BufferVal& bufferVal = *(BufferVal*)resourceGroupDesc.buffers[i];
            bufferVal.SetBoundToMemory();
        }

        for (uint32_t i = 0; i < resourceGroupDesc.textureNum; i++) {
            TextureVal& textureVal = *(TextureVal*)resourceGroupDesc.textures[i];
            textureVal.SetBoundToMemory();
        }

        for (uint32_t i = 0; i < allocationNum; i++)
            allocations[i] = (Memory*)Allocate<MemoryVal>(GetStdAllocator(), *this, allocations[i], 0, resourceGroupDesc.memoryLocation);
    }

    return result;
}

Result DeviceVal::QueryVideoMemoryInfo(MemoryLocation memoryLocation, VideoMemoryInfo& videoMemoryInfo) const {
    return m_HelperAPI.QueryVideoMemoryInfo(m_Device, memoryLocation, videoMemoryInfo);
}

Result DeviceVal::CreatePipeline(const RayTracingPipelineDesc& pipelineDesc, Pipeline*& pipeline) {
    RETURN_ON_FAILURE(this, pipelineDesc.pipelineLayout != nullptr, Result::INVALID_ARGUMENT, "'pipelineDesc.pipelineLayout' is NULL");
    RETURN_ON_FAILURE(this, pipelineDesc.shaderLibrary != nullptr, Result::INVALID_ARGUMENT, "'pipelineDesc.shaderLibrary' is NULL");
    RETURN_ON_FAILURE(this, pipelineDesc.shaderGroupDescs != nullptr, Result::INVALID_ARGUMENT, "'pipelineDesc.shaderGroupDescs' is NULL");
    RETURN_ON_FAILURE(this, pipelineDesc.shaderGroupDescNum != 0, Result::INVALID_ARGUMENT, "'pipelineDesc.shaderGroupDescNum' is 0");
    RETURN_ON_FAILURE(this, pipelineDesc.recursionDepthMax != 0, Result::INVALID_ARGUMENT, "'pipelineDesc.recursionDepthMax' is 0");

    uint32_t uniqueShaderStages = 0;
    for (uint32_t i = 0; i < pipelineDesc.shaderLibrary->shaderNum; i++) {
        const ShaderDesc& shaderDesc = pipelineDesc.shaderLibrary->shaders[i];

        RETURN_ON_FAILURE(this, shaderDesc.bytecode != nullptr, Result::INVALID_ARGUMENT, "'pipelineDesc.shaderLibrary->shaders[%u].bytecode' is invalid", i);

        RETURN_ON_FAILURE(this, shaderDesc.size != 0, Result::INVALID_ARGUMENT, "'pipelineDesc.shaderLibrary->shaders[%u].size' is 0", i);
        RETURN_ON_FAILURE(this, IsShaderStageValid(shaderDesc.stage, uniqueShaderStages, StageBits::RAY_TRACING_SHADERS), Result::INVALID_ARGUMENT,
            "'pipelineDesc.shaderLibrary->shaders[%u].stage' must include only 1 ray tracing shader stage, unique for the entire pipeline", i);
    }

    auto pipelineDescImpl = pipelineDesc;
    pipelineDescImpl.pipelineLayout = NRI_GET_IMPL(PipelineLayout, pipelineDesc.pipelineLayout);

    Pipeline* pipelineImpl = nullptr;
    Result result = m_RayTracingAPI.CreateRayTracingPipeline(m_Device, pipelineDescImpl, pipelineImpl);

    if (result == Result::SUCCESS)
        pipeline = (Pipeline*)Allocate<PipelineVal>(GetStdAllocator(), *this, pipelineImpl);

    return result;
}

Result DeviceVal::CreateAccelerationStructure(const AccelerationStructureDesc& accelerationStructureDesc, AccelerationStructure*& accelerationStructure) {
    RETURN_ON_FAILURE(this, accelerationStructureDesc.instanceOrGeometryObjectNum != 0, Result::INVALID_ARGUMENT,
        "'accelerationStructureDesc.instanceOrGeometryObjectNum' is 0");

    AccelerationStructureDesc accelerationStructureDescImpl = accelerationStructureDesc;

    uint32_t geometryObjectNum = accelerationStructureDesc.type == AccelerationStructureType::BOTTOM_LEVEL ? accelerationStructureDesc.instanceOrGeometryObjectNum : 0;
    Scratch<GeometryObject> objectImplArray = AllocateScratch(*this, GeometryObject, geometryObjectNum);

    if (accelerationStructureDesc.type == AccelerationStructureType::BOTTOM_LEVEL) {
        ConvertGeometryObjectsVal(objectImplArray, accelerationStructureDesc.geometryObjects, geometryObjectNum);
        accelerationStructureDescImpl.geometryObjects = objectImplArray;
    }

    AccelerationStructure* accelerationStructureImpl = nullptr;
    Result result = m_RayTracingAPI.CreateAccelerationStructure(m_Device, accelerationStructureDescImpl, accelerationStructureImpl);

    if (result == Result::SUCCESS) {
        MemoryDesc memoryDesc = {};
        m_RayTracingAPI.GetAccelerationStructureMemoryDesc(GetImpl(), accelerationStructureDescImpl, MemoryLocation::DEVICE, memoryDesc);

        accelerationStructure = (AccelerationStructure*)Allocate<AccelerationStructureVal>(GetStdAllocator(), *this, accelerationStructureImpl, false, memoryDesc);
    }

    return result;
}

Result DeviceVal::AllocateAccelerationStructure(const AllocateAccelerationStructureDesc& accelerationStructureDesc, AccelerationStructure*& accelerationStructure) {
    RETURN_ON_FAILURE(this, accelerationStructureDesc.desc.instanceOrGeometryObjectNum != 0, Result::INVALID_ARGUMENT, "'accelerationStructureDesc.instanceOrGeometryObjectNum' is 0");

    AllocateAccelerationStructureDesc accelerationStructureDescImpl = accelerationStructureDesc;

    uint32_t geometryObjectNum = accelerationStructureDesc.desc.type == AccelerationStructureType::BOTTOM_LEVEL ? accelerationStructureDesc.desc.instanceOrGeometryObjectNum : 0;
    Scratch<GeometryObject> objectImplArray = AllocateScratch(*this, GeometryObject, geometryObjectNum);

    if (accelerationStructureDesc.desc.type == AccelerationStructureType::BOTTOM_LEVEL) {
        ConvertGeometryObjectsVal(objectImplArray, accelerationStructureDesc.desc.geometryObjects, geometryObjectNum);
        accelerationStructureDescImpl.desc.geometryObjects = objectImplArray;
    }

    AccelerationStructure* accelerationStructureImpl = nullptr;
    Result result = m_ResourceAllocatorAPI.AllocateAccelerationStructure(m_Device, accelerationStructureDescImpl, accelerationStructureImpl);

    if (result == Result::SUCCESS) {
        MemoryDesc memoryDesc = {};
        m_RayTracingAPI.GetAccelerationStructureMemoryDesc(GetImpl(), accelerationStructureDescImpl.desc, MemoryLocation::DEVICE, memoryDesc);

        accelerationStructure = (AccelerationStructure*)Allocate<AccelerationStructureVal>(GetStdAllocator(), *this, accelerationStructureImpl, true, memoryDesc);
    }

    return result;
}

Result DeviceVal::BindAccelerationStructureMemory(const AccelerationStructureMemoryBindingDesc* memoryBindingDescs, uint32_t memoryBindingDescNum) {
    RETURN_ON_FAILURE(this, memoryBindingDescs != nullptr, Result::INVALID_ARGUMENT, "'memoryBindingDescs' is NULL");

    AccelerationStructureMemoryBindingDesc* memoryBindingDescsImpl = StackAlloc(AccelerationStructureMemoryBindingDesc, memoryBindingDescNum);
    for (uint32_t i = 0; i < memoryBindingDescNum; i++) {
        AccelerationStructureMemoryBindingDesc& destDesc = memoryBindingDescsImpl[i];
        const AccelerationStructureMemoryBindingDesc& srcDesc = memoryBindingDescs[i];

        MemoryVal& memory = (MemoryVal&)*srcDesc.memory;
        AccelerationStructureVal& accelerationStructure = (AccelerationStructureVal&)*srcDesc.accelerationStructure;
        const MemoryDesc& memoryDesc = accelerationStructure.GetMemoryDesc();

        RETURN_ON_FAILURE(this, !accelerationStructure.IsBoundToMemory(), Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].accelerationStructure' is already bound to memory", i);
        RETURN_ON_FAILURE(this, !memoryDesc.mustBeDedicated || srcDesc.offset == 0, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].offset' must be 0 for dedicated allocation", i);
        RETURN_ON_FAILURE(this, memoryDesc.alignment != 0, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].alignment' is 0", i);
        RETURN_ON_FAILURE(this, srcDesc.offset % memoryDesc.alignment == 0, Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].offset' is misaligned", i);

        const uint64_t rangeMax = srcDesc.offset + memoryDesc.size;
        const bool memorySizeIsUnknown = memory.GetSize() == 0;

        RETURN_ON_FAILURE(this, memorySizeIsUnknown || rangeMax <= memory.GetSize(), Result::INVALID_ARGUMENT, "'memoryBindingDescs[%u].offset' is invalid", i);

        destDesc = srcDesc;
        destDesc.memory = memory.GetImpl();
        destDesc.accelerationStructure = accelerationStructure.GetImpl();
    }

    Result result = m_RayTracingAPI.BindAccelerationStructureMemory(m_Device, memoryBindingDescsImpl, memoryBindingDescNum);

    if (result == Result::SUCCESS) {
        for (uint32_t i = 0; i < memoryBindingDescNum; i++) {
            MemoryVal& memory = *(MemoryVal*)memoryBindingDescs[i].memory;
            memory.BindAccelerationStructure(*(AccelerationStructureVal*)memoryBindingDescs[i].accelerationStructure);
        }
    }

    return result;
}

void DeviceVal::DestroyAccelerationStructure(AccelerationStructure& accelerationStructure) {
    Destroy(GetStdAllocator(), (AccelerationStructureVal*)&accelerationStructure);
}

void DeviceVal::Destruct() {
    Destroy(GetStdAllocator(), this);
}

DeviceBase* CreateDeviceValidation(const DeviceCreationDesc& deviceCreationDesc, DeviceBase& device) {
    StdAllocator<uint8_t> allocator(deviceCreationDesc.allocationCallbacks);
    DeviceVal* deviceVal = Allocate<DeviceVal>(allocator, deviceCreationDesc.callbackInterface, allocator, device);

    if (!deviceVal->Create()) {
        Destroy(allocator, deviceVal);
        return nullptr;
    }

    return deviceVal;
}

#include "DeviceVal.hpp"
