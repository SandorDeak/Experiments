//#define D3DX12_NO_STATE_OBJECT_HELPERS 1
//#include "d3dx12.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Unknwn.h>
#undef min
#undef max

#include <stdio.h>

#ifdef ROOTD12INCLUDE
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#else
//only for intellisense, they are not used for compilation
#include "C:\Program Files (x86)\Windows Kits\10\Include\10.0.17763.0\um\d3d12.h"
#include "C:\Program Files (x86)\Windows Kits\10\Include\10.0.17763.0\shared\dxgi1_6.h"
#include <d3dcompiler.h>
#endif

#include "math.h"
#include "memory_arena.h"

inline D3D12_RESOURCE_BARRIER transition(
	ID3D12Resource* pResource,
	D3D12_RESOURCE_STATES stateBefore,
	D3D12_RESOURCE_STATES stateAfter,
	UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
	D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE)
{
	D3D12_RESOURCE_BARRIER result = {};
	result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	result.Flags = flags;
	result.Transition.pResource = pResource;
	result.Transition.StateBefore = stateBefore;
	result.Transition.StateAfter = stateAfter;
	result.Transition.Subresource = subresource;
	return result;
}

inline D3D12_ROOT_PARAMETER InitAsConstantsRootParam(
	UINT num32BitValues,
	UINT shaderRegister,
	UINT registerSpace = 0,
	D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL)
{
	D3D12_ROOT_PARAMETER result = {};
	result.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	result.ShaderVisibility = visibility;
	result.Constants.Num32BitValues = num32BitValues;
	result.Constants.ShaderRegister = shaderRegister;
	result.Constants.RegisterSpace = registerSpace;
	return result;
}

inline D3D12_ROOT_PARAMETER InitAsConstantsBufferView(
	UINT shaderRegister,
	UINT registerSpace = 0,
	D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL)
{
	D3D12_ROOT_PARAMETER result = {};
	result.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	result.ShaderVisibility = visibility;
	result.Descriptor.ShaderRegister = shaderRegister;
	result.Descriptor.RegisterSpace = registerSpace;

	return result;
}

inline D3D12_ROOT_PARAMETER InitAsDescriptorTable(
	UINT descriptorRangeCount,
	D3D12_DESCRIPTOR_RANGE* descriptorRanges,
	D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL)
{
	D3D12_ROOT_PARAMETER result = {};
	result.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	result.ShaderVisibility = visibility;
	result.DescriptorTable.NumDescriptorRanges = descriptorRangeCount;
	result.DescriptorTable.pDescriptorRanges = descriptorRanges;

	return result;
}

inline D3D12_HEAP_PROPERTIES createHeapProperties(D3D12_HEAP_TYPE heapType)
{
	D3D12_HEAP_PROPERTIES result = {};
	result.Type = heapType;
	result.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	result.MemoryPoolPreference= D3D12_MEMORY_POOL_UNKNOWN;
	return result;
}

inline D3D12_RESOURCE_DESC createResourceDescBuffer(u64 width,
	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE, u64 alignment = 0)
{
	D3D12_RESOURCE_DESC result = {};
	result.Width = width;
	result.Flags = flags;
	result.Alignment = alignment;
	result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	result.Format = DXGI_FORMAT_UNKNOWN;
	result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	result.SampleDesc.Count = 1;
	result.SampleDesc.Quality = 0;
	result.Height = 1;
	result.DepthOrArraySize = 1;
	result.MipLevels = 1;
	return result;
}

inline D3D12_RESOURCE_DESC createResourceDescTex2D(DXGI_FORMAT format, u32 width, u32 height,
	u16 mipLevels,
	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
	D3D12_TEXTURE_LAYOUT layout = D3D12_TEXTURE_LAYOUT_UNKNOWN, u64 alignment = 0)
{
	D3D12_RESOURCE_DESC result = {};
	result.Width = width;
	result.Flags = flags;
	result.Alignment = alignment;
	result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	result.Format = format;
	result.Layout = layout;
	result.SampleDesc.Count = 1;
	result.SampleDesc.Quality = 0;
	result.Height = height;
	result.DepthOrArraySize = 1;
	result.MipLevels = mipLevels;
	return result;
}

inline D3D12_INPUT_ELEMENT_DESC createInputElementDesc(char* semanticName, DXGI_FORMAT format, 
	u32 offset = D3D12_APPEND_ALIGNED_ELEMENT,
	u32 inputSlot = 0,
	D3D12_INPUT_CLASSIFICATION inputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA)
{
	D3D12_INPUT_ELEMENT_DESC result = {};
	result.AlignedByteOffset = offset;
	result.Format = format;
	result.InputSlot = inputSlot;
	result.InputSlotClass = inputSlotClass;
	result.SemanticIndex = 0;
	result.SemanticName = semanticName;
	return result;
}

static ID3D12DescriptorHeap* createDescriptorHeap(ID3D12Device2* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 descriptorCount, b32 shaderVisible = false)
{
	ID3D12DescriptorHeap* result = 0;

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = type;
	heapDesc.NumDescriptors = descriptorCount;
	heapDesc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ASSERT(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&result)) == S_OK);

	return result;
}

static void waitForFenceValue(ID3D12Fence* fence, u64 fenceValue, u32 timeOutMilliseconds = U32_MAX)
{
	if (fence->GetCompletedValue() < fenceValue)
	{
		//TODO: store the events in a pool or something...
		HANDLE fenceEvent = CreateEventA(0, FALSE, FALSE, 0);
		ASSERT(fenceEvent);
		ASSERT(fence->SetEventOnCompletion(fenceValue, fenceEvent) == S_OK);
		DWORD waitResult = WaitForSingleObject(fenceEvent, (DWORD)timeOutMilliseconds);
		ASSERT(waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT);
		CloseHandle(fenceEvent);
	}
}

static void incrementAndSignal(ID3D12CommandQueue* commandQueue, ID3D12Fence* fence, u64* fenceValue)
{
	++(*fenceValue);
	ASSERT(commandQueue->Signal(fence, *fenceValue) == S_OK);
}

static void flushCommandQueue(ID3D12CommandQueue* commandQueue, ID3D12Fence* fence, u64* fenceValue)
{
	incrementAndSignal(commandQueue, fence, fenceValue);
	waitForFenceValue(fence, *fenceValue);
}

struct FenceSlot
{
	ID3D12Fence* d12Fence;
	u64 requiredFenceValue;
};

static b32 fenceSlotEmptyOrValueReached(FenceSlot* fenceSlot)
{
	b32 result = (!fenceSlot->d12Fence || (fenceSlot->d12Fence->GetCompletedValue() >= fenceSlot->requiredFenceValue));
	return result;
}

static b32 fenceSlotIsNewerOrEqual(FenceSlot* fenceSlot, ID3D12Fence* fence, u64 fenceValue)
{
	b32 result = false;
	if (fenceSlot->d12Fence == fence)
	{
		ASSERT(fenceValue >= fenceSlot->requiredFenceValue && "Older than already requested fenceValue was requested. Is it a bug?");
		result = true;
	}
	return result;
}

static b32 fenceSlotValueNotReachedYet(FenceSlot* fenceSlot)
{
	b32 result = false;
	if (fenceSlot->d12Fence)
	{
		if (fenceSlot->d12Fence->GetCompletedValue() < fenceSlot->requiredFenceValue)
		{
			result = true;
		}
		else
		{
			fenceSlot->d12Fence = 0;
		}
	}
	return result;
}

struct TrackedResource
{
	ID3D12Resource* d12Resource;
	FenceSlot useFenceSlots[4];
	FenceSlot modifyFenceSlot;
	D3D12_RESOURCE_STATES stateAfterModification;
};

static void markUse(TrackedResource* resource, ID3D12CommandQueue* commandQueue, ID3D12Fence* fence, u64 requiredFenceValue)
{
	if (!fenceSlotIsNewerOrEqual(&resource->modifyFenceSlot, fence, requiredFenceValue))
	{
		if (fenceSlotValueNotReachedYet(&resource->modifyFenceSlot))
		{
			commandQueue->Wait(resource->modifyFenceSlot.d12Fence, resource->modifyFenceSlot.requiredFenceValue);
		}

		b32 alreadyRequested = false;
		FenceSlot* emptyFenceSlot = 0;
		for (s32 fenceSlotIndex = 0; fenceSlotIndex < ARRAY_SIZE(resource->useFenceSlots); ++fenceSlotIndex)
		{
			FenceSlot* fenceSlot = resource->useFenceSlots + fenceSlotIndex;
			if (fenceSlotIsNewerOrEqual(fenceSlot, fence, requiredFenceValue))
			{
				alreadyRequested = true;
				break;
			}
			else if (!emptyFenceSlot && fenceSlotEmptyOrValueReached(fenceSlot))
			{
				emptyFenceSlot = fenceSlot;
				
			}
		}
		if (!alreadyRequested)
		{
			ASSERT(emptyFenceSlot && "No empty fenceSlot!");
			emptyFenceSlot->d12Fence = fence;
			emptyFenceSlot->requiredFenceValue = requiredFenceValue;
		}
	}
}


static void markModify(TrackedResource* resource, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList,
	ID3D12Fence* fence, u64 requiredFenceValue, D3D12_RESOURCE_STATES stateToModify)
{
	if (!fenceSlotIsNewerOrEqual(&resource->modifyFenceSlot, fence, requiredFenceValue) && fenceSlotValueNotReachedYet(&resource->modifyFenceSlot))
	{
		commandQueue->Wait(resource->modifyFenceSlot.d12Fence, resource->modifyFenceSlot.requiredFenceValue);
	}
	for (s32 fenceSlotIndex = 0; fenceSlotIndex < ARRAY_SIZE(resource->useFenceSlots); ++fenceSlotIndex)
	{
		FenceSlot* fenceSlot = resource->useFenceSlots + fenceSlotIndex;
		if (!fenceSlotIsNewerOrEqual(fenceSlot, fence, requiredFenceValue) && fenceSlotValueNotReachedYet(fenceSlot))
		{
			commandQueue->Wait(fenceSlot->d12Fence, fenceSlot->requiredFenceValue);
			fenceSlot->d12Fence = 0;
		}
	}

	if (stateToModify != resource->stateAfterModification)
	{
		commandList->ResourceBarrier(1,
			&transition(
				resource->d12Resource,
				resource->stateAfterModification,
				stateToModify
			)
		);
		resource->stateAfterModification = stateToModify;
	}

	resource->modifyFenceSlot.requiredFenceValue = requiredFenceValue;
	resource->modifyFenceSlot.d12Fence = fence;
}

static b32 resourceReady(TrackedResource* resource)
{
	return fenceSlotEmptyOrValueReached(&resource->modifyFenceSlot);
}

struct Button
{
	b32 wasDown;
	b32 isDown;
};

struct Input
{
	Button W;
	Button A;
	Button S;
	Button D;
	Button space;
	Button left;
	Button right;
	Button up;
	Button C;
	Button down;
};


struct VertexBuffer
{
	TrackedResource resource;
	D3D12_VERTEX_BUFFER_VIEW d12View;
};

struct IndexBuffer
{
	TrackedResource resource;
	D3D12_INDEX_BUFFER_VIEW d12View;
};
struct ConstantBuffer
{
	TrackedResource resource;
	D3D12_GPU_VIRTUAL_ADDRESS gpuVirtualAddress;
};

struct UploadHeap
{
	ID3D12Resource* d12Resource;
	MemoryArena arena;
};

struct CommandList
{
	ID3D12GraphicsCommandList* d12CommandList;
};

struct CommandQueue
{
	ID3D12CommandQueue* d12CommandQueue;
	u64 reachedFenceValue;
	u64 lastSignaledFenceValue;
	ID3D12Fence* d12Fence;
};

struct CommandAllocator
{
	ID3D12CommandAllocator* d12CommandAllocator;
	u64 requiredFenceValueForReset;
};

struct UploadHeapAllocation
{
	void* cpuMemory;
	u64 gpuMemoryOffset;
	ID3D12Resource* heap;
};

#define RESOURCE_MANAGER_RING_SIZE 3
#define RESOURCE_MANAGER_UPLOAD_HEAP_SIZE (64*1024*1024)
struct ResourceManager
{
	UploadHeap uploadHeaps[RESOURCE_MANAGER_RING_SIZE];
	CommandAllocator commandAllocators[RESOURCE_MANAGER_RING_SIZE];
	CommandList commandList;
	CommandQueue commandQueue;
	u32 currentRingIndex;
	ID3D12Device2* device;


	UploadHeap* getCurrentUploadHeap() { return uploadHeaps + currentRingIndex; }
	CommandAllocator* getCurrentCommandAllocator() { return commandAllocators + currentRingIndex; }
	void advanceRing()
	{
		uploadHeaps[currentRingIndex].d12Resource->Unmap(0, 0);
		currentRingIndex = (currentRingIndex + 1) % RESOURCE_MANAGER_RING_SIZE;

		waitForFenceValue(commandQueue.d12Fence, commandAllocators[currentRingIndex].requiredFenceValueForReset);
		commandAllocators[currentRingIndex].d12CommandAllocator->Reset();
		void* mappedHeap = 0;
		uploadHeaps[currentRingIndex].d12Resource->Map(0, 0, &mappedHeap);
		ASSERT(mappedHeap);
		uploadHeaps[currentRingIndex].arena = createMemoryArena(mappedHeap, RESOURCE_MANAGER_UPLOAD_HEAP_SIZE);
	}

	UploadHeapAllocation allocateFromUploadHeap(umm size, umm alignment = 4)
	{
		ASSERT(size <= RESOURCE_MANAGER_UPLOAD_HEAP_SIZE);
		UploadHeapAllocation result = {};
		void* cpuMemory = pushSize(&uploadHeaps[currentRingIndex].arena, size, alignment);
		if (!cpuMemory)
		{
			advanceRing();
			cpuMemory = pushSize(&uploadHeaps[currentRingIndex].arena, size, alignment);
		}
		ASSERT(cpuMemory);

		commandAllocators[currentRingIndex].requiredFenceValueForReset = commandQueue.lastSignaledFenceValue + 1;

		result.cpuMemory = cpuMemory;
		result.gpuMemoryOffset = getOffset(&uploadHeaps[currentRingIndex].arena, cpuMemory);
		result.heap = uploadHeaps[currentRingIndex].d12Resource;

		return result;
	}

	ID3D12GraphicsCommandList* startCommandList()
	{
		commandList.d12CommandList->Reset(commandAllocators[currentRingIndex].d12CommandAllocator, 0);
		commandAllocators[currentRingIndex].requiredFenceValueForReset = commandQueue.lastSignaledFenceValue + 1;
		return commandList.d12CommandList;
	}

	u64 submitCommandListAndSignal()
	{
		ASSERT(commandList.d12CommandList->Close() == S_OK);
		ID3D12CommandList* commandLists[] = { commandList.d12CommandList };
		commandQueue.d12CommandQueue->ExecuteCommandLists(ARRAY_SIZE(commandLists), commandLists);
		incrementAndSignal(commandQueue.d12CommandQueue, commandQueue.d12Fence, &commandQueue.lastSignaledFenceValue);
		return commandQueue.lastSignaledFenceValue;
	}
	static ResourceManager create(ID3D12Device2* device)
	{
		ResourceManager result = {};

		result.device = device;

		//create uploadHeaps
		for (s32 heapIndex = 0; heapIndex < ARRAY_SIZE(result.uploadHeaps); ++heapIndex)
		{
			UploadHeap* heap = result.uploadHeaps + heapIndex;

			ASSERT(device->CreateCommittedResource(
				&createHeapProperties(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&createResourceDescBuffer(RESOURCE_MANAGER_UPLOAD_HEAP_SIZE),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				0,
				IID_PPV_ARGS(&heap->d12Resource)
			) == S_OK);

		}
		void* mappedHeap = 0;
		result.uploadHeaps[0].d12Resource->Map(0, 0, &mappedHeap);
		ASSERT(mappedHeap);
		result.uploadHeaps[0].arena = createMemoryArena(mappedHeap, RESOURCE_MANAGER_UPLOAD_HEAP_SIZE);

		//create command allocators
		for (s32 commandAllocatorIndex = 0; commandAllocatorIndex < ARRAY_SIZE(result.commandAllocators); ++commandAllocatorIndex)
		{
			CommandAllocator* commandAllocator = result.commandAllocators + commandAllocatorIndex;
			ASSERT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,
				IID_PPV_ARGS(&commandAllocator->d12CommandAllocator)) == S_OK);
		}

		//create commandList
		ASSERT(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, result.commandAllocators[0].d12CommandAllocator,
			0, IID_PPV_ARGS(&result.commandList.d12CommandList)) == S_OK);
		ASSERT(result.commandList.d12CommandList->Close() == S_OK);

		//create commandQueue
		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		ASSERT(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&result.commandQueue.d12CommandQueue)) == S_OK);

		//createFence
		ASSERT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result.commandQueue.d12Fence)) == S_OK);
		

		return result;
	}

	void release()
	{
		ASSERT(!"Not implemented");
	}
};

static void uploadToBuffer(ResourceManager* resourceManager, TrackedResource* buffer, u64 offset, u64 size, void* data)
{
	UploadHeapAllocation alloc = resourceManager->allocateFromUploadHeap(size);
	memcpy(alloc.cpuMemory, data, size);

	u64 fenceValue = resourceManager->commandQueue.lastSignaledFenceValue + 1;
	ID3D12GraphicsCommandList* commandList = resourceManager->startCommandList();
	markModify(buffer, resourceManager->commandQueue.d12CommandQueue, commandList, resourceManager->commandQueue.d12Fence,
		fenceValue, D3D12_RESOURCE_STATE_COPY_DEST);
	commandList->CopyBufferRegion(buffer->d12Resource, offset, alloc.heap, alloc.gpuMemoryOffset, size);
	ASSERT(fenceValue == resourceManager->submitCommandListAndSignal());
}

struct Image2D
{
	u8* memory;
	u32 width;
	u32 height;
	u32 pitch;
};

struct Image2DLod
{
	Image2D lod[16];
	u32 lodCount;
};

static void uploadToTextureLod(ResourceManager* resourceManager, TrackedResource* texture, Image2DLod* imageLod, DXGI_FORMAT format)
{
	u64 fenceValue = resourceManager->commandQueue.lastSignaledFenceValue + 1;
	ID3D12GraphicsCommandList* commandList = resourceManager->startCommandList();
	markModify(texture, resourceManager->commandQueue.d12CommandQueue, commandList, resourceManager->commandQueue.d12Fence, fenceValue,
		D3D12_RESOURCE_STATE_COPY_DEST);

	for (u32 lod = 0; lod < imageLod->lodCount; ++lod)
	{
		Image2D* image = imageLod->lod + lod;
		u64 size = image->height * image->pitch;
		UploadHeapAllocation alloc = resourceManager->allocateFromUploadHeap(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		memcpy(alloc.cpuMemory, image->memory, size);

		D3D12_TEXTURE_COPY_LOCATION src = {};
		src.PlacedFootprint.Offset = alloc.gpuMemoryOffset;
		src.PlacedFootprint.Footprint.Format = format;
		src.PlacedFootprint.Footprint.Height = image->height;
		src.PlacedFootprint.Footprint.Depth = 1;
		src.PlacedFootprint.Footprint.Width = image->width;
		src.PlacedFootprint.Footprint.RowPitch = image->pitch;

		src.pResource = alloc.heap;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.SubresourceIndex = lod;
		dst.pResource = texture->d12Resource;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

		commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, 0);
	}
	ASSERT(fenceValue == resourceManager->submitCommandListAndSignal());
}

static void uploadToTexture(ResourceManager* resourceManager, TrackedResource* texture, Image2D* image, DXGI_FORMAT format)
{
	u64 size = image->height * image->pitch;
	UploadHeapAllocation alloc = resourceManager->allocateFromUploadHeap(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	memcpy(alloc.cpuMemory, image->memory, size);

	u64 fenceValue = resourceManager->commandQueue.lastSignaledFenceValue + 1;
	ID3D12GraphicsCommandList* commandList = resourceManager->startCommandList();
	markModify(texture, resourceManager->commandQueue.d12CommandQueue, commandList, resourceManager->commandQueue.d12Fence, fenceValue,
		D3D12_RESOURCE_STATE_COPY_DEST);

	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.PlacedFootprint.Offset = alloc.gpuMemoryOffset;
	src.PlacedFootprint.Footprint.Format = format;
	src.PlacedFootprint.Footprint.Height = image->height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.Width = image->width;
	src.PlacedFootprint.Footprint.RowPitch = image->pitch;

	src.pResource = alloc.heap;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.SubresourceIndex = 0;
	dst.pResource = texture->d12Resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

	commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, 0);
	ASSERT(fenceValue == resourceManager->submitCommandListAndSignal());
}

static ConstantBuffer createConstantBuffer(ResourceManager* resourceManager, u32 size, void* data)
{
	ASSERT(!data && "copy not implemented yet");

	ConstantBuffer result = {};

	ID3D12Resource* d12Cb = 0;
	ASSERT(resourceManager->device->CreateCommittedResource(
		&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&createResourceDescBuffer(size),
		D3D12_RESOURCE_STATE_COPY_DEST,
		0,
		IID_PPV_ARGS(&d12Cb)
	) == S_OK);

	result.resource.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	result.resource.d12Resource = d12Cb;
	result.gpuVirtualAddress = d12Cb->GetGPUVirtualAddress();

	return result;
}

static VertexBuffer createVertexBuffer(ResourceManager* resourceManager, u64 size, u64 stride, void* data)
{
	VertexBuffer result = {};

	ID3D12Resource* d12Vb = 0;
	ASSERT(resourceManager->device->CreateCommittedResource(
		&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&createResourceDescBuffer(size),
		D3D12_RESOURCE_STATE_COPY_DEST,
		0,
		IID_PPV_ARGS(&d12Vb)
	) == S_OK);

	UploadHeapAllocation alloc = resourceManager->allocateFromUploadHeap(size);
	memcpy(alloc.cpuMemory, data, size);

	ID3D12GraphicsCommandList* commandList = resourceManager->startCommandList();
	commandList->CopyBufferRegion(d12Vb, 0, alloc.heap, alloc.gpuMemoryOffset, size);
	u64 fenceValue = resourceManager->submitCommandListAndSignal();
	result.resource.d12Resource = d12Vb;
	result.resource.modifyFenceSlot.d12Fence = resourceManager->commandQueue.d12Fence;
	result.resource.modifyFenceSlot.requiredFenceValue = fenceValue;
	result.resource.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	result.d12View.SizeInBytes = (UINT)size;
	result.d12View.StrideInBytes = (UINT)stride;
	result.d12View.BufferLocation = d12Vb->GetGPUVirtualAddress();

	return result;
}

static IndexBuffer createIndexBuffer(ResourceManager* resourceManager, u64 size, b32 typeIsU32, void* data)
{
	IndexBuffer result = {};

	ID3D12Resource* d12Ib = 0;
	ASSERT(resourceManager->device->CreateCommittedResource(
		&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&createResourceDescBuffer(size),
		D3D12_RESOURCE_STATE_COPY_DEST,
		0,
		IID_PPV_ARGS(&d12Ib)
	) == S_OK);

	UploadHeapAllocation alloc = resourceManager->allocateFromUploadHeap(size);
	memcpy(alloc.cpuMemory, data, size);

	ID3D12GraphicsCommandList* commandList = resourceManager->startCommandList();
	commandList->CopyBufferRegion(d12Ib, 0, alloc.heap, alloc.gpuMemoryOffset, size);
	u64 fenceValue = resourceManager->submitCommandListAndSignal();

	result.resource.d12Resource = d12Ib;
	result.resource.modifyFenceSlot.d12Fence = resourceManager->commandQueue.d12Fence;
	result.resource.modifyFenceSlot.requiredFenceValue = fenceValue;
	result.resource.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;

	result.d12View.SizeInBytes = (UINT)size;
	result.d12View.Format = typeIsU32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
	result.d12View.BufferLocation = d12Ib->GetGPUVirtualAddress();

	return result;
}

static void updateButton(Button* button)
{
	button->wasDown = button->isDown;
}

static void updateInput(Input* input)
{
	updateButton(&input->W);
	updateButton(&input->A);
	updateButton(&input->S);
	updateButton(&input->D);
	updateButton(&input->C);
	updateButton(&input->space);
	updateButton(&input->left);
	updateButton(&input->right);
	updateButton(&input->up);
	updateButton(&input->down);
}

enum SHADER
{
	SHADER_VERTEX,
	SHADER_HULL,
	SHADER_DOMAIN,
	SHADER_GEOMETRY,
	SHADER_PIXEL,
	SHADER_COUNT,
};


struct FileInfo
{
	wchar_t* name;
	FILETIME lastUpdateTime;
};
struct GraphicsPipeline
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
	FileInfo shaders[SHADER_COUNT];
	D3D12_INPUT_ELEMENT_DESC inputElementDescs[16];
	ID3D12PipelineState* pipeline;
};

static FILETIME Win32GetLastWriteTime(wchar_t* fileName)
{
	FILETIME result = {};

	WIN32_FILE_ATTRIBUTE_DATA attributes;
	if (GetFileAttributesExW(fileName, GetFileExInfoStandard, &attributes))
	{
		result = attributes.ftLastWriteTime;
	}
	return result;
}

static b32 Win32FileWasUpdated(wchar_t* fileName, FILETIME lastKnownUpdateTime)
{
	b32 result = false;

	FILETIME lastUpdateTime = Win32GetLastWriteTime(fileName);
	if (CompareFileTime(&lastKnownUpdateTime, &lastUpdateTime) == -1)
	{
		result = true;
	}

	return result;
}

static b32 shouldRebuildGraphicspipeline(GraphicsPipeline* graphicsPipeline)
{
	b32 result = false;
	for (u32 shaderIndex = 0; shaderIndex < SHADER_COUNT; ++shaderIndex)
	{
		FileInfo* shader = graphicsPipeline->shaders + shaderIndex;
		if (shader->name && Win32FileWasUpdated(shader->name, shader->lastUpdateTime))
		{
			result = true;
			break;
		}
	}
	return result;
}

static void rebuildGraphicsPipeline(ID3D12Device2* device, GraphicsPipeline* graphicsPipeline)
{
	graphicsPipeline->desc.VS = {};
	graphicsPipeline->desc.HS = {};
	graphicsPipeline->desc.DS = {};
	graphicsPipeline->desc.GS = {};
	graphicsPipeline->desc.PS = {};

	ID3DBlob* shaderBlobs[SHADER_COUNT] = {};
	for (u32 shaderIndex = 0; shaderIndex < SHADER_COUNT; ++shaderIndex)
	{
		FileInfo* shader = graphicsPipeline->shaders + shaderIndex;
		if (shader->name)
		{
			ID3DBlob** shaderBlob = shaderBlobs + shaderIndex;
			shader->lastUpdateTime = Win32GetLastWriteTime(shader->name);
			ASSERT(D3DReadFileToBlob(shader->name, shaderBlob) == S_OK);

			D3D12_SHADER_BYTECODE* byteCode = 0;
			switch (shaderIndex)
			{
			case SHADER_VERTEX: { byteCode = &graphicsPipeline->desc.VS; break; }
			case SHADER_HULL: { byteCode = &graphicsPipeline->desc.HS; break; }
			case SHADER_DOMAIN: { byteCode = &graphicsPipeline->desc.DS; break; }
			case SHADER_GEOMETRY: { byteCode = &graphicsPipeline->desc.GS; break; }
			case SHADER_PIXEL: { byteCode = &graphicsPipeline->desc.PS; break; }
			default: { INVALID_CODE_PATH; }
			}

			byteCode->BytecodeLength = (*shaderBlob)->GetBufferSize();
			byteCode->pShaderBytecode = (*shaderBlob)->GetBufferPointer();
		}
	}
	if (graphicsPipeline->pipeline)
	{
		graphicsPipeline->pipeline->Release();
	}
	ASSERT(device->CreateGraphicsPipelineState(&graphicsPipeline->desc, IID_PPV_ARGS(&graphicsPipeline->pipeline)) == S_OK);

	for (u32 shaderBlobIndex = 0; shaderBlobIndex < SHADER_COUNT; ++shaderBlobIndex)
	{
		if (shaderBlobs[shaderBlobIndex])
		{
			shaderBlobs[shaderBlobIndex]->Release();
		}
	}

}

struct Vertex
{
	v3 position;
	v3 normal;
	v3 tangent;
	v3 bitangent;
	v2 uv;
};

#pragma warning(push)
#pragma warning(disable : 4324)
struct SceneBuffer
{
	m4 projview;
	v3 lightPos;
	v3 viewPos;
};
struct alignas(256) ModelBuffer
{
	m4 model;
	v4 color;
	v3 scale;
	f32 roughness;
	f32 metalness;
	f32 vertexDisplacement;
};
#pragma warning(pop)


global b32 g_running = true;
global LARGE_INTEGER g_perfCounterFrequency;

static void* Win32AllocateMemory(umm size)
{
	void* result = 0;
	result = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	return result;
}

static void Win32DeallocateMemory(void* ptr)
{
	VirtualFree(ptr, 0, MEM_RELEASE);
}

static void Win32ProcessPendingMessages(Input* input)
{
	MSG message;
	while (PeekMessageA(&message, 0, 0, 0, PM_REMOVE))
	{
		switch (message.message)
		{
		case WM_SYSKEYUP:
		case WM_SYSKEYDOWN:
		case WM_KEYUP:
		case WM_KEYDOWN:
		{
			u32 vkCode = (u32)message.wParam;
			b32 wasDown = (message.lParam & (1 << 30)) != 0;
			b32 isDown = (message.lParam & (1 << 31)) == 0;

			switch (vkCode)
			{
			case 'W': { input->W.isDown = isDown; break; }
			case 'A': { input->A.isDown = isDown; break; }
			case 'S': { input->S.isDown = isDown; break; }
			case 'D': { input->D.isDown = isDown; break; }
			case 'C': { input->C.isDown = isDown; break; }
			case VK_SPACE: { input->space.isDown = isDown; break; }
			case VK_LEFT: { input->left.isDown = isDown; break; }
			case VK_RIGHT: { input->right.isDown = isDown; break; }
			case VK_UP: { input->up.isDown = isDown; break; }
			case VK_DOWN: { input->down.isDown = isDown; break; }
			}
			break;
		}
		default:
		{
			TranslateMessage(&message);
			DispatchMessageA(&message);
			break;
		}
		}
	}
}

static LRESULT CALLBACK Win32WindowCallback(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	LRESULT result = 0;
	switch (message)
	{
	case WM_DESTROY:
	{
		//todo: handle this as an error
		g_running = false;
		OutputDebugStringA("WM_DESTROY\n");
	} break;
	case WM_CLOSE:
	{
		g_running = false;
		OutputDebugStringA("WM_CLOSE\n");
	} break;
	case WM_QUIT:
	{
		g_running = false;
		OutputDebugStringA("WM_QUIT\n");
	} break;
	default:
	{
		result = DefWindowProcA(window, message, wParam, lParam);
	} break;
	}
	return result;
}

static LARGE_INTEGER Win32GetWallClock()
{
	LARGE_INTEGER result;
	QueryPerformanceCounter(&result);
	return result;
}

static f32 Win32GetSecondsElapsed(LARGE_INTEGER start, LARGE_INTEGER end)
{
	ASSERT(g_perfCounterFrequency.QuadPart);
	s64 counterElapsed = end.QuadPart - start.QuadPart;
	return (f32)(counterElapsed) / (f32)(g_perfCounterFrequency.QuadPart);
}

struct Mesh
{
	Vertex* vertices;
	u32 vertexCount;
	u32* indices;
	u32 indexCount;
};


#define fetchSample(image, u, v, type) (*(type*)((image)->memory + (image)->pitch*(v) + sizeof(type)*(u)))

static Image2D _pushImage2D(MemoryArena* arena, u32 width, u32 height, u32 pixelSize, u32 pitchAlign)
{
	Image2D result = {};

	u32 pitch = (u32)ALIGN_NUM(width * pixelSize, pitchAlign);
	u8* memory = (u8*)pushSize(arena, pitch * height, pitchAlign);

	result.height = height;
	result.width = width;
	result.pitch = pitch;
	result.memory = memory;

	return result;
}

#define pushImage2D(arena, width, height, type, pitchAlign) _pushImage2D(arena, width, height, sizeof(type), MAX(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, pitchAlign))

static void fillWithRandomGradients(Image2D* image, u32 seed)
{
	srand(seed);

	u32 width = image->width;
	u32 height = image->height;
	u32 pitch = image->pitch;

	u8* row = image->memory;
	for (u32 y = 0; y < height; ++y)
	{
		v2* pixel = (v2*)row;
		for (u32 x = 0; x < width; ++x)
		{
			f32 angle = 2.f * pi32 * (f32)rand() / (f32)RAND_MAX;
			*pixel++ = { cosf(angle), sinf(angle) };
		}
		row += pitch;
	}
}

static v2 addPerlinNoise(Image2D* image, Image2D* grads, v2 tileSize, f32 scale)
{
	if (tileSize.y == 0.f)
	{
		ASSERT(tileSize.x != 0.f);
		tileSize.y = tileSize.x;
	}
	if (tileSize.x == 0.f)
	{
		ASSERT(tileSize.y != 0.f);
		tileSize.x = tileSize.y;
	}

	u8* row = image->memory;
	v2 range = { 1e10f, -1e10f };
	for (u32 y = 0; y < image->height; ++y)
	{
		f32 v =  ((f32)y + 0.5f) / tileSize.y;

		u32 v0 = (u32)v;
		u32 v1 = v0 + 1;
		f32 dv = v - (f32)v0;

		v0 = v0 % grads->height;
		v1 = v1 % grads->height;

		f32* pixel = (f32*)row;
		for (u32 x = 0; x < image->width; ++x)
		{
			f32 u = ((f32)x + 0.5f) / tileSize.x;

			u32 u0 = (u32)u;
			u32 u1 = u0 + 1;
			f32 du = u - (f32)u0;

			u0 = u0 % grads->width;
			u1 = u1 % grads->width;
			
			v2 t00 = fetchSample(grads, u0, v0, v2);
			v2 t10 = fetchSample(grads, u1, v0, v2);
			v2 t01 = fetchSample(grads, u0, v1, v2);
			v2 t11 = fetchSample(grads, u1, v1, v2);

			f32 a = smoothBlend(dot(t00, { du, dv       }), dot(t10, { du - 1.f, dv       }), du);
			f32 b = smoothBlend(dot(t01, { du, dv - 1.f }), dot(t11, { du - 1.f, dv - 1.f }), du);
			f32 c = smoothBlend(a, b, dv);

			c = scale * c + *pixel;
			range.x = MIN(c, range.x);
			range.y = MAX(c, range.y);

			*pixel++ = c;
		}
		row += image->pitch;
	}

	return range;
}

static void fillNormalMapForHeightMap(Image2D* heightMap, Image2D* normalMap, f32 pixelSize)
{
	ASSERT(heightMap->width == normalMap->width);
	ASSERT(heightMap->height == normalMap->height);

	u8* rowNormal = normalMap->memory;

	for (u32 y = 0; y < heightMap->height; ++y)
	{
		u32 y0 = MAX(y, 1) - 1;
		u32 y1 = MIN(y, heightMap->height - 2) + 1;

		u32* normal = (u32*)rowNormal;
		for (u32 x = 0; x < heightMap->width; ++x)
		{
			u32 x0 = MAX(x, 1) - 1;
			u32 x1 = MIN(x, heightMap->width - 2) + 1;

			f32 dhdx = (fetchSample(heightMap, x1, y, f32) - fetchSample(heightMap, x0, y, f32)) / (2.f * pixelSize);
			f32 dhdy = (fetchSample(heightMap, x, y1, f32) - fetchSample(heightMap, x, y0, f32)) / (2.f*pixelSize);

			v3 n = normalize(v3{ -dhdx, -dhdy, 1.f }); //coordinate order: tangent, bitangent, normal
			n = 0.5f*n + V3(0.5f);
			n *= 255.f;
			u32 nX = (u32)n.x;
			u32 nY = (u32)n.y;
			u32 nZ = (u32)n.z;

			*normal++ = (255 << 24) | (nZ << 16) | (nY << 8) | (nX << 0);
		}
		rowNormal += normalMap->pitch;
	}
}

static void clearImage2D(Image2D* image)
{
	memset(image->memory, 0, image->height*image->pitch);
}

inline Vertex createSphereVertex(f32 u, f32 v)
{
	Vertex result = {};
	result.position = spherePoint(u, v);
	result.normal = result.position;
	result.tangent = dspherePointdu(u, v);
	result.bitangent = dspherePointdv(u, v);
	result.uv = { u, v };
	return result;
}

//TODO: fix the uv mapping around the borders!
static Mesh createSphereMesh(MemoryArena* arena, u32 quadCountU, u32 quadCountV)
{
	Mesh result = {};
	ASSERT(quadCountV > 1 && quadCountU > 2);

	u32 vertexCount = (quadCountU + 1) * (quadCountV + 1) - (2 * quadCountU + 1) - (quadCountV - 2);

	u32 triangleCount = (quadCountV - 1) * quadCountU * 2; //in the first and last row there is only one triangle per quad
	u32 indexCount = 3 * triangleCount;

	Vertex* vertices = pushArray(arena, vertexCount, Vertex);
	u32* indices = pushArray(arena, indexCount, u32);

	u32 index = 0;
	u32* indexAt = indices;

	f32 du = 1.f / (f32)quadCountU;
	f32 dv = 1.f / (f32)quadCountV;

	//build north hat
	Vertex* northPole = vertices + index++;
	*northPole = createSphereVertex(0.f, 0.f);

	u32 aIndex = index++;
	Vertex* a = vertices + aIndex;
	*a = createSphereVertex(0.f, dv);

	f32 u = du;
	for (u32 uIndex = 0; uIndex < quadCountU-1; ++uIndex)
	{
		u32 newIndex = index++;
		Vertex* v = vertices + newIndex;
		*v = createSphereVertex(u, dv);
		
		*indexAt++ = newIndex - 1;
		*indexAt++ = newIndex;
		*indexAt++ = 0; //northPole index

		u += du;
	}

	//close the ring
	*indexAt++ = index - 1; //was added last
	*indexAt++ = aIndex;
	*indexAt++ = 0; //northPole index

	//build cylinder part
	u32 vertexCountInOneRow = quadCountU;

	f32 v = 2 * dv;
	for (u32 vIndex = 0; vIndex < quadCountV - 2; ++vIndex)
	{
		u32 firstIndexInRow= index++;
		Vertex* firstInRow = vertices + firstIndexInRow;
		*firstInRow = createSphereVertex(0.f, v);

		u = du;
		for (f32 uIndex = 0; uIndex < quadCountU - 1; ++uIndex)
		{
			u32 newIndex = index++;
			Vertex* newVertex = vertices + newIndex;
			*newVertex = createSphereVertex(u, v);

			*indexAt++ = newIndex - 1;
			*indexAt++ = newIndex - vertexCountInOneRow;
			*indexAt++ = newIndex - 1 - vertexCountInOneRow;

			*indexAt++ = newIndex - 1;
			*indexAt++ = newIndex;
			*indexAt++ = newIndex - vertexCountInOneRow;

			u += du;
		}

		//close the ring
		*indexAt++ = index - 1;
		*indexAt++ = firstIndexInRow - vertexCountInOneRow;
		*indexAt++ = index - 1 - vertexCountInOneRow;

		*indexAt++ = index - 1;
		*indexAt++ = firstIndexInRow;
		*indexAt++ = firstIndexInRow - vertexCountInOneRow;

		v += dv;
	}

	//build south hat
	u32 southPoleIndex = index++;
	ASSERT(index == vertexCount);

	Vertex* southPole = vertices + southPoleIndex;
	southPole->position = { 0.f, -1.f, 0.f };
	southPole->normal = southPole->position;
	southPole->uv = { 0.f, 1.f };

	u32 firstIndexInLastRow = southPoleIndex - vertexCountInOneRow;
	u32 indexInLastRow = firstIndexInLastRow + 1;

	for (u32 uIndex = 0; uIndex < quadCountU - 1; ++uIndex)
	{
		*indexAt++ = southPoleIndex;
		*indexAt++ = indexInLastRow;
		*indexAt++ = indexInLastRow - 1;

		++indexInLastRow;
	}

	//close the ring
	*indexAt++ = southPoleIndex;
	*indexAt++ = firstIndexInLastRow;
	*indexAt++ = indexInLastRow - 1;

	ASSERT(indices + indexCount == indexAt);

	result.vertices = vertices;
	result.vertexCount = vertexCount;
	result.indices = indices;
	result.indexCount = indexCount;

	return result;
}

static Mesh createCubeMesh(MemoryArena* arena)
{
	Mesh result = {};

	u32 vertexCount = 36;
	Vertex* vertices = pushArray(arena, vertexCount, Vertex);
	vertices[0] = { {-1.f, -1.f, -1.f },{-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f} };
	vertices[1] = { {-1.f, 1.f, -1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f} };
	vertices[2] = { {-1.f, -1.f, 1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f} };
				   
	vertices[3] = { {-1.f, 1.f, 1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f} };
	vertices[4] = { {-1.f, -1.f, 1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f} };
	vertices[5] = { {-1.f, 1.f, -1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f} };
				   
	vertices[6] = { {1.f, -1.f, -1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f} };
	vertices[7] = { {1.f, -1.f, 1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f} };
	vertices[8] = { {1.f, 1.f, -1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f} };
				   
	vertices[9] = { {1.f, 1.f, 1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f} };
	vertices[10] = { {1.f, 1.f, -1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f} };
	vertices[11] = { {1.f, -1.f, 1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f} };
		
	vertices[12] = { {-1.f, -1.f, -1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f} };
	vertices[13] = { {-1.f, -1.f, 1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f} };
	vertices[14] = { {1.f, -1.f, -1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f} };
		
	vertices[15] = { {1.f, -1.f, 1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f} };
	vertices[16] = { {1.f, -1.f, -1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f} };
	vertices[17] = { {-1.f, -1.f, 1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f} };
		
	vertices[18] = { {-1.f, 1.f, -1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f} };
	vertices[19] = { {1.f, 1.f, -1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f} };
	vertices[20] = { {-1.f, 1.f, 1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f} };
		
	vertices[21] = { {1.f, 1.f, 1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f} };
	vertices[22] = { {-1.f, 1.f, 1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f} };
	vertices[23] = { {1.f, 1.f, -1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f} };
		
	vertices[24] = { {-1.f, -1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f} };
	vertices[25] = { {1.f, -1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 0.f} };
	vertices[26] = { {-1.f, 1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 1.f} };
		
	vertices[27] = { {1.f, 1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 1.f} };
	vertices[28] = { {-1.f, 1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 1.f} };
	vertices[29] = { {1.f, -1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 0.f} };
		
	vertices[30] = { {-1.f, -1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f} };
	vertices[31] = { {-1.f, 1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 1.f} };
	vertices[32] = { {1.f, -1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 0.f} };
		
	vertices[33] = { {1.f, 1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 1.f} };
	vertices[34] = { {1.f, -1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {1.f, 0.f} };
	vertices[35] = { {-1.f, 1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 1.f} };

	u32* indices = pushArray(arena, vertexCount, u32);
	for (u32 index = 0; index < vertexCount; ++index)
	{
		indices[index] = index;
	}

	result.vertexCount = vertexCount;
	result.vertices = vertices;
	result.indexCount = vertexCount;
	result.indices = indices;

	return result;
}

static v4 unpackColor(u32 color)
{
	v4 result =
	{
		(f32)((color >> 0) & 255),
		(f32)((color >> 8) & 255),
		(f32)((color >> 16) & 255),
		(f32)((color >> 24) & 255)
	};
	result *= (1.f / 255.f);

	return result;
}

static u32 packColor(v4 color)
{
	color = saturate(color);
	u32 r = (u32)(color.r * 255.f);
	u32 g = (u32)(color.g * 255.f);
	u32 b = (u32)(color.b * 255.f);
	u32 a = (u32)(color.a * 255.f);

	return (a << 24) | (b << 16) | (g << 8) | (r << 0);
}

static void generateMipLevels(MemoryArena* arena, Image2DLod* image, u32 pitchAlign)
{

	ASSERT(image->lodCount == 1);

	u32 width = image->lod[0].width;
	u32 height = image->lod[0].height;
	while (width > 1 || height > 1)
	{
		width = MAX(1, width >> 1);
		height = MAX(1, height >> 1);

		ASSERT(image->lodCount < ARRAY_SIZE(image->lod));
		Image2D* prevImage = image->lod + (image->lodCount - 1);

		Image2D* newImage = image->lod + image->lodCount++;
		*newImage = pushImage2D(arena, width, height, u32, pitchAlign);
		u8* row = newImage->memory;
		for (u32 y = 0; y < newImage->height; ++y)
		{
			f32 v = ((f32)y + 0.5f) * (f32)prevImage->height / (f32)newImage->height;
			v -= 0.5f;
			ASSERT(v >= 0.f);

			u32 v0 = (u32)v;
			u32 v1 = v0 + 1;
			v1 = MIN(v1, prevImage->height);
			f32 dv = v - (f32)v0;

			u32* pixel = (u32*)row;
			for (u32 x = 0; x < newImage->width; ++x)
			{
				f32 u = ((f32)x + 0.5f) * (f32)prevImage->width / (f32)newImage->width;
				u-= 0.5f;
				ASSERT(u >= 0.f);

				u32 u0 = (u32)u;
				u32 u1 = u0 + 1;
				u1 = MIN(u1, prevImage->width);
				f32 du = u - (f32)u0;

				v4 c00 = unpackColor(fetchSample(prevImage, u0, v0, u32));
				v4 c10 = unpackColor(fetchSample(prevImage, u1, v0, u32));
				v4 c01 = unpackColor(fetchSample(prevImage, u0, v1, u32));
				v4 c11 = unpackColor(fetchSample(prevImage, u1, v1, u32));

				v4 a = lerp(c00, c10, du);
				v4 b = lerp(c01, c11, du);
				v4 c = lerp(a, b, dv);

				*pixel++ = packColor(c);
			}
			row += newImage->pitch;
		}
	}
}


int CALLBACK WinMain(
	HINSTANCE hInstance,
	HINSTANCE hPrevInstance,
	LPSTR     lpCmdLine,
	int       nShowCmd
)
{
	ASSERT(QueryPerformanceFrequency(&g_perfCounterFrequency) == TRUE);
	umm storageSize = 256 * 1024 * 1024;
	MemoryArena arena = createMemoryArena(Win32AllocateMemory(storageSize), storageSize);

#ifdef _DEBUG
	ID3D12Debug* debugInterface = 0;
	ASSERT(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)) == S_OK);
	debugInterface->EnableDebugLayer();
	//debugInterface->SetEnableGPUBasedValidation(TRUE);
#endif


	WNDCLASSA wndClass = {};
	wndClass.style = CS_HREDRAW | CS_VREDRAW;
	wndClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wndClass.lpfnWndProc = &Win32WindowCallback;
	wndClass.hInstance = hInstance;
	wndClass.lpszClassName = "MainWindowClass";
	wndClass.hCursor = LoadCursorA(hInstance, IDC_ARROW);
	ASSERT(RegisterClassA(&wndClass));

	HWND window = CreateWindowExA(
		0,
		"MainWindowClass",
		"D3D12 Test",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		0,
		0,
		hInstance,
		0
	);
	ASSERT(window);

	HDC dc = GetDC(window);
	ShowWindow(window, SW_SHOW);

	//create dxgi factory
	IDXGIFactory4* dxgiFactory = 0;
	UINT adapterFlags = 0;
#ifdef  _DEBUG
	adapterFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	ASSERT(CreateDXGIFactory2(adapterFlags, IID_PPV_ARGS(&dxgiFactory)) == S_OK);

	//create dxgi adapter
	IDXGIAdapter1* adapter1 = 0;
	IDXGIAdapter4* adapter4 = 0;
	umm maxDedicatedVideoMemory = 0;
	for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &adapter1) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		DXGI_ADAPTER_DESC1 adapterDesc1;
		adapter1->GetDesc1(&adapterDesc1);
		if ((adapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
			SUCCEEDED(D3D12CreateDevice(adapter1, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), 0)) &&
			adapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory)
		{
			maxDedicatedVideoMemory = adapterDesc1.DedicatedVideoMemory;
			ASSERT(adapter1->QueryInterface(IID_PPV_ARGS(&adapter4)) == S_OK);
		}
	}

	//create device
	ID3D12Device2* device = 0;
	ASSERT(D3D12CreateDevice(adapter4, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)) == S_OK);

	//enable debug messages
#ifdef _DEBUG
	ID3D12InfoQueue* infoQueue = 0;
	ASSERT(device->QueryInterface(IID_PPV_ARGS(&infoQueue)) == S_OK);
	infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
	infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
	infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

	// Suppress whole categories of messages
	//D3D12_MESSAGE_CATEGORY categories[] = {};
	
	// Suppress messages based on their severity level
	D3D12_MESSAGE_SEVERITY severities[] =
	{
		D3D12_MESSAGE_SEVERITY_INFO
	};

	// Suppress individual messages by their ID
	D3D12_MESSAGE_ID denyIds[] = {
		D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,   // I'm really not sure how to avoid this message.
		D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,                         // This warning occurs when using capture frame while graphics debugging.
		D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,                       // This warning occurs when using capture frame while graphics debugging.
	};

	D3D12_INFO_QUEUE_FILTER newFilter = {};
	//newFilter.DenyList.NumCategories = ARRAY_SIZE(Categories);
	//newFilter.DenyList.pCategoryList = Categories;
	newFilter.DenyList.NumSeverities = ARRAY_SIZE(severities);
	newFilter.DenyList.pSeverityList = severities;
	newFilter.DenyList.NumIDs = ARRAY_SIZE(denyIds);
	newFilter.DenyList.pIDList = denyIds;

	ASSERT(infoQueue->PushStorageFilter(&newFilter) == S_OK);
#endif

	//create command queue
	ID3D12CommandQueue* commandQueue = 0;
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	ASSERT(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)) == S_OK);

	UINT swapChainWidth = 1920;
	UINT swapChainHeight = 1080;
	constexpr UINT backBufferCount = 3;

	//create swap chain
	IDXGISwapChain4* swapChain = 0;
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = swapChainWidth;
	swapChainDesc.Height = swapChainHeight;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.Stereo = FALSE;
	swapChainDesc.SampleDesc = { 1, 0 };
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = backBufferCount;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	swapChainDesc.Flags = 0;

	IDXGISwapChain1* swapChain1 = 0;
	ASSERT(dxgiFactory->CreateSwapChainForHwnd(
		commandQueue,
		window,
		&swapChainDesc,
		0,
		0,
		&swapChain1
	) == S_OK);
	ASSERT(dxgiFactory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER) == S_OK);
	ASSERT(swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain)) == S_OK);

	ID3D12DescriptorHeap* rtvHeap = createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, backBufferCount);

	//create rtvs for backbuffer
	UINT rtvDescriptorHandleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
	
	ID3D12Resource* backBuffers[backBufferCount] = {};
	for (UINT backBufferIndex = 0; backBufferIndex < ARRAY_SIZE(backBuffers); ++backBufferIndex)
	{
		ID3D12Resource* backBuffer = 0;
		ASSERT(swapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer)) == S_OK);
		device->CreateRenderTargetView(backBuffer, 0, rtvHandle);
		backBuffers[backBufferIndex] = backBuffer;
		rtvHandle.ptr += rtvDescriptorHandleSize;
	}

	//create command allocators
	ID3D12CommandAllocator* commandAllocators[backBufferCount] = {};
	for (UINT commandAllocatorIndex = 0; commandAllocatorIndex < ARRAY_SIZE(commandAllocators); ++commandAllocatorIndex)
	{
		ID3D12CommandAllocator* commandAllocator = 0;
		ASSERT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)) == S_OK);
		commandAllocators[commandAllocatorIndex] = commandAllocator;
	}

	//create command list
	ID3D12GraphicsCommandList* commandList = 0;
	ASSERT(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[swapChain->GetCurrentBackBufferIndex()], 0, IID_PPV_ARGS(&commandList)) == S_OK);
	commandList->Close();

	//create fences
	ID3D12Fence* fence = 0;
	ASSERT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) == S_OK);

	//create fenceEvents
	HANDLE fenceEvent = 0;
	fenceEvent = CreateEventA(0, FALSE, FALSE, 0);
	ASSERT(fenceEvent);

	//create root signature
	ID3DBlob* signatureBlob = 0;
	ID3DBlob* errorBlob = 0;
	ID3D12RootSignature* rootSignature = 0;
	{
		D3D12_DESCRIPTOR_RANGE texRange = {};
		texRange.BaseShaderRegister = 0;
		texRange.RegisterSpace = 0;
		texRange.NumDescriptors = 2;
		texRange.OffsetInDescriptorsFromTableStart = 0;
		texRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

		D3D12_ROOT_PARAMETER rootParams[] = 
		{
			InitAsConstantsBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX),
			InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_ALL),
			InitAsConstantsBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL),
		};
		D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
		samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
		samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[1].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		samplers[1].ShaderRegister = 1;


		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
		desc.NumParameters = ARRAY_SIZE(rootParams);
		desc.pParameters = rootParams;
		desc.NumStaticSamplers = ARRAY_SIZE(samplers);
		desc.pStaticSamplers = samplers;

		D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
		featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0; //setting it to 1_1 leads to error, I don't know why...
		if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
		{
			featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
		}
		ASSERT(D3D12SerializeRootSignature(&desc, featureData.HighestVersion, &signatureBlob, &errorBlob) == S_OK);
		ASSERT(device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
			IID_PPV_ARGS(&rootSignature)) == S_OK);
		signatureBlob->Release();
	}

	GraphicsPipeline graphicsPipeline = {};
	{
		graphicsPipeline.inputElementDescs[0] = createInputElementDesc("POSITION", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, position));
		graphicsPipeline.inputElementDescs[1] = createInputElementDesc("NORMAL", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, normal));
		graphicsPipeline.inputElementDescs[2] = createInputElementDesc("TANGENT", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, tangent));
		graphicsPipeline.inputElementDescs[3] = createInputElementDesc("BITANGENT", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, bitangent));
		graphicsPipeline.inputElementDescs[4] = createInputElementDesc("UV", DXGI_FORMAT_R32G32_FLOAT, offsetof(Vertex, uv));
		

		graphicsPipeline.desc.InputLayout = { graphicsPipeline.inputElementDescs, 5 };
		graphicsPipeline.desc.pRootSignature = rootSignature;
		
		graphicsPipeline.desc.DepthStencilState.DepthEnable = true;
		graphicsPipeline.desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		graphicsPipeline.desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		graphicsPipeline.desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

		graphicsPipeline.desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		graphicsPipeline.desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		graphicsPipeline.desc.RasterizerState.FrontCounterClockwise = true;
		graphicsPipeline.desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		graphicsPipeline.desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		graphicsPipeline.desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		graphicsPipeline.desc.RasterizerState.DepthClipEnable = TRUE;
		graphicsPipeline.desc.RasterizerState.MultisampleEnable = FALSE;
		graphicsPipeline.desc.RasterizerState.AntialiasedLineEnable = FALSE;
		graphicsPipeline.desc.RasterizerState.ForcedSampleCount = 0;
		graphicsPipeline.desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		graphicsPipeline.desc.BlendState.RenderTarget[0] =
		{
			FALSE,FALSE,
			D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
			D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
			D3D12_LOGIC_OP_NOOP,
			D3D12_COLOR_WRITE_ENABLE_ALL,
		};

		graphicsPipeline.desc.SampleMask = UINT_MAX;
		graphicsPipeline.desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		graphicsPipeline.desc.NumRenderTargets = 1;
		graphicsPipeline.desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		graphicsPipeline.desc.SampleDesc.Count = 1;

		graphicsPipeline.shaders[SHADER_VERTEX].name = L"simpleShaderVS.cso";
		graphicsPipeline.shaders[SHADER_PIXEL].name = L"simpleShaderPS.cso";
		
		rebuildGraphicsPipeline(device, &graphicsPipeline);
	}

	//create depth, vertex, index buffer
	TempMemory tempMem = startTempMemory(&arena);
	Mesh cubeMesh = createCubeMesh(&arena);
	Mesh sphereMesh = createSphereMesh(&arena, 512, 512);

	ID3D12Resource* depthBuffer = 0;

	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil = { 1.f, 0 };
	ASSERT(device->CreateCommittedResource(
		&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&createResourceDescTex2D(DXGI_FORMAT_D32_FLOAT, swapChainWidth, swapChainHeight, 0, 
			D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&clearValue,
		IID_PPV_ARGS(&depthBuffer)
	) == S_OK);

	ResourceManager resourceManager = ResourceManager::create(device);
	VertexBuffer vb = createVertexBuffer(&resourceManager, sphereMesh.vertexCount * sizeof(Vertex), sizeof(Vertex), sphereMesh.vertices);
	IndexBuffer ib = createIndexBuffer(&resourceManager, sphereMesh.indexCount * sizeof(u32), true, sphereMesh.indices);
	endTempMemory(&tempMem);
	
	ConstantBuffer cbs[backBufferCount] = {};

	for (s32 cbIndex = 0; cbIndex < ARRAY_SIZE(cbs); ++cbIndex)
	{
		cbs[cbIndex] = createConstantBuffer(&resourceManager, sizeof(SceneBuffer), 0);
	}

	ID3D12CommandList* commandListsToSubmit[] = { commandList };

	u64 fenceValue = 0;

	D3D12_VERTEX_BUFFER_VIEW vbv = vb.d12View;
	D3D12_INDEX_BUFFER_VIEW ibv = ib.d12View;
	
	//create dsv heap
	ID3D12DescriptorHeap* dsvHeap = createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);

	//create dsv
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Texture2D.MipSlice = 0;
	device->CreateDepthStencilView(depthBuffer, &dsvDesc, dsvHeap->GetCPUDescriptorHandleForHeapStart());

	//create srv heap
	u32 srvHeapSize = 64;
	ID3D12DescriptorHeap* srvHeap = createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, srvHeapSize, true);
	u32 srvHeapIncrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	
	//create diffuse texture
	u32 texWidth = 4096;
	u32 texHeight = 4096;
	u32 texSize = texWidth * texHeight * 4;
	tempMem = startTempMemory(&arena);
	Image2D heightMap = pushImage2D(&arena, texWidth, texHeight, f32, 0);
	Image2DLod normalMap = {};
	normalMap.lod[0] = pushImage2D(&arena, texWidth, texHeight, u32, 0);
	normalMap.lodCount = 1;
	Image2D grad = pushImage2D(&arena, 64, 64, v2, 0);

	fillWithRandomGradients(&grad, 13);
	clearImage2D(&heightMap);

	u32 tileSize = 1024;
	u32 scale = 1;
	v2 info{};
	for (u32 iter = 0; iter < 10; ++iter)
	{
		info = addPerlinNoise(&heightMap, &grad, { (f32)tileSize, 0.f }, 1.f / (f32)scale);
		tileSize >>= 1;
		scale <<= 1;
	}
	fillNormalMapForHeightMap(&heightMap, &normalMap.lod[0], 0.05f);

	generateMipLevels(&arena, &normalMap, 0);

	//u32* pixel = (u32*)image.memory;
	//for (u32 y = 0; y < texHeight; ++y)
	//{
	//	for (u32 x = 0; x < texWidth; ++x)
	//	{
	//		if (((x % 256) < 128) && ((y % 256) >= 128))
	//		{
	//			fetchSample(&image, x, y, u32) = (255u << 24) | (0u << 16) | (0u << 8) | (255u << 0);
	//		}
	//		else
	//		{
	//			fetchSample(&image, x, y, u32) = (255u << 24) | (0u << 16) | (255u << 8) | (0u << 0);
	//		}
	//	}
	//}

	//normalMap
	ID3D12Resource* normalMapResource = 0;
	D3D12_RESOURCE_DESC normalTexDesc = createResourceDescTex2D(DXGI_FORMAT_R8G8B8A8_UNORM, normalMap.lod[0].width, normalMap.lod[0].height, (u16)normalMap.lodCount);
	ASSERT(device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
		&normalTexDesc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&normalMapResource)) == S_OK);


	TrackedResource normalTex = {};
	normalTex.d12Resource = normalMapResource;
	normalTex.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	uploadToTextureLod(&resourceManager, &normalTex, &normalMap, DXGI_FORMAT_R8G8B8A8_UNORM);
	
	//heightMap
	ID3D12Resource* heightMapResource = 0;
	D3D12_RESOURCE_DESC heightTexDesc = createResourceDescTex2D(DXGI_FORMAT_R32_FLOAT, texWidth, texHeight, 0);
	ASSERT(device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
		&heightTexDesc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&heightMapResource)) == S_OK);

	TrackedResource heightTex = {};
	heightTex.d12Resource = heightMapResource;
	heightTex.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	uploadToTexture(&resourceManager, &heightTex, &heightMap, DXGI_FORMAT_R32_FLOAT);

	
	
	endTempMemory(&tempMem);

	//create srv for textures
	D3D12_CPU_DESCRIPTOR_HANDLE cpuTexView = srvHeap->GetCPUDescriptorHandleForHeapStart();
	device->CreateShaderResourceView(normalMapResource, 0, cpuTexView);
	cpuTexView.ptr += srvHeapIncrementSize;
	device->CreateShaderResourceView(heightMapResource, 0, cpuTexView);
	
	u64 frameResourcesAvailableFenceValues[backBufferCount] = {};

	LARGE_INTEGER frameStart = Win32GetWallClock();

	//create model cbs
	constexpr u32 modelCountX = 10;
	constexpr u32 modelCountY = 10;
	constexpr u32 modelCountZ = 10;

	ModelBuffer modelBuffers[modelCountZ][modelCountY][modelCountX];

	//random coloring
	{
		ModelBuffer* modelBuffer = &modelBuffers[0][0][0];
		for (u32 modelBufferIndex = 0; modelBufferIndex < modelCountX*modelCountY*modelCountZ; ++modelBufferIndex)
		{
			modelBuffer->color =
			{
				(f32)rand() / (f32)RAND_MAX,
				(f32)rand() / (f32)RAND_MAX,
				(f32)rand() / (f32)RAND_MAX,
				1.f
			};
			modelBuffer->metalness = (f32)rand() / (f32)RAND_MAX;
			modelBuffer->roughness = (f32)rand() / (f32)RAND_MAX;

			++modelBuffer;
		}
	}

	ConstantBuffer modelCbs[backBufferCount];
	for (u32 modelCbIndex = 0; modelCbIndex < ARRAY_SIZE(modelCbs); ++modelCbIndex)
	{
		modelCbs[modelCbIndex] = createConstantBuffer(&resourceManager, sizeof(modelBuffers), 0);
	}

	f32 angle = 0.f;
	f32 angleVelocity = pi32 / 2.f;

	Input input = {};

	m4 proj = projection((f32)swapChainWidth / (f32)swapChainHeight, pi32 * 0.5f, 0.1f, 500.f);

	f32 walkSpeed = 8.f;
	f32 elevationSpeed = 8.f;
	f32 rotationSpeed = pi32 * 0.25f;
	v3 cameraPos = {};
	v2 cameraRot = {};
	v3 lightPos = {};
	f32 maxCameraRotXAbs = pi32 * 0.5f * 0.9f;

	f32 dt = 0;
	while (g_running)
	{
		updateInput(&input);
		if (shouldRebuildGraphicspipeline(&graphicsPipeline))
		{
			flushCommandQueue(commandQueue, fence, &fenceValue);
			rebuildGraphicsPipeline(device, &graphicsPipeline);
		}

		Win32ProcessPendingMessages(&input);

		//set cameraModel, update it from input
		m4 cameraModel = {};
		{
			v2 drot = {};
			if (input.left.isDown) { drot.y += 1.f; }
			if (input.right.isDown) { drot.y -= 1.f; }
			if (input.up.isDown) { drot.x += 1.f; }
			if (input.down.isDown) { drot.x -= 1.f; }

			if (drot.x != 0.f || drot.y != 0.f)
			{
				drot = rotationSpeed * dt * normalize(drot);
				cameraRot += drot;
				if (cameraRot.x < -maxCameraRotXAbs)
				{
					cameraRot.x = -maxCameraRotXAbs;
				}
				else if (maxCameraRotXAbs < cameraRot.x)
				{
					cameraRot.x = maxCameraRotXAbs;
				}
				if (cameraRot.y < -pi32)
				{
					cameraRot.y += 2.f * pi32;
				}
				else if (pi32 < cameraRot.y)
				{
					cameraRot.y -=2.f *  pi32;
				}
			}

			cameraModel = rotationY(cameraRot.y) * rotationX(cameraRot.x);
			ASSERT(cameraModel.xAxis.y == 0.f);
			
			v3 moveInput = {};
			if (input.W.isDown) { moveInput.z -= 1.f; }
			if (input.S.isDown) { moveInput.z += 1.f; }
			if (input.A.isDown) { moveInput.x -= 1.f; }
			if (input.D.isDown) { moveInput.x += 1.f; }

			if (moveInput.x != 0.f || moveInput.z != 0.f)
			{
				v3 dwalk = moveInput.x * cameraModel.xAxis + moveInput.z * cameraModel.zAxis;
				dwalk.y = 0.f;
				dwalk = walkSpeed * dt * normalizeSafe(dwalk);
				cameraPos += dwalk;
			}

			f32 elevation = 0.f;
			if (input.space.isDown) { elevation += 1.f; }
			if (input.C.isDown) { elevation -= 1.f; }

			if (elevation != 0.f)
			{
				elevation *= elevationSpeed * dt;
				cameraPos.y += elevation;
			}
			cameraModel.translation = cameraPos;

		}

		//update model matrices
		{
			angle += angleVelocity * dt;
			if (angle > 200.f*3.14)
			{
				angle -= 200.f*3.14f;
			}
			m4 model = identityM4();
			model = model * rotationY(angle*0.125f);
			model = model * rotationX(angle*0.125f);
			model.translation = { 0.f, 0.f, -20.f };

			for (s32 modelIndexZ = 0; modelIndexZ < modelCountZ; ++modelIndexZ)
			{
				for (s32 modelIndexY = 0; modelIndexY < modelCountY; ++modelIndexY)
				{
					for (s32 modelIndexX = 0; modelIndexX < modelCountX; ++modelIndexX)
					{
						ModelBuffer* modelBuffer = &modelBuffers[modelIndexZ][modelIndexY][modelIndexX];
						modelBuffer->model = translation(3.f*V3(modelIndexX, modelIndexY, -modelIndexZ)) * model;
						modelBuffer->scale = V3(1.f);// { 1.3f, 1.f, 0.5f };
						modelBuffer->vertexDisplacement = 0.5f + 0.5f*sinf(angle*0.1f);
						
					}
				}
			}
			//lightPos = (rotationY(angle * 0.125f) * v4 { 15.f, 0.f, 15.f, 1.f }).xyz;
			//modelBuffers[0][0][0].model = translation(lightPos);
		}

		UINT currentBackBufferIndex = swapChain->GetCurrentBackBufferIndex();
		waitForFenceValue(fence, frameResourcesAvailableFenceValues[currentBackBufferIndex]);
		ID3D12CommandAllocator* commandAllocator = commandAllocators[currentBackBufferIndex];
		ID3D12Resource* backBuffer = backBuffers[currentBackBufferIndex];

		ASSERT(commandAllocator->Reset() == S_OK);
		ASSERT(commandList->Reset(commandAllocator, 0) == S_OK);

		//transition backbuffer state
		{
			D3D12_RESOURCE_BARRIER barrier = transition(
				backBuffer,
				D3D12_RESOURCE_STATE_PRESENT,
				D3D12_RESOURCE_STATE_RENDER_TARGET
			);
			commandList->ResourceBarrier(1, &barrier);
		}
		f32 clearColor[4] = { 0.4f, 0.6f, 0.9f, 1.0f };
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
		rtv.ptr += currentBackBufferIndex * rtvDescriptorHandleSize;
		D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();

		D3D12_VIEWPORT vp = {};
		vp.Width = (f32)swapChainWidth;
		vp.Height = (f32)swapChainHeight;
		vp.TopLeftX = 0.f;
		vp.TopLeftY = 0.f;
		vp.MinDepth = 0.f;
		vp.MaxDepth = 1.f;

		D3D12_RECT scissor = {};
		scissor.bottom = LONG_MAX;
		scissor.left = 0;
		scissor.right = LONG_MAX;
		scissor.top = 0;

		commandList->ClearRenderTargetView(rtv, clearColor, 0, 0);
		commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, 0);

		commandList->SetPipelineState(graphicsPipeline.pipeline);
		commandList->SetGraphicsRootSignature(rootSignature);
		commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		b32 canBeDrawn = true;
		if (vb.resource.stateAfterModification != D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER )
		{
			if (resourceReady(&vb.resource))
			{
				markModify(&vb.resource, commandQueue, commandList, fence, fenceValue + 1,
					D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			}
			else
			{
				canBeDrawn = false;
			}
		}
		else
		{
			markUse(&vb.resource, commandQueue, fence, fenceValue + 1);
		}

		if (ib.resource.stateAfterModification != D3D12_RESOURCE_STATE_INDEX_BUFFER)
		{
			if (resourceReady(&ib.resource))
			{
				markModify(&ib.resource, commandQueue, commandList, fence, fenceValue + 1,
					D3D12_RESOURCE_STATE_INDEX_BUFFER);
			}
			else
			{
				canBeDrawn = false;
			}
		}
		else
		{
			markUse(&ib.resource, commandQueue, fence, fenceValue + 1);
		}

		if (normalTex.stateAfterModification != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		{
			if (resourceReady(&normalTex))
			{
				markModify(&normalTex, commandQueue, commandList, fence, fenceValue + 1, 
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			else
			{
				canBeDrawn = false;
			}
		}
		else
		{
			markUse(&normalTex, commandQueue, fence, fenceValue + 1);
		}
		if (heightTex.stateAfterModification != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
		{
			if (resourceReady(&heightTex))
			{
				markModify(&heightTex, commandQueue, commandList, fence, fenceValue + 1,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
			else
			{
				canBeDrawn = false;
			}
		}
		else
		{
			markUse(&heightTex, commandQueue, fence, fenceValue + 1);
		}



		commandList->IASetVertexBuffers(0, 1, &vbv);
		commandList->IASetIndexBuffer(&ibv);
		commandList->SetDescriptorHeaps(1, &srvHeap);
		
		commandList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());

		SceneBuffer sceneBuffer = {};
		m4 view = invertOrtho3Translation(cameraModel);
		m4 projview = proj * view;
		sceneBuffer.projview = projview;
		sceneBuffer.lightPos = lightPos;
		sceneBuffer.viewPos = cameraPos;
		

		uploadToBuffer(&resourceManager, &cbs[currentBackBufferIndex].resource, 0, sizeof(sceneBuffer), &sceneBuffer);
		markModify(&cbs[currentBackBufferIndex].resource, commandQueue, commandList, fence, fenceValue + 1,
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

		uploadToBuffer(&resourceManager, &modelCbs[currentBackBufferIndex].resource, 0, sizeof(modelBuffers), modelBuffers);
		markModify(&modelCbs[currentBackBufferIndex].resource, commandQueue, commandList, fence, fenceValue + 1,
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

		commandList->RSSetViewports(1, &vp);
		commandList->RSSetScissorRects(1, &scissor);
		commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
		commandList->SetGraphicsRootConstantBufferView(0, cbs[currentBackBufferIndex].gpuVirtualAddress);
		
		if (canBeDrawn)
		{
			D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = modelCbs[currentBackBufferIndex].gpuVirtualAddress;
			//for (u32 modelIndexZ = 0; modelIndexZ < modelCountZ; ++modelIndexZ)
			{
				//for (u32 modelIndexY = 0; modelIndexY < modelCountY; ++ modelIndexY)
				{
					for (u32 modelIndexX = 0; modelIndexX < modelCountX; ++modelIndexX)
					{
						commandList->SetGraphicsRootConstantBufferView(2, gpuAddress);
						commandList->DrawIndexedInstanced(sphereMesh.indexCount, 1, 0, 0, 0);
						gpuAddress += sizeof(ModelBuffer);
					}
				}
			}
		}
		else
		{
			int k = 4;
		}

		markModify(&cbs[currentBackBufferIndex].resource, commandQueue, commandList, fence, fenceValue + 1,
			D3D12_RESOURCE_STATE_COPY_DEST);

		markModify(&modelCbs[currentBackBufferIndex].resource, commandQueue, commandList, fence, fenceValue + 1,
			D3D12_RESOURCE_STATE_COPY_DEST);

		//transition backbuffer state
		{
			D3D12_RESOURCE_BARRIER barrier = transition(
				backBuffer,
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PRESENT
			);
			commandList->ResourceBarrier(1, &barrier);
		}

		ASSERT(commandList->Close() == S_OK);

		commandQueue->ExecuteCommandLists(ARRAY_SIZE(commandListsToSubmit), commandListsToSubmit);
		incrementAndSignal(commandQueue, fence, &fenceValue);

		ASSERT(swapChain->Present(1, 0) == S_OK);

		frameResourcesAvailableFenceValues[currentBackBufferIndex] = fenceValue;

		LARGE_INTEGER frameEnd = Win32GetWallClock();
		dt = Win32GetSecondsElapsed(frameStart, frameEnd);
		frameStart = frameEnd;
		char buffer[256];
		sprintf_s(buffer, "Frame time: %fms\n", dt*1000.f);
		OutputDebugStringA(buffer);
	}

	return 0;
}