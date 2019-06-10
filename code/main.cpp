/* TODO

fix uv spheres,
tessellating sphere
optimize mip levels
add some profiling
optimize perlin noise computing
temporal and volume Perlin noise

particles snap to implicit surfaces
have some fun with bezier curves

better graphics, not just point lights
show some debug info (frame time, vertex count, root signature refresh)

*/



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

struct DebugTimeInfo
{
	u64 start;
	u64 end;
	char* tag;
	u32 hitCount;
};
struct DebugInfo
{
	DebugTimeInfo timeInfos[256];
	u32 timeInfoCount;
};

struct DebugTimer
{
	DebugTimeInfo* info;
	DebugTimer(DebugInfo* debugInfo, char* tag)
	{
		ASSERT(debugInfo->timeInfoCount < ARRAY_SIZE(debugInfo->timeInfos));
		info = debugInfo->timeInfos + debugInfo->timeInfoCount++;
		info->tag = tag;
		info->hitCount = 1;
		info->start = __rdtsc();
	}
	~DebugTimer()
	{
		if (info)
		{
			info->end = __rdtsc();
		}
	}
	void end(u32 hitCount = 1)
	{
		ASSERT(hitCount > 0);
		info->end = __rdtsc();
		info->hitCount = hitCount;
		info = 0;
	}
};

global DebugInfo g_debugInfo;

#define __TIMED_BLOCK(count, tag) DebugTimer debugTimer##count (&g_debugInfo, tag);
#define _TIMED_BLOCK(count, tag) __TIMED_BLOCK(count, tag)
#define TIMED_BLOCK() _TIMED_BLOCK(__COUNTER__, __FUNCTION__)
#define START_TIMER(tag) DebugTimer debugTimer##tag (&g_debugInfo, #tag);
#define END_TIMER(tag, ...) debugTimer##tag .end(##__VA_ARGS__);


static void updateDebugInfo()
{
	char buff[256];
	for (u32 timeInfoIndex = 0; timeInfoIndex < g_debugInfo.timeInfoCount; ++timeInfoIndex)
	{
		DebugTimeInfo* timeInfo = g_debugInfo.timeInfos + timeInfoIndex;
		u64 duration = timeInfo->end - timeInfo->start;
		_snprintf_s(buff, ARRAY_SIZE(buff), "Timer: %12llucy/h, %s \n", duration/timeInfo->hitCount, timeInfo->tag);
		OutputDebugStringA(buff);
	}
	g_debugInfo.timeInfoCount = 0;
}

struct TicketMutex
{
	u64 volatile nextTicket;
	u64 volatile servingTicket;
};

inline void beginTicketMutex(TicketMutex* mutex)
{
	u64 ticket = _InterlockedIncrement64((volatile LONGLONG*)&mutex->nextTicket) - 1;
	if (ticket != mutex->servingTicket)
	{
		START_TIMER(WaitingForTicketMutex);
		busyWaitWhile(ticket != mutex->servingTicket);
		END_TIMER(WaitingForTicketMutex);
	}
}
inline void endTicketMutex(TicketMutex* mutex)
{
	_InterlockedIncrement64((volatile LONGLONG*)&mutex->servingTicket);
}

typedef void(WorkQueueCallback)(void*);

struct WorkQueueEntry
{
	void* data;
	WorkQueueCallback* callback;
};

struct WorkQueue //NOTE: one producer, multiple consumer
{
	u32 volatile nextEntryToWrite;
	u32 volatile nextEntryToRead;
	u32 volatile currentlyWorkingThreadCount;
	WorkQueueEntry entries[256];
	HANDLE semaphore;
};

static void pushEntry(WorkQueue* queue, void* data, WorkQueueCallback* callback)
{
	u32 nextEntryToWrite = queue->nextEntryToWrite;
	u32 newNextEntryToWrite = (nextEntryToWrite + 1) % ARRAY_SIZE(queue->entries);
	
	if (newNextEntryToWrite == queue->nextEntryToRead)
	{
		TIMED_BLOCK();
		busyWaitWhile(newNextEntryToWrite == queue->nextEntryToRead);
	}

	WorkQueueEntry* entry = queue->entries + queue->nextEntryToWrite;
	entry->callback = callback;
	entry->data = data;
	
	_WriteBarrier();
	queue->nextEntryToWrite = newNextEntryToWrite;

	ReleaseSemaphore(queue->semaphore, 1, 0);
}

static WorkQueueEntry popEntry(WorkQueue* queue)
{
	WorkQueueEntry result = {};
	bool success = false;
	while (!success)
	{
		u32 nextEntryToRead = queue->nextEntryToRead;
		u32 newNextEntryToRead = (nextEntryToRead + 1) % ARRAY_SIZE(queue->entries);
		if (nextEntryToRead != queue->nextEntryToWrite)
		{
			result = queue->entries[nextEntryToRead];
			u32 nextEntryToRead2 = _InterlockedCompareExchange(
				(volatile LONG*)&queue->nextEntryToRead,
				(LONG)newNextEntryToRead,
				nextEntryToRead
			);

			if (nextEntryToRead == nextEntryToRead2)
			{
				success = true;
			}
		}
		else //queue is empty
		{
			result = {};
			success = true;
		}
	}

	return result;
}

static DWORD threadProc(LPVOID lpParam)
{
	WorkQueue* queue = (WorkQueue*)lpParam;
	srand((u32)(umm)&queue);

	while (1)
	{
		DWORD waitResult = WaitForSingleObject(queue->semaphore, 0);
		if (waitResult == WAIT_TIMEOUT) //queue looks empty, go to sleep
		{
			ASSERT(queue->currentlyWorkingThreadCount > 0);
			_InterlockedDecrement((volatile LONG*)&queue->currentlyWorkingThreadCount);
			waitResult = WaitForSingleObject(queue->semaphore, INFINITE);
			_InterlockedIncrement((volatile LONG*)&queue->currentlyWorkingThreadCount);
		}
		ASSERT(waitResult == WAIT_OBJECT_0);

		WorkQueueEntry entry = popEntry(queue);
		ASSERT(entry.callback); //otherwise the queue was empty, but that shouldn't happen, since we waited for the semaphore
		entry.callback(entry.data);
	}
	return 0;
}

static void flushQueue(WorkQueue* queue)
{
	while (queue->nextEntryToRead != queue->nextEntryToWrite)
	{
		DWORD waitResult = WaitForSingleObject(queue->semaphore, 0);
		if (waitResult == WAIT_OBJECT_0)
		{
			WorkQueueEntry entry = popEntry(queue);
			ASSERT(entry.callback);
			entry.callback(entry.data);
		}
		else
		{
			ASSERT(waitResult == WAIT_TIMEOUT);
		}
	}
	while (queue->currentlyWorkingThreadCount != 0);
}

static void initWorkQueue(WorkQueue* queue, u32 threadCount)
{
	queue->nextEntryToRead = 0;
	queue->nextEntryToWrite = 0;
	queue->currentlyWorkingThreadCount = threadCount;

	queue->semaphore = CreateSemaphoreA(0, 0, ARRAY_SIZE(queue->entries), 0);

	for (u32 threadIndex = 0; threadIndex < threadCount; ++threadIndex)
	{
		DWORD threadID;
		HANDLE threadHandle = CreateThread(0, 0, threadProc, queue, 0, &threadID);
		CloseHandle(threadHandle);
	}
}


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
		ASSERT(fence->GetCompletedValue() >= fenceValue);
		CloseHandle(fenceEvent);
	}
}

static void incrementAndSignal(ID3D12CommandQueue* commandQueue, ID3D12Fence* fence, u64* fenceValue)
{
	++(*fenceValue);
	ASSERT(commandQueue->Signal(fence, *fenceValue) == S_OK);
}

struct CommandQueue
{
	ID3D12CommandQueue* d12commandQueue;
	u64 lastSignaledFenceValue;
	ID3D12Fence* d12fence;
};

static void signalNext(CommandQueue* commandQueue)
{
	incrementAndSignal(commandQueue->d12commandQueue, commandQueue->d12fence, &commandQueue->lastSignaledFenceValue);
}

static void flushCommandQueue(ID3D12CommandQueue* commandQueue, ID3D12Fence* fence, u64* fenceValue)
{
	incrementAndSignal(commandQueue, fence, fenceValue);
	waitForFenceValue(fence, *fenceValue);
}


static void flushCommandQueue(CommandQueue* commandQueue)
{
	flushCommandQueue(commandQueue->d12commandQueue, commandQueue->d12fence, &commandQueue->lastSignaledFenceValue);
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
		if (fenceValue <= fenceSlot->requiredFenceValue)
		{
			result = true;
		}
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
			if (fenceSlot->d12Fence == fence)
			{
				fenceSlot->requiredFenceValue = MAX(fenceSlot->requiredFenceValue, requiredFenceValue);
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

static void markUse(TrackedResource* resource, CommandQueue* commandQueue)
{
	markUse(resource, commandQueue->d12commandQueue, commandQueue->d12fence, commandQueue->lastSignaledFenceValue + 1);
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

static void markModify(TrackedResource* resource, CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList,
	D3D12_RESOURCE_STATES stateToModify)
{
	markModify(resource, commandQueue->d12commandQueue, commandList, commandQueue->d12fence,
		commandQueue->lastSignaledFenceValue + 1, stateToModify);
}

static b32 resourceReady(TrackedResource* resource, CommandQueue* commandQueue = 0)
{
	b32 result = fenceSlotEmptyOrValueReached(&resource->modifyFenceSlot);
	if (!result && commandQueue && 
		(resource->modifyFenceSlot.d12Fence == commandQueue->d12fence && 
			resource->modifyFenceSlot.requiredFenceValue <= commandQueue->lastSignaledFenceValue+1))
	{
		//the last modification will be done by this commandQueue, so from the point of this command queue, the resource is ready
		result = true;
	}

	return result;
}

struct Button
{
	b32 wasDown;
	b32 isDown;
};

static b32 wasPressed(Button* button)
{
	return button->isDown && !button->wasDown;
}

static b32 wasReleased(Button* button)
{
	return !button->isDown && button->wasDown;
}

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
	Button Q;
	Button tab;
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
#define RESOURCE_MANAGER_UPLOAD_HEAP_SIZE (256*1024*1024)
struct ResourceManager // call it copy engine?
{
	UploadHeap uploadHeaps[RESOURCE_MANAGER_RING_SIZE];
	CommandAllocator commandAllocators[RESOURCE_MANAGER_RING_SIZE];
	CommandList commandList;
	CommandQueue commandQueue;
	u32 currentRingIndex;
	ID3D12Device2* device;
};

static void _advanceRing(ResourceManager* resourceManager)
{
	TIMED_BLOCK();

	resourceManager->uploadHeaps[resourceManager->currentRingIndex].d12Resource->Unmap(0, 0);
	resourceManager->currentRingIndex = (resourceManager->currentRingIndex + 1) % RESOURCE_MANAGER_RING_SIZE;

	waitForFenceValue(resourceManager->commandQueue.d12fence, resourceManager->commandAllocators[resourceManager->currentRingIndex].requiredFenceValueForReset);
	resourceManager->commandAllocators[resourceManager->currentRingIndex].d12CommandAllocator->Reset();
	
	void* mappedHeap = 0;
	resourceManager->uploadHeaps[resourceManager->currentRingIndex].d12Resource->Map(0, 0, &mappedHeap);
	ASSERT(mappedHeap);
	resourceManager->uploadHeaps[resourceManager->currentRingIndex].arena = createMemoryArena(mappedHeap, RESOURCE_MANAGER_UPLOAD_HEAP_SIZE);
}

static UploadHeapAllocation _allocateFromUploadHeap(ResourceManager* resourceManager, umm size, umm alignment = 4)
{

	ASSERT(size <= RESOURCE_MANAGER_UPLOAD_HEAP_SIZE);
	UploadHeapAllocation result = {};
	void* cpuMemory = pushSize(&resourceManager->uploadHeaps[resourceManager->currentRingIndex].arena, size, alignment);
	if (!cpuMemory)
	{
		_advanceRing(resourceManager);
		cpuMemory = pushSize(&resourceManager->uploadHeaps[resourceManager->currentRingIndex].arena, size, alignment);
	}
	ASSERT(cpuMemory);

	resourceManager->commandAllocators[resourceManager->currentRingIndex].requiredFenceValueForReset = resourceManager->commandQueue.lastSignaledFenceValue + 1;

	result.cpuMemory = cpuMemory;
	result.gpuMemoryOffset = getOffset(&resourceManager->uploadHeaps[resourceManager->currentRingIndex].arena, cpuMemory);
	result.heap = resourceManager->uploadHeaps[resourceManager->currentRingIndex].d12Resource;

	return result;
}

static ID3D12GraphicsCommandList* _startCommandList(ResourceManager* resourceManager)
{
	resourceManager->commandList.d12CommandList->Reset(resourceManager->commandAllocators[resourceManager->currentRingIndex].d12CommandAllocator, 0);
	resourceManager->commandAllocators[resourceManager->currentRingIndex].requiredFenceValueForReset = resourceManager->commandQueue.lastSignaledFenceValue + 1;
	return resourceManager->commandList.d12CommandList;
}

static u64 _submitCommandListAndSignal(ResourceManager* resourceManager)
{
	ASSERT(resourceManager->commandList.d12CommandList->Close() == S_OK);
	ID3D12CommandList* commandLists[] = { resourceManager->commandList.d12CommandList };
	resourceManager->commandQueue.d12commandQueue->ExecuteCommandLists(ARRAY_SIZE(commandLists), commandLists);
	incrementAndSignal(resourceManager->commandQueue.d12commandQueue, resourceManager->commandQueue.d12fence, &resourceManager->commandQueue.lastSignaledFenceValue);
	return resourceManager->commandQueue.lastSignaledFenceValue;
}
static ResourceManager createResourceManager(ID3D12Device2* device)
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
	ASSERT(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&result.commandQueue.d12commandQueue)) == S_OK);

	//createFence
	ASSERT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result.commandQueue.d12fence)) == S_OK);
	

	return result;
}

static void uploadToBuffer(ResourceManager* resourceManager, TrackedResource* buffer, u64 offset, u64 size, void* data)
{
	UploadHeapAllocation alloc = _allocateFromUploadHeap(resourceManager, size);
	memcpy(alloc.cpuMemory, data, size);

	u64 fenceValue = resourceManager->commandQueue.lastSignaledFenceValue + 1;
	ID3D12GraphicsCommandList* commandList = _startCommandList(resourceManager);
	markModify(buffer, resourceManager->commandQueue.d12commandQueue, commandList, resourceManager->commandQueue.d12fence,
		fenceValue, D3D12_RESOURCE_STATE_COPY_DEST);
	commandList->CopyBufferRegion(buffer->d12Resource, offset, alloc.heap, alloc.gpuMemoryOffset, size);
	ASSERT(fenceValue == _submitCommandListAndSignal(resourceManager));
}

struct Image2D
{
	u8* memory;
	u32 width;
	u32 height;
	u32 pitch;
	u32 pixelSize;
};

struct Image2DLod
{
	Image2D lod[16];
	u32 lodCount;
};

struct Image2DLodRegion
{
	u32 minX;
	u32 minY;
	u32 width;
	u32 height;
	u32 lod;
};

static void uploadToTextureLod(ResourceManager* resourceManager, TrackedResource* texture, Image2DLod* imageLod, DXGI_FORMAT format, Image2DLodRegion* region = 0)
{
	u64 fenceValue = resourceManager->commandQueue.lastSignaledFenceValue + 1;
	ID3D12GraphicsCommandList* commandList = _startCommandList(resourceManager);
	markModify(texture, resourceManager->commandQueue.d12commandQueue, commandList, resourceManager->commandQueue.d12fence, fenceValue,
		D3D12_RESOURCE_STATE_COPY_DEST);

	if (region)
	{
		ASSERT(region->lod < imageLod->lodCount);
		Image2D* image = imageLod->lod + region->lod;
		ASSERT(region->minX + region->width <= image->width);
		ASSERT(region->minY + region->height <= image->height);

		u32 uploadHeapPitch = ALIGN_NUM(region->width*image->pixelSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		u64 size = region->height * uploadHeapPitch;
		UploadHeapAllocation alloc = _allocateFromUploadHeap(resourceManager, size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		
		u8* row = image->memory + region->minX*image->pixelSize + region->minY*image->pitch;
		u8* uploadHeapRow = (u8*)alloc.cpuMemory;
		for (u32 y = 0; y < region->height; ++y)
		{
			memcpy(uploadHeapRow, row, region->width*image->pixelSize);
			row += image->pitch;
			uploadHeapRow += uploadHeapPitch;
		}

		D3D12_TEXTURE_COPY_LOCATION src = {};
		src.PlacedFootprint.Offset = alloc.gpuMemoryOffset;
		src.PlacedFootprint.Footprint.Format = format;
		src.PlacedFootprint.Footprint.Height = region->height;
		src.PlacedFootprint.Footprint.Depth = 1;
		src.PlacedFootprint.Footprint.Width = region->width;
		src.PlacedFootprint.Footprint.RowPitch = uploadHeapPitch;

		src.pResource = alloc.heap;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

		D3D12_TEXTURE_COPY_LOCATION dst = {};
		dst.SubresourceIndex = region->lod;
		dst.pResource = texture->d12Resource;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

		commandList->CopyTextureRegion(&dst, region->minX, region->minY, 0, &src, 0);
	}
	else //upload full image
	{

		for (u32 lod = 0; lod < imageLod->lodCount; ++lod)
		{
			Image2D* image = imageLod->lod + lod;
			u64 size = image->height * image->pitch;
			UploadHeapAllocation alloc = _allocateFromUploadHeap(resourceManager, size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
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
	}
	ASSERT(fenceValue == _submitCommandListAndSignal(resourceManager));
}

static void uploadToTexture(ResourceManager* resourceManager, TrackedResource* texture, Image2D* image, DXGI_FORMAT format)
{
	u64 size = image->height * image->pitch;
	UploadHeapAllocation alloc = _allocateFromUploadHeap(resourceManager, size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	memcpy(alloc.cpuMemory, image->memory, size);

	u64 fenceValue = resourceManager->commandQueue.lastSignaledFenceValue + 1;
	ID3D12GraphicsCommandList* commandList = _startCommandList(resourceManager);
	markModify(texture, resourceManager->commandQueue.d12commandQueue, commandList, resourceManager->commandQueue.d12fence, fenceValue,
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
	ASSERT(fenceValue == _submitCommandListAndSignal(resourceManager));
}

static ConstantBuffer createConstantBuffer(ID3D12Device2* device, u32 size, D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_COPY_DEST)
{
	ConstantBuffer result = {};

	ID3D12Resource* d12Cb = 0;
	ASSERT(device->CreateCommittedResource(
		&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&createResourceDescBuffer(size),
		initState,
		0,
		IID_PPV_ARGS(&d12Cb)
	) == S_OK);

	result.resource.stateAfterModification = initState;
	result.resource.d12Resource = d12Cb;
	result.gpuVirtualAddress = d12Cb->GetGPUVirtualAddress();

	return result;
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

	UploadHeapAllocation alloc = _allocateFromUploadHeap(resourceManager, size);
	memcpy(alloc.cpuMemory, data, size);

	ID3D12GraphicsCommandList* commandList = _startCommandList(resourceManager);
	commandList->CopyBufferRegion(d12Vb, 0, alloc.heap, alloc.gpuMemoryOffset, size);
	u64 fenceValue = _submitCommandListAndSignal(resourceManager);
	result.resource.d12Resource = d12Vb;
	result.resource.modifyFenceSlot.d12Fence = resourceManager->commandQueue.d12fence;
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

	UploadHeapAllocation alloc = _allocateFromUploadHeap(resourceManager, size);
	memcpy(alloc.cpuMemory, data, size);

	ID3D12GraphicsCommandList* commandList = _startCommandList(resourceManager);
	commandList->CopyBufferRegion(d12Ib, 0, alloc.heap, alloc.gpuMemoryOffset, size);
	u64 fenceValue = _submitCommandListAndSignal(resourceManager);

	result.resource.d12Resource = d12Ib;
	result.resource.modifyFenceSlot.d12Fence = resourceManager->commandQueue.d12fence;
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
	updateButton(&input->Q);
	updateButton(&input->space);
	updateButton(&input->left);
	updateButton(&input->right);
	updateButton(&input->up);
	updateButton(&input->down);
	updateButton(&input->tab);
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

static void toggleFillMode(GraphicsPipeline* pipeline)
{
	D3D12_FILL_MODE* fillMode = &pipeline->desc.RasterizerState.FillMode;
	if (*fillMode == D3D12_FILL_MODE_SOLID)
	{
		*fillMode = D3D12_FILL_MODE_WIREFRAME;
	}
	else
	{
		*fillMode = D3D12_FILL_MODE_SOLID;
	}
}

static void Win32ToggleFullScreen(HWND window, WINDOWPLACEMENT* windowPlacement)
{
	//ChangeDisplaySettings is also used before this to modify the display settings (e.g. refresh rate)
	DWORD style = GetWindowLongA(window, GWL_STYLE);
	if (style & WS_OVERLAPPEDWINDOW)
	{
		MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
		if (GetWindowPlacement(window, windowPlacement) &&
			GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitorInfo))
		{
			SetWindowLongA(window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
			SetWindowPos(window, HWND_TOP,
				monitorInfo.rcMonitor.left,
				monitorInfo.rcMonitor.top,
				monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
				monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
				SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
	}
	else
	{
		SetWindowLongA(window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
		SetWindowPlacement(window, windowPlacement);
		SetWindowPos(window, NULL, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
	}
}

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

static b32 shouldRebuildGraphicspipeline(Input* input, GraphicsPipeline* graphicsPipeline)
{
	b32 result = false;

	if (wasPressed(&input->Q))
	{
		toggleFillMode(graphicsPipeline);
		result = true;
	}

	for (u32 shaderIndex = 0; shaderIndex < SHADER_COUNT && !result; ++shaderIndex)
	{
		FileInfo* shader = graphicsPipeline->shaders + shaderIndex;
		if (shader->name && Win32FileWasUpdated(shader->name, shader->lastUpdateTime))
		{
			result = true;
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
	v4 shapeOp;
	v2 uv;
};

#pragma warning(push)
#pragma warning(disable : 4324)

struct LightBuffer
{
	v3 alignas(16) pos;
	v3 alignas(16) intensity;
};

struct alignas(256) SceneBuffer
{
	m4 projview;
	v3 viewPos;
	u32 lightCount;
	LightBuffer lights[8];
};
struct alignas(256) ModelBuffer
{
	m4 model;
	v4 color;
	f32 scale;
	f32 roughness;
	f32 metalness;
	f32 vertexDisplacement;
	f32 fractalZoomScale;
	s32 fractalIndex;
	f32 heightMapFractalZoomScale;
	s32 heightMapFractalIndex;
	f32 emissionScale;
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
			case 'Q': { input->Q.isDown = isDown; break; }
			case VK_SPACE: { input->space.isDown = isDown; break; }
			case VK_LEFT: { input->left.isDown = isDown; break; }
			case VK_RIGHT: { input->right.isDown = isDown; break; }
			case VK_UP: { input->up.isDown = isDown; break; }
			case VK_DOWN: { input->down.isDown = isDown; break; }
			case VK_TAB: { input->tab.isDown = isDown; break; }
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

struct HeightMap
{
	Image2DLod height;
	Image2DLod normal;
};

struct GPUMesh
{
	VertexBuffer vertexBuffer;
	IndexBuffer indexBuffer;
	u32 indexCount;
};

struct GPUHeightMap
{
	TrackedResource height;
	TrackedResource normal;
};

struct GPUFractal
{
	TrackedResource storageImages[2];
	u32 imageIndex;
	u32 uploadWorkIndex;
};

struct GPUHeightMapFractal
{
	GPUHeightMap storedHeightMaps[2];
	u32 imageIndex;
	u32 uploadWorkIndex;
};

inline TrackedResource* getImage(GPUFractal* fractal)
{
	return fractal->storageImages + fractal->imageIndex;
}

inline GPUHeightMap* getHeightMap(GPUHeightMapFractal* fractal)
{
	return fractal->storedHeightMaps + fractal->imageIndex;
}
struct GPUDescriptorBinding
{
	u32 descriptorTableIndexInHeap;
	GPUHeightMap* heightMap;
	GPUFractal* fractalMap;
	GPUHeightMapFractal* heightMapFractal;
};

static GPUMesh createGPUMesh(ResourceManager* resourceManager, Mesh* mesh)
{
	GPUMesh result = {};

	result.vertexBuffer = createVertexBuffer(resourceManager, mesh->vertexCount * sizeof(Vertex), sizeof(Vertex), mesh->vertices);
	result.indexBuffer = createIndexBuffer(resourceManager, mesh->indexCount * sizeof(u32), true, mesh->indices);
	result.indexCount = mesh->indexCount;

	return result;
}

static GPUHeightMap createGPUHeightMap(ResourceManager* resourceManager, HeightMap* heightMap)
{
	GPUHeightMap result = {};

	//normalMap
	ID3D12Resource* normalMapResource = 0;
	D3D12_RESOURCE_DESC normalTexDesc = createResourceDescTex2D(DXGI_FORMAT_R8G8B8A8_UNORM, heightMap->normal.lod[0].width, heightMap->normal.lod[0].height, (u16)heightMap->normal.lodCount);
	ASSERT(resourceManager->device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
		&normalTexDesc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&normalMapResource)) == S_OK);


	result.normal.d12Resource = normalMapResource;
	result.normal.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	uploadToTextureLod(resourceManager, &result.normal, &heightMap->normal, DXGI_FORMAT_R8G8B8A8_UNORM);

	//heightMap
	ID3D12Resource* heightMapResource = 0;
	D3D12_RESOURCE_DESC heightTexDesc = createResourceDescTex2D(DXGI_FORMAT_R32_FLOAT, heightMap->height.lod[0].width, heightMap->height.lod[0].height, (u16)heightMap->height.lodCount);
	ASSERT(resourceManager->device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
		&heightTexDesc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&heightMapResource)) == S_OK);

	result.height.d12Resource = heightMapResource;
	result.height.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	uploadToTextureLod(resourceManager, &result.height, &heightMap->height, DXGI_FORMAT_R32_FLOAT);

	return result;
}

inline void bindBuffers(ID3D12GraphicsCommandList* commandList, ID3D12CommandQueue* commandQueue, ID3D12Fence* fence, u64 requiredFenceValue, GPUMesh* mesh)
{
	markUse(&mesh->vertexBuffer.resource, commandQueue, fence, requiredFenceValue);
	markUse(&mesh->indexBuffer.resource, commandQueue, fence, requiredFenceValue);
	commandList->IASetIndexBuffer(&mesh->indexBuffer.d12View);
	commandList->IASetVertexBuffers(0, 1, &mesh->vertexBuffer.d12View);
}

inline void drawIndexed(ID3D12GraphicsCommandList* commandList, GPUMesh* mesh)
{
	commandList->DrawIndexedInstanced(mesh->indexCount, 1, 0, 0, 0);
}


#define fetchSample(image, u, v, type) (*(type*)((image)->memory + (image)->pitch*(v) + sizeof(type)*(u)))

static Image2D _pushImage2D(MemoryArena* arena, u32 width, u32 height, u32 pixelSize, u32 pitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)
{
	Image2D result = {};

	u32 pitch = (u32)ALIGN_NUM(width * pixelSize, pitchAlign);
	u8* memory = (u8*)pushSize(arena, pitch * height, pitchAlign);
	ASSERT(memory);

	result.height = height;
	result.width = width;
	result.pitch = pitch;
	result.pixelSize = pixelSize;
	result.memory = memory;

	return result;
}

#define pushImage2D(arena, width, height, type, ...) _pushImage2D(arena, width, height, sizeof(type), ##__VA_ARGS__)

static Image2DLod _pushImage2DLod(MemoryArena* arena, u32 width, u32 height, u32 pixelSize, u32 maxLodCount = 0xffffffff, u32 pitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)
{
	Image2DLod result = {};
	while ((width > 0 || height > 0) && result.lodCount < maxLodCount)
	{
		width = MAX(1, width);
		height = MAX(1, height);

		ASSERT(result.lodCount < ARRAY_SIZE(result.lod));
		result.lod[result.lodCount++] = _pushImage2D(arena, width, height, pixelSize, pitchAlign);
		width >>= 1;
		height >>= 1;
	}

	return result;
}

#define pushImage2DLod(arena, width, height, type, ...) _pushImage2DLod(arena, width, height, sizeof(type), ##__VA_ARGS__)

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

static v2 addPerlinNoise(Image2D* image, Image2D* grad, u32 gradAlignX, u32 gradAlignY, u32 tileSize, f32 heightScale)
{
	u8* row = image->memory;
	v2 range = { 1e10f, -1e10f };

	ASSERT(IS_POW2(grad->width) && IS_POW2(grad->height));

	f32 tileSizeScale = 1.f / (f32)tileSize;

	s32 gradMaskU = grad->width - 1;
	s32 gradMaskV = grad->height - 1;

	for (u32 y = 0; y < image->height; ++y)
	{
		f32 v =  ((f32)y - (f32)gradAlignY + 0.5f) * tileSizeScale;
		s32 v0 = (s32)floorf(v);
		s32 v1 = v0 + 1;
		f32 dv = v - (f32)v0;
		v0 = v0 & gradMaskV;
		v1 = v1 & gradMaskV;

		f32* pixel = (f32*)row;
		for (u32 x = 0; x < image->width; ++x)
		{
			f32 u = ((f32)x - (f32)gradAlignX + 0.5f) * tileSizeScale;
			s32 u0 = (s32)(floorf(u));
			s32 u1 = u0 + 1;
			f32 du = u - (f32)u0;
			u0 = u0 & gradMaskU;
			u1 = u1 & gradMaskU;
			
			v2 t00 = fetchSample(grad, u0, v0, v2);
			v2 t10 = fetchSample(grad, u1, v0, v2);
			v2 t01 = fetchSample(grad, u0, v1, v2);
			v2 t11 = fetchSample(grad, u1, v1, v2);

			f32 a = smoothBlend2(dot(t00, { du, dv       }), dot(t10, { du - 1.f, dv       }), du);
			f32 b = smoothBlend2(dot(t01, { du, dv - 1.f }), dot(t11, { du - 1.f, dv - 1.f }), du);
			f32 c = smoothBlend2(a, b, dv);

			c = heightScale * c + *pixel;
			range.x = MIN(c, range.x);
			range.y = MAX(c, range.y);

			*pixel++ = c;
		}
		row += image->pitch;
	}

	return range;
}

static void clearImage2D(Image2D* image)
{
	memset(image->memory, 0, image->height*image->pitch);
}

inline v4 unpackColor(u32 color)
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

inline u32 packColor(v4 color)
{
	color = saturate(color);
	u32 r = (u32)(color.r * 255.f);
	u32 g = (u32)(color.g * 255.f);
	u32 b = (u32)(color.b * 255.f);
	u32 a = (u32)(color.a * 255.f);

	return (a << 24) | (b << 16) | (g << 8) | (r << 0);
}

inline u32 packNormal(v3 normal)
{
	normal = normalize(normal);
	normal = 0.5f *normal + V3(0.5f);
	return packColor(V4(normal, 1.f));
}

inline v3 unpackNormal(u32 normal)
{
	v3 result = unpackColor(normal).xyz;
	result = 2.f*result - V3(1.f);
	result = normalize(result);
	return result;
}

static void fillNormalMapForHeightMap(Image2D* heightMap, Image2D* normalMap)
{
	ASSERT(heightMap->width == normalMap->width);
	ASSERT(heightMap->height == normalMap->height);

	f32 pixelSizeX = 1.f / (f32)heightMap->width;
	f32 pixelSizeY = 1.f / (f32)heightMap->height;

	u8* rowNormal = normalMap->memory;
	for (u32 y = 0; y < heightMap->height; ++y)
	{
		u32 y0 = (y + heightMap->height- 1) % heightMap->height;
		u32 y1 = (y + 1) % heightMap->height;

		u32* normal = (u32*)rowNormal;
		for (u32 x = 0; x < heightMap->width; ++x)
		{
			u32 x0 = (x + heightMap->width- 1) % heightMap->width;
			u32 x1 = (x + 1) % heightMap->width;

			f32 dhdx = (fetchSample(heightMap, x1, y, f32) - fetchSample(heightMap, x0, y, f32)) / (2.f * pixelSizeX);
			f32 dhdy = (fetchSample(heightMap, x, y1, f32) - fetchSample(heightMap, x, y0, f32)) / (2.f*pixelSizeY);

			*normal++ = packNormal({ -dhdx, -dhdy, 1.f });
			//v3 n = normalize(v3{ -dhdx, -dhdy, 1.f }); //coordinate order: tangent, bitangent, normal
			//n = 0.5f*n + V3(0.5f);
			//n *= 255.f;
			//u32 nX = (u32)n.x;
			//u32 nY = (u32)n.y;
			//u32 nZ = (u32)n.z;
			//
			//*normal++ = (255 << 24) | (nZ << 16) | (nY << 8) | (nX << 0);
		}
		rowNormal += normalMap->pitch;
	}
}

enum IMAGE_EDGE
{
	IMAGE_EDGE_U0,
	IMAGE_EDGE_U1,
	IMAGE_EDGE_V0,
	IMAGE_EDGE_V1,
};

static Mesh createSphereMesh(MemoryArena* arena, u32 quadCountU, u32 quadCountV)
{
	Mesh result = {};
	ASSERT(quadCountV > 1 && quadCountU > 2);

	u32 vertexCount = (quadCountU + 1) * (quadCountV + 1);

	u32 triangleCount = 2*quadCountV * quadCountU;
	u32 indexCount = 3 * triangleCount;

	Vertex* vertices = pushArray(arena, vertexCount, Vertex);
	u32* indices = pushArray(arena, indexCount, u32);

	Vertex* vertexAt = vertices;
	for (u32 vIndex = 0; vIndex < quadCountV + 1; ++vIndex)
	{
		f32 v = (f32)vIndex / (f32)quadCountV;
		for (u32 uIndex = 0; uIndex < quadCountU + 1; ++uIndex)
		{
			f32 u =(f32)uIndex / (f32)quadCountU;

			Vertex* newVertex = vertexAt++;

			newVertex->position = spherePos(u, v);
			newVertex->normal = newVertex->position;
			newVertex->uv = polarCoord(u,v);
			m3x2 Dpos = DspherePos(u, v) * DpolarCoordInv(u, v);
			newVertex->tangent = Dpos.c[0];
			newVertex->bitangent = Dpos.c[1];
			newVertex->shapeOp = sphereShapeOp(u, v);
		}
	}
	ASSERT(vertices + vertexCount == vertexAt);

	u32* indexAt = indices;
	for (u32 vIndex = 0; vIndex < quadCountV; ++vIndex)
	{
		for (u32 uIndex = 0; uIndex < quadCountU; ++uIndex)
		{
			u32 baseVertexIndex = vIndex * (quadCountU + 1) + uIndex;

			*indexAt++ = baseVertexIndex;
			*indexAt++ = baseVertexIndex + (quadCountU + 1) + 1;
			*indexAt++ = baseVertexIndex + (quadCountU + 1);

			*indexAt++ = baseVertexIndex + (quadCountU + 1) + 1;
			*indexAt++ = baseVertexIndex;
			*indexAt++ = baseVertexIndex + 1;
		}
	}
	ASSERT(indices + indexCount == indexAt);

	result.indexCount = indexCount;
	result.indices = indices;
	result.vertexCount = vertexCount;
	result.vertices = vertices;

	return result;
}

static void reverseTangentSpaceOrientation(Mesh* mesh)
{
	for (u32 vertexIndex = 0; vertexIndex < mesh->vertexCount; ++vertexIndex)
	{
		Vertex* vertex = mesh->vertices + vertexIndex;
		vertex->bitangent *= -1.f;
		vertex->normal *= -1.f;
	}
}

static void reverseTriangleOrientation(Mesh* mesh)
{
	for (u32 triangleIndex = 0; triangleIndex < mesh->indexCount; triangleIndex += 3)
	{
		u32 temp = mesh->indices[triangleIndex];
		mesh->indices[triangleIndex] = mesh->indices[triangleIndex + 1];
		mesh->indices[triangleIndex + 1] = temp;
	}
};

static void reverseOrientation(Mesh* mesh)
{
	reverseTangentSpaceOrientation(mesh);
	reverseTriangleOrientation(mesh);
}

static Mesh createTorusMesh(MemoryArena* arena, u32 tileCountU, u32 tileCountV, f32 holeRadius)
{
	Mesh result = {};

	u32 vertexCount = (tileCountU + 1) * (tileCountV + 1);
	u32 triangleCount = 2 * tileCountU * tileCountV;
	u32 indexCount = 3 * triangleCount;

	Vertex* vertices = pushArray(arena, vertexCount, Vertex);
	u32* indices = pushArray(arena, indexCount, u32);

	Vertex* vertexAt = vertices;
	for (u32 vIndex = 0; vIndex < tileCountV + 1; ++vIndex)
	{
		f32 v = (f32)vIndex / (f32)tileCountV;
		for (u32 uIndex = 0; uIndex < tileCountU + 1; ++uIndex)
		{
			f32 u = (f32)uIndex / (f32)tileCountU;

			Vertex* newVertex = vertexAt++;

			newVertex->position = torusPos(u, v, holeRadius);
			newVertex->tangent = torusdPosdu(u, v, holeRadius);
			newVertex->bitangent = torusdPosdv(u, v, holeRadius);
			newVertex->shapeOp = torusShapeOp(u, v, holeRadius);
			newVertex->uv = { u,v };
			newVertex->normal = normalize(cross(newVertex->tangent, newVertex->bitangent));
		}
	}
	ASSERT(vertices + vertexCount == vertexAt);

	u32* indexAt = indices;
	for (u32 vIndex = 0; vIndex < tileCountV; ++vIndex)
	{
		for (u32 uIndex = 0; uIndex < tileCountU; ++uIndex)
		{
			u32 baseVertexIndex = vIndex * (tileCountU + 1) + uIndex;
			
			*indexAt++ = baseVertexIndex;
			*indexAt++ = baseVertexIndex + (tileCountU + 1) + 1;
			*indexAt++ = baseVertexIndex + (tileCountU + 1);

			*indexAt++ = baseVertexIndex + (tileCountU + 1) + 1;
			*indexAt++ = baseVertexIndex;
			*indexAt++ = baseVertexIndex + 1;
		}
	}
	ASSERT(indices + indexCount == indexAt);

	result.indexCount = indexCount;
	result.indices = indices;
	result.vertexCount = vertexCount;
	result.vertices = vertices;

	return result;
}

static Mesh createCubeMesh(MemoryArena* arena)
{
	Mesh result = {};

	u32 vertexCount = 36;
	Vertex* vertices = pushArray(arena, vertexCount, Vertex);
	vertices[0] = { {-1.f, -1.f, -1.f },{-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 0.f} };
	vertices[1] = { {-1.f, -1.f, 1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 1.f} };
	vertices[2] = { {-1.f, 1.f, -1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 0.f} };
				   
	vertices[3] = { {-1.f, 1.f, 1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 1.f} };
	vertices[4] = { {-1.f, 1.f, -1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 0.f} };
	vertices[5] = { {-1.f, -1.f, 1.f }, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 1.f} };
				   
	vertices[6] = { {1.f, -1.f, -1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 0.f} };
	vertices[7] = { {1.f, 1.f, -1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 0.f} };
	vertices[8] = { {1.f, -1.f, 1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 1.f} };
				   
	vertices[9] = { {1.f, 1.f, 1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 1.f} };
	vertices[10] = { {1.f, -1.f, 1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 1.f} };
	vertices[11] = { {1.f, 1.f, -1.f }, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 0.f} };
		
	vertices[12] = { {-1.f, -1.f, -1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 0.f} };
	vertices[13] = { {1.f, -1.f, -1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 0.f} };
	vertices[14] = { {-1.f, -1.f, 1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 1.f} };
		
	vertices[15] = { {1.f, -1.f, 1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 1.f} };
	vertices[16] = { {-1.f, -1.f, 1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 1.f} };
	vertices[17] = { {1.f, -1.f, -1.f }, {0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 0.f} };
		
	vertices[18] = { {-1.f, 1.f, -1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 0.f} };
	vertices[19] = { {-1.f, 1.f, 1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 1.f} };
	vertices[20] = { {1.f, 1.f, -1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 0.f} };
		
	vertices[21] = { {1.f, 1.f, 1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 1.f} };
	vertices[22] = { {1.f, 1.f, -1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {1.f, 0.f} };
	vertices[23] = { {-1.f, 1.f, 1.f }, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {}, {0.f, 1.f} };
		
	vertices[24] = { {-1.f, -1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {0.f, 0.f} };
	vertices[25] = { {-1.f, 1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {0.f, 1.f} };
	vertices[26] = { {1.f, -1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {1.f, 0.f} };
		
	vertices[27] = { {1.f, 1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {1.f, 1.f} };
	vertices[28] = { {1.f, -1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {1.f, 0.f} };
	vertices[29] = { {-1.f, 1.f, -1.f }, {0.f, 0.f, -1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {0.f, 1.f} };
		
	vertices[30] = { {-1.f, -1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {0.f, 0.f} };
	vertices[31] = { {1.f, -1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {1.f, 0.f} };
	vertices[32] = { {-1.f, 1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {0.f, 1.f} };
		
	vertices[33] = { {1.f, 1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {1.f, 1.f} };
	vertices[34] = { {-1.f, 1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {0.f, 1.f} };
	vertices[35] = { {1.f, -1.f, 1.f }, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {}, {1.f, 0.f} };

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

static Mesh createPlaneMesh(MemoryArena* arena, v2 tileSize, u32 tileCountX, u32 tileCountZ)
{
	ASSERT(tileCountX > 0);
	ASSERT(tileCountZ > 0);

	Mesh result = {};

	u32 vertexCountX = tileCountX + 1;
	u32 vertexCountZ = tileCountZ + 1;
	u32 vertexCount = vertexCountX * vertexCountZ;
	u32 triangleCount = tileCountX * tileCountZ * 2;
	u32 indexCount = 3 * triangleCount;

	Vertex* vertices = pushArray(arena, vertexCount, Vertex);
	u32* indices = pushArray(arena, indexCount, u32);

	Vertex* vertexAt = vertices;
	for (u32 vertexIndexZ = 0; vertexIndexZ < vertexCountZ; ++vertexIndexZ)
	{
		for (u32 vertexIndexX = 0; vertexIndexX < vertexCountX; ++vertexIndexX)
		{
			Vertex* newVertex = vertexAt++;
			newVertex->position = { (f32)vertexIndexX * tileSize.x, 0.f, (f32)vertexIndexZ * tileSize.y };
			newVertex->normal = { 0.f, 1.f, 0.f };
			newVertex->tangent = { 1.f, 0.f, 0.f };
			newVertex->bitangent = { 0.f, 0.f, 1.f };
			newVertex->uv = { (f32)vertexIndexX / (f32)tileCountX, (f32)vertexIndexZ / (f32)tileCountZ };
		}
	}
	ASSERT(vertices + vertexCount == vertexAt);

	u32* indexAt = indices;
	for (u32 tileIndexZ = 0; tileIndexZ < tileCountZ; ++tileIndexZ)
	{
		for (u32 tileIndexX = 0; tileIndexX < tileCountX; ++tileIndexX)
		{
			*indexAt++ = tileIndexX + tileIndexZ * vertexCountX;
			*indexAt++ = tileIndexX + (tileIndexZ + 1)*vertexCountX;
			*indexAt++ = tileIndexX + 1 + tileIndexZ * vertexCountX;

			*indexAt++ = tileIndexX + (tileIndexZ + 1)*vertexCountX;
			*indexAt++ = tileIndexX + 1 + (tileIndexZ + 1)*vertexCountX;
			*indexAt++ = tileIndexX + 1 + tileIndexZ * vertexCountX;
		}
	}
	ASSERT(indices + indexCount == indexAt);

	result.indexCount = indexCount;
	result.indices = indices;
	result.vertexCount = vertexCount;
	result.vertices = vertices;

	return result;
}

struct SampleParams1D
{
	s32 u0;
	s32 u1;
	f32 du;
};

inline SampleParams1D getSampleParams(u32 width, f32 u) //u is expected to be in [0,1]
{
	SampleParams1D result = {};

	u *= (f32)width;
	u -= 0.5f;

	s32 u0 = (s32)floorf(u);
	s32 u1 = u0 + 1;
	u1 = CLAMP(0, (s32)width - 1, u1);
	u0 = CLAMP(0, (s32)width - 1, u0);

	f32 du = u - (f32)u0;

	result.u0 = u0;
	result.u1 = u1;
	result.du = du;

	return result;
}

struct SampleParams1DAVX
{
	__m256i u0;
	__m256i u1;
	__m256 du;
};

inline SampleParams1DAVX getSampleParams(__m256i width, __m256 u) //u is expected to be in [0,1]
{
	SampleParams1DAVX result = {};

	u = u * _mm256_cvtepi32_ps(width);
	u = u - _mm256_set1_ps(0.5f);

	__m256i u0 = _mm256_cvtps_epi32(_mm256_round_ps(u, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
	__m256i u1 = _mm256_add_epi32(u0, _mm256_set1_epi32(1));
	
	u0 = _mm256_max_epi32(_mm256_min_epi32(_mm256_sub_epi32(width, _mm256_set1_epi32(1)), u0), _mm256_set1_epi32(0));
	u1 = _mm256_max_epi32(_mm256_min_epi32(_mm256_sub_epi32(width, _mm256_set1_epi32(1)), u1), _mm256_set1_epi32(0));

	__m256 du = _mm256_sub_ps(u, _mm256_cvtepi32_ps(u0));

	result.u0 = u0;
	result.u1 = u1;
	result.du = du;

	return result;
}

union SampleParams2D
{
	struct
	{
		s32 u0;
		s32 u1;
		f32 du;

		s32 v0;
		s32 v1;
		f32 dv;
	};
	struct
	{
		SampleParams1D paramsU;
		SampleParams1D paramsV;
	};

};

union SampleParams2DAVX
{
	struct
	{
		__m256i u0;
		__m256i u1;
		__m256 du;

		__m256i v0;
		__m256i v1;
		__m256 dv;
	};
	struct
	{
		SampleParams1DAVX paramsU;
		SampleParams1DAVX paramsV;
	};

};

inline SampleParams2DAVX getSampleParams(__m256i width, __m256i height, __m256 u, __m256 v) //u and v are expected to be in [0,1]
{
	SampleParams2DAVX result = {};

	result.paramsU = getSampleParams(width, u);
	result.paramsV = getSampleParams(height, v);

	return result;
}

static SampleParams2D getSampleParams(u32 width, u32 height, f32 u, f32 v)
{
	SampleParams2D result = {};
	result.paramsU = getSampleParams(width, u);
	result.paramsV = getSampleParams(height, v);
	return result;
}

static void generateMipLevels4U8(Image2DLod* image)
{
	for (u32 lod = 1; lod < image->lodCount; ++lod)
	{
		Image2D* newImage = image->lod + lod;
		Image2D* prevImage = image->lod + (lod - 1);

		u8* row = newImage->memory;
		for (u32 y = 0; y < newImage->height; ++y)
		{
			f32 v = ((f32)y + 0.5f) / (f32)newImage->height;
			SampleParams2D s = {};
			s.paramsV = getSampleParams(prevImage->height, v);

			u32* pixel = (u32*)row;
			for (u32 x = 0; x < newImage->width; ++x)
			{
				f32 u = ((f32)x + 0.5f) / (f32)newImage->width;
				s.paramsU = getSampleParams(prevImage->width, u);

				v4 c00 = unpackColor(fetchSample(prevImage, s.u0, s.v0, u32));
				v4 c10 = unpackColor(fetchSample(prevImage, s.u1, s.v0, u32));
				v4 c01 = unpackColor(fetchSample(prevImage, s.u0, s.v1, u32));
				v4 c11 = unpackColor(fetchSample(prevImage, s.u1, s.v1, u32));

				v4 a = lerp(c00, c10, s.du);
				v4 b = lerp(c01, c11, s.du);
				v4 c = lerp(a, b, s.dv);

				*pixel++ = packColor(c);

			}
			row += newImage->pitch;
		}
	}
}

static void generateMipLevels1F32(Image2DLod* image)
{
	for(u32 lod = 1; lod < image->lodCount; ++lod)
	{
		Image2D* newImage = image->lod + lod;
		Image2D* prevImage = image->lod + (lod - 1);

		u8* row = newImage->memory;
		for (u32 y = 0; y < newImage->height; ++y)
		{
			f32 v = ((f32)y + 0.5f) / (f32)newImage->height;
			SampleParams2D s = {};
			s.paramsV = getSampleParams(prevImage->height, v);

			f32* pixel = (f32*)row;
			for (u32 x = 0; x < newImage->width; ++x)
			{
				f32 u = ((f32)x + 0.5f) / (f32)newImage->width;
				s.paramsU = getSampleParams(prevImage->width, u);

				f32 c00 = fetchSample(prevImage, s.u0, s.v0, f32);
				f32 c10 = fetchSample(prevImage, s.u1, s.v0, f32);
				f32 c01 = fetchSample(prevImage, s.u0, s.v1, f32);
				f32 c11 = fetchSample(prevImage, s.u1, s.v1, f32);

				f32 a = lerp(c00, c10, s.du);
				f32 b = lerp(c01, c11, s.du);
				f32 c = lerp(a, b, s.dv);

				*pixel++ = c;
			}
			row += newImage->pitch;
		}
	}
}

static void generateMipLevels1F32AVX(Image2DLod* image)
{
	__m256 _0_to_7 = _mm256_setr_ps(0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f);

	for (u32 lod = 1; lod < image->lodCount; ++lod)
	{
		Image2D* newImage = image->lod + lod;
		Image2D* prevImage = image->lod + (lod - 1);

		ASSERT((newImage->pitch & 31) == 0); //pitch is aligned to avx registers

		__m256 newImageWidth = _mm256_set1_ps((f32)newImage->width);
		__m256i prevImageWidth = _mm256_set1_epi32(prevImage->width);
		__m256i prevImagePitch = _mm256_set1_epi32(prevImage->pitch);

		u8* row = newImage->memory;
		for (u32 y = 0; y < newImage->height; ++y)
		{
			f32 v = ((f32)y + 0.5f) / (f32)newImage->height;
			SampleParams2DAVX s = {};
			s.paramsV = getSampleParams(_mm256_set1_epi32(prevImage->height), _mm256_set1_ps(v));

			f32* pixel = (f32*)row;
			for (u32 _x = 0; _x < newImage->width; _x+=8)
			{
				__m256 x = _mm256_add_ps(_mm256_set1_ps((f32)_x), _0_to_7);

				__m256 u = (x + _mm256_set1_ps(0.5f)) / newImageWidth;
				s.paramsU = getSampleParams(prevImageWidth, u);

				__m256i i00 = _mm256_add_epi32(_mm256_mullo_epi32(s.u0, _mm256_set1_epi32(sizeof(f32))), _mm256_mullo_epi32(s.v0, prevImagePitch));
				__m256i i10 = _mm256_add_epi32(_mm256_mullo_epi32(s.u1, _mm256_set1_epi32(sizeof(f32))), _mm256_mullo_epi32(s.v0, prevImagePitch));
				__m256i i01 = _mm256_add_epi32(_mm256_mullo_epi32(s.u0, _mm256_set1_epi32(sizeof(f32))), _mm256_mullo_epi32(s.v1, prevImagePitch));
				__m256i i11 = _mm256_add_epi32(_mm256_mullo_epi32(s.u1, _mm256_set1_epi32(sizeof(f32))), _mm256_mullo_epi32(s.v1, prevImagePitch));

				__m256 c00;
				c00.m256_f32[0] = *(f32*)(prevImage->memory + i00.m256i_i32[0]);
				c00.m256_f32[1] = *(f32*)(prevImage->memory + i00.m256i_i32[1]);
				c00.m256_f32[2] = *(f32*)(prevImage->memory + i00.m256i_i32[2]);
				c00.m256_f32[3] = *(f32*)(prevImage->memory + i00.m256i_i32[3]);
				c00.m256_f32[4] = *(f32*)(prevImage->memory + i00.m256i_i32[4]);
				c00.m256_f32[5] = *(f32*)(prevImage->memory + i00.m256i_i32[5]);
				c00.m256_f32[6] = *(f32*)(prevImage->memory + i00.m256i_i32[6]);
				c00.m256_f32[7] = *(f32*)(prevImage->memory + i00.m256i_i32[7]);

				__m256 c10;
				c10.m256_f32[0] = *(f32*)(prevImage->memory + i10.m256i_i32[0]);
				c10.m256_f32[1] = *(f32*)(prevImage->memory + i10.m256i_i32[1]);
				c10.m256_f32[2] = *(f32*)(prevImage->memory + i10.m256i_i32[2]);
				c10.m256_f32[3] = *(f32*)(prevImage->memory + i10.m256i_i32[3]);
				c10.m256_f32[4] = *(f32*)(prevImage->memory + i10.m256i_i32[4]);
				c10.m256_f32[5] = *(f32*)(prevImage->memory + i10.m256i_i32[5]);
				c10.m256_f32[6] = *(f32*)(prevImage->memory + i10.m256i_i32[6]);
				c10.m256_f32[7] = *(f32*)(prevImage->memory + i10.m256i_i32[7]);

				__m256 c01;
				c01.m256_f32[0] = *(f32*)(prevImage->memory + i01.m256i_i32[0]);
				c01.m256_f32[1] = *(f32*)(prevImage->memory + i01.m256i_i32[1]);
				c01.m256_f32[2] = *(f32*)(prevImage->memory + i01.m256i_i32[2]);
				c01.m256_f32[3] = *(f32*)(prevImage->memory + i01.m256i_i32[3]);
				c01.m256_f32[4] = *(f32*)(prevImage->memory + i01.m256i_i32[4]);
				c01.m256_f32[5] = *(f32*)(prevImage->memory + i01.m256i_i32[5]);
				c01.m256_f32[6] = *(f32*)(prevImage->memory + i01.m256i_i32[6]);
				c01.m256_f32[7] = *(f32*)(prevImage->memory + i01.m256i_i32[7]);

				__m256 c11;
				c11.m256_f32[0] = *(f32*)(prevImage->memory + i11.m256i_i32[0]);
				c11.m256_f32[1] = *(f32*)(prevImage->memory + i11.m256i_i32[1]);
				c11.m256_f32[2] = *(f32*)(prevImage->memory + i11.m256i_i32[2]);
				c11.m256_f32[3] = *(f32*)(prevImage->memory + i11.m256i_i32[3]);
				c11.m256_f32[4] = *(f32*)(prevImage->memory + i11.m256i_i32[4]);
				c11.m256_f32[5] = *(f32*)(prevImage->memory + i11.m256i_i32[5]);
				c11.m256_f32[6] = *(f32*)(prevImage->memory + i11.m256i_i32[6]);
				c11.m256_f32[7] = *(f32*)(prevImage->memory + i11.m256i_i32[7]);


				__m256 a = lerp(c00, c10, s.du);
				__m256 b = lerp(c01, c11, s.du);
				__m256 c = lerp(a, b, s.dv);

				_mm256_store_ps(pixel, c);

				pixel += 8;
			}
			row += newImage->pitch;
		}
	}
}


struct FractalGrad
{
	Image2D grad;
	s32 gridAlignX;
	s32 gridAlignY;
};

enum IMAGE_STATE
{
	IMAGE_STATE_OBSOLETE,
	IMAGE_STATE_PRECOMPUTING,
	IMAGE_STATE_COMPUTING,
	IMAGE_STATE_POSTCOMPUTING,
	IMAGE_STATE_READY,

	//for colored fractal
	IMAGE_STATE_COMPUTING_CHANNELS,

	//for height map fractal
	IMAGE_STATE_COMPUTING_HEIGHT_MAP,
	IMAGE_STATE_COMPUTING_NORMAL_MAP,
};

struct Fractal;
struct ComputeFractalWork
{
	//input
	Fractal* fractal;
	ClipRect clipRect;

	//output
	v2 range;
};

struct Fractal
{
	LARGE_INTEGER DEBUGstartComputeTime;

	Image2DLod im;
	f32 zoomFactor;
	f32 zoomSpeed;
	FractalGrad grads[16];
	u32 currentBaseGradIndex;
	u32 maxTileSize;
	u32 layerCount;

	ComputeFractalWork* works;
	u32 workCount;

	u32 volatile partsInFlightCount;
	
	IMAGE_STATE imageState;
	b32 resetHappened;
	b32 shouldRecompute;
};

struct ColoredFractal
{
	union
	{
		Fractal channels[3];
		struct
		{
			Fractal red;
			Fractal green;
			Fractal blue;
		};
	};
	LARGE_INTEGER DEBUGstartComputeTime;

	Image2DLod im;

	f32 zoomFactor;
	ComputeFractalWork* works;
	u32 workCount;

	u32 volatile partsInFlightCount;

	IMAGE_STATE imageState;
	b32 resetHappened;
	b32 shouldRecompute;
};

struct HeightMapFractal
{
	Fractal height;
	Image2DLod normal;

	u32 volatile partsInFlightCount;

	IMAGE_STATE imageState;
	b32 resetHappened;
	b32 shouldRecompute;
};


static void wrapImage1F32(Image2D*image, b32 alongU, u32 blendWidthInPixels)
{
	ASSERT(blendWidthInPixels > 0);
	if (alongU)
	{
		ASSERT(2 * blendWidthInPixels < image->height);
		u8* row1 = image->memory;
		u8* row2 = image->memory + image->pitch * (image->height - 1);
		for (u32 blendRowIndex = 0; blendRowIndex < blendWidthInPixels+1; ++blendRowIndex)
		{
			f32 t = 0.5f - 0.5f*((f32)blendRowIndex / (f32)blendWidthInPixels); // in [0, 0.5]

			f32* pixel1 = (f32*)row1;
			f32* pixel2 = (f32*)row2;
			for (u32 x = 0; x < image->width; ++x)
			{
				f32 p1 = *pixel1;
				f32 p2 = *pixel2;

				*pixel1++ = smoothBlend2(p1, p2, t);
				*pixel2++ = smoothBlend2(p2, p1, t);
			}

			row1 += image->pitch;
			row2 -= image->pitch;
		}
	}
	else
	{
		ASSERT(2 * blendWidthInPixels < image->width);
		u8* row = image->memory;
		for (u32 y = 0; y < image->height; ++y)
		{
			f32* pixel1 = (f32*)row;
			f32* pixel2 = (f32*)row + (image->width - 1);
			for (u32 blendPixelIndex = 0; blendPixelIndex < blendWidthInPixels + 1; ++blendPixelIndex)
			{
				f32 t = 0.5f - 0.5f*((f32)blendPixelIndex / (f32)blendWidthInPixels); // in [0, 0.5]
				
				f32 p1 = *pixel1;
				f32 p2 = *pixel2;

				*pixel1++ = smoothBlend2(p1, p2, t);
				*pixel2-- = smoothBlend2(p2, p1, t);
			}
			row += image->pitch;
		}
	}
}

inline void _blendRow(f32* row, f32* temp, u32 currentWidth, u32 newWidth)
{
	for (u32 x = 0; x < newWidth; ++x)
	{
		f32 u = ((f32)x + 0.5f) / (f32)newWidth;

		SampleParams1D s = getSampleParams(currentWidth, u);
		temp[x] = lerp(row[s.u0], row[s.u1], s.du);
	}

	for (u32 x = 0; x < newWidth; ++x)
	{
		*row++ = *temp++;
	}
}


static void collapsEdge1F32(Image2D* image, u32 stripSize, IMAGE_EDGE edge)
{
	ASSERT(stripSize > 0);
	if (edge == IMAGE_EDGE_V0 || edge == IMAGE_EDGE_V1)
	{
		u8* collapsedRow = image->memory + (edge == IMAGE_EDGE_V0 ? 0 : image->pitch * (image->height - 1));
		s32 rowAdvance = (edge == IMAGE_EDGE_V0) ? (s32)image->pitch : -(s32)image->pitch;

		f32 collapsedEdgeValue = 0.f;
		f32* pixel = (f32*)collapsedRow;
		for (u32 x = 0; x < image->width; ++x)
		{
			collapsedEdgeValue += *pixel++;
		}
		collapsedEdgeValue /= (f32)image->width;

		//now we can use the memory of the collapsedRow as temporary storage for blending

		u8* row = collapsedRow + rowAdvance;
		for (u32 blendRowIndex = 1; blendRowIndex < stripSize+1; ++blendRowIndex)
		{
			u32 downSampledWidth = MAX(1, (u32)((f32)image->width*smoothStep2((f32)blendRowIndex / (f32)stripSize)));
			
			//downsample
			u32 currentWidth = image->width;
			while (currentWidth / 2 > downSampledWidth)
			{
				u32 newWidth = currentWidth / 2;
				_blendRow((f32*)row, (f32*)collapsedRow, currentWidth, newWidth);
				currentWidth = newWidth;
			}
			if (currentWidth > downSampledWidth)
			{
				_blendRow((f32*)row, (f32*)collapsedRow, currentWidth, downSampledWidth);
			}
			//upsample
			_blendRow((f32*)row, (f32*)collapsedRow, downSampledWidth, image->width);

			//blend with prev row
			f32* prevRowPixel = (f32*)(row - rowAdvance);
			f32* rowPixel = (f32*)row;
			for (u32 x = 0; x < image->width; ++x)
			{
				*rowPixel++ = 0.5f * (*rowPixel + *prevRowPixel++);
			}

			row += rowAdvance;
		}

		f32* collapsedPixel = (f32*)collapsedRow;
		for (u32 x = 0; x < image->width; ++x)
		{
			*collapsedPixel++ = collapsedEdgeValue;
		}
	}
	else
	{
		ASSERT(!"Not implemented!");
	}
}

struct Terrain
{
	GPUMesh mesh;
	TrackedResource heightMap;
	TrackedResource normalMap;
	u32 tileCount;
	f32 tileSize;
	f32 maxHeight;
};

#if 0
static Terrain createTerrain(ResourceManager* resourceManager, MemoryArena* arena, u32 tileCount, f32 tileSize, f32 maxHeight, u32 heightPixelPerTile)
{
	Terrain result = {};
	result.tileCount = tileCount;
	result.tileSize = tileSize;
	result.maxHeight = maxHeight;

	TempMemory tempMem = startTempMemory(arena);
	Mesh planeMesh = createPlaneMesh(arena, { result.tileSize, result.tileSize }, result.tileCount, result.tileCount);

	result.mesh = createGPUMesh(resourceManager, &planeMesh);

	endTempMemory(&tempMem);

	tempMem = startTempMemory(arena);
	Image2DLod heightMap = {};
	heightMap.lod[0] = pushImage2D(arena, result.tileCount * heightPixelPerTile, result.tileCount * heightPixelPerTile, f32);
	heightMap.lodCount = 1;
	Image2DLod normalMap = {};
	normalMap.lod[0] = pushImage2D(arena, result.tileCount * heightPixelPerTile, result.tileCount * heightPixelPerTile, u32);
	normalMap.lodCount = 1;
	Image2D grad = pushImage2D(arena, 64, 64, v2);

	fillWithRandomGradients(&grad, 13);
	clearImage2D(&heightMap.lod[0]);

	u32 scale = 1;
	v2 info{};
	u32 gradGridSize = 1024;
	for (u32 iter = 0; iter < 10 && gradGridSize  > 0; ++iter)
	{
		info = addPerlinNoise(&heightMap.lod[0], &grad, { (f32)gradGridSize, 0.f }, 0, 0, result.maxHeight / (f32)scale);
		gradGridSize >>= 1;
		scale <<= 1;
	}

	collapsEdge1F32(&heightMap.lod[0], 2000, IMAGE_EDGE_V0);

	generateMipLevels1F32(&heightMap);
	generateMipLevels4U8(&normalMap);

	for (u32 lod = 0; lod < heightMap.lodCount; ++lod)
	{
		fillNormalMapForHeightMap(&heightMap.lod[lod], &normalMap.lod[lod]);
	}

	//normalMap
	ID3D12Resource* normalMapResource = 0;
	D3D12_RESOURCE_DESC normalTexDesc = createResourceDescTex2D(DXGI_FORMAT_R8G8B8A8_UNORM, normalMap.lod[0].width, normalMap.lod[0].height, (u16)normalMap.lodCount);
	ASSERT(resourceManager->device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
		&normalTexDesc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&normalMapResource)) == S_OK);


	result.normalMap.d12Resource = normalMapResource;
	result.normalMap.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	uploadToTextureLod(resourceManager, &result.normalMap, &normalMap, DXGI_FORMAT_R8G8B8A8_UNORM);

	//heightMap
	ID3D12Resource* heightMapResource = 0;
	D3D12_RESOURCE_DESC heightTexDesc = createResourceDescTex2D(DXGI_FORMAT_R32_FLOAT, heightMap.lod[0].width, heightMap.lod[0].height, (u16)heightMap.lodCount);
	ASSERT(resourceManager->device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
		&heightTexDesc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&heightMapResource)) == S_OK);

	result.heightMap.d12Resource = heightMapResource;
	result.heightMap.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	uploadToTextureLod(resourceManager, &result.heightMap, &heightMap, DXGI_FORMAT_R32_FLOAT);

	endTempMemory(&tempMem);

	return result;
}
#endif

static v2 addPerlinNoiseAVX(Image2D* image, Image2D* grad, u32 gradAlignX, u32 gradAlignY, u32 tileSize, f32 heightScale, ClipRect* clipRect = 0)
{
	ASSERT(IS_POW2(grad->width) && IS_POW2(grad->height));
	ASSERT((image->pitch & 31) == 0);
	if (clipRect)
	{
		ASSERT((clipRect->minX & 7) == 0);
		ASSERT((clipRect->maxX & 7) == 0 || clipRect->maxX >= image->width);
	}

	u32 minX = clipRect ? clipRect->minX : 0;
	u32 minY = clipRect ? clipRect->minY : 0;
	u32 maxX = clipRect ? clipRect->maxX : image->width;
	u32 maxY = clipRect ? clipRect->maxY : image->height;

	m256v2 range = { _mm256_set1_ps(1e10f), _mm256_set1_ps(-1e10f) };

	__m256i gradPitch = _mm256_set1_epi32(grad->pitch);
	__m256i gradUMask = _mm256_set1_epi32(grad->width - 1);
	__m256i gradVMask = _mm256_set1_epi32(grad->height - 1);
	__m256 _0_to_7 = _mm256_setr_ps(0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f);

	__m256 tileSizeScale = _mm256_set1_ps(1.f / (f32)tileSize);
	__m256 scale = _mm256_set1_ps(heightScale);

	u8* row = image->memory + minX * sizeof(f32) + minY * image->pitch;
	for (u32 _y = minY; _y < maxY; ++_y)
	{
		__m256 y = _mm256_set1_ps((f32)_y);
		__m256 v = (y - _mm256_set1_ps((f32)gradAlignY - 0.5f)) * tileSizeScale;
		__m256i v0 = _mm256_cvtps_epi32(_mm256_round_ps(v, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
		__m256i v1 = _mm256_add_epi32(v0, _mm256_set1_epi32(1));
		__m256 dv = _mm256_sub_ps(v, _mm256_cvtepi32_ps(v0));
		v0 = _mm256_and_si256(v0, gradVMask);
		v1 = _mm256_and_si256(v1, gradVMask);

		f32* pixel = (f32*)row;
		for (u32 _x = minX; _x < maxX; _x += 8)
		{
			__m256 pixelValue = _mm256_load_ps(pixel);

			__m256 x = _mm256_set1_ps((f32)_x) + _0_to_7;

			__m256 u = (x - _mm256_set1_ps((f32)gradAlignX - 0.5f)) * tileSizeScale;
			__m256i u0 = _mm256_cvtps_epi32(_mm256_round_ps(u, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
			__m256i u1 = _mm256_add_epi32(u0, _mm256_set1_epi32(1));
			__m256 du = _mm256_sub_ps(u, _mm256_cvtepi32_ps(u0));
			u0 = _mm256_and_si256(u0, gradUMask);
			u1 = _mm256_and_si256(u1, gradUMask);

			__m256i i00 = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_set1_epi32(sizeof(v2)), u0), _mm256_mullo_epi32(v0, gradPitch));
			__m256i i10 = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_set1_epi32(sizeof(v2)), u1), _mm256_mullo_epi32(v0, gradPitch));
			__m256i i01 = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_set1_epi32(sizeof(v2)), u0), _mm256_mullo_epi32(v1, gradPitch));
			__m256i i11 = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_set1_epi32(sizeof(v2)), u1), _mm256_mullo_epi32(v1, gradPitch));

			//t00
			m256v2 t00;
			t00.x.m256_f32[0] = *(f32*)(grad->memory + i00.m256i_i32[0]);
			t00.x.m256_f32[1] = *(f32*)(grad->memory + i00.m256i_i32[1]);
			t00.x.m256_f32[2] = *(f32*)(grad->memory + i00.m256i_i32[2]);
			t00.x.m256_f32[3] = *(f32*)(grad->memory + i00.m256i_i32[3]);
			t00.x.m256_f32[4] = *(f32*)(grad->memory + i00.m256i_i32[4]);
			t00.x.m256_f32[5] = *(f32*)(grad->memory + i00.m256i_i32[5]);
			t00.x.m256_f32[6] = *(f32*)(grad->memory + i00.m256i_i32[6]);
			t00.x.m256_f32[7] = *(f32*)(grad->memory + i00.m256i_i32[7]);

			t00.y.m256_f32[0] = *(f32*)(grad->memory + i00.m256i_i32[0] + sizeof(f32));
			t00.y.m256_f32[1] = *(f32*)(grad->memory + i00.m256i_i32[1] + sizeof(f32));
			t00.y.m256_f32[2] = *(f32*)(grad->memory + i00.m256i_i32[2] + sizeof(f32));
			t00.y.m256_f32[3] = *(f32*)(grad->memory + i00.m256i_i32[3] + sizeof(f32));
			t00.y.m256_f32[4] = *(f32*)(grad->memory + i00.m256i_i32[4] + sizeof(f32));
			t00.y.m256_f32[5] = *(f32*)(grad->memory + i00.m256i_i32[5] + sizeof(f32));
			t00.y.m256_f32[6] = *(f32*)(grad->memory + i00.m256i_i32[6] + sizeof(f32));
			t00.y.m256_f32[7] = *(f32*)(grad->memory + i00.m256i_i32[7] + sizeof(f32));

			//t01
			m256v2 t01;
			t01.x.m256_f32[0] = *(f32*)(grad->memory + i01.m256i_i32[0]);
			t01.x.m256_f32[1] = *(f32*)(grad->memory + i01.m256i_i32[1]);
			t01.x.m256_f32[2] = *(f32*)(grad->memory + i01.m256i_i32[2]);
			t01.x.m256_f32[3] = *(f32*)(grad->memory + i01.m256i_i32[3]);
			t01.x.m256_f32[4] = *(f32*)(grad->memory + i01.m256i_i32[4]);
			t01.x.m256_f32[5] = *(f32*)(grad->memory + i01.m256i_i32[5]);
			t01.x.m256_f32[6] = *(f32*)(grad->memory + i01.m256i_i32[6]);
			t01.x.m256_f32[7] = *(f32*)(grad->memory + i01.m256i_i32[7]);

			t01.y.m256_f32[0] = *(f32*)(grad->memory + i01.m256i_i32[0] + sizeof(f32));
			t01.y.m256_f32[1] = *(f32*)(grad->memory + i01.m256i_i32[1] + sizeof(f32));
			t01.y.m256_f32[2] = *(f32*)(grad->memory + i01.m256i_i32[2] + sizeof(f32));
			t01.y.m256_f32[3] = *(f32*)(grad->memory + i01.m256i_i32[3] + sizeof(f32));
			t01.y.m256_f32[4] = *(f32*)(grad->memory + i01.m256i_i32[4] + sizeof(f32));
			t01.y.m256_f32[5] = *(f32*)(grad->memory + i01.m256i_i32[5] + sizeof(f32));
			t01.y.m256_f32[6] = *(f32*)(grad->memory + i01.m256i_i32[6] + sizeof(f32));
			t01.y.m256_f32[7] = *(f32*)(grad->memory + i01.m256i_i32[7] + sizeof(f32));

			//t10
			m256v2 t10;
			t10.x.m256_f32[0] = *(f32*)(grad->memory + i10.m256i_i32[0]);
			t10.x.m256_f32[1] = *(f32*)(grad->memory + i10.m256i_i32[1]);
			t10.x.m256_f32[2] = *(f32*)(grad->memory + i10.m256i_i32[2]);
			t10.x.m256_f32[3] = *(f32*)(grad->memory + i10.m256i_i32[3]);
			t10.x.m256_f32[4] = *(f32*)(grad->memory + i10.m256i_i32[4]);
			t10.x.m256_f32[5] = *(f32*)(grad->memory + i10.m256i_i32[5]);
			t10.x.m256_f32[6] = *(f32*)(grad->memory + i10.m256i_i32[6]);
			t10.x.m256_f32[7] = *(f32*)(grad->memory + i10.m256i_i32[7]);

			t10.y.m256_f32[0] = *(f32*)(grad->memory + i10.m256i_i32[0] + sizeof(f32));
			t10.y.m256_f32[1] = *(f32*)(grad->memory + i10.m256i_i32[1] + sizeof(f32));
			t10.y.m256_f32[2] = *(f32*)(grad->memory + i10.m256i_i32[2] + sizeof(f32));
			t10.y.m256_f32[3] = *(f32*)(grad->memory + i10.m256i_i32[3] + sizeof(f32));
			t10.y.m256_f32[4] = *(f32*)(grad->memory + i10.m256i_i32[4] + sizeof(f32));
			t10.y.m256_f32[5] = *(f32*)(grad->memory + i10.m256i_i32[5] + sizeof(f32));
			t10.y.m256_f32[6] = *(f32*)(grad->memory + i10.m256i_i32[6] + sizeof(f32));
			t10.y.m256_f32[7] = *(f32*)(grad->memory + i10.m256i_i32[7] + sizeof(f32));

			//t11
			m256v2 t11;
			t11.x.m256_f32[0] = *(f32*)(grad->memory + i11.m256i_i32[0]);
			t11.x.m256_f32[1] = *(f32*)(grad->memory + i11.m256i_i32[1]);
			t11.x.m256_f32[2] = *(f32*)(grad->memory + i11.m256i_i32[2]);
			t11.x.m256_f32[3] = *(f32*)(grad->memory + i11.m256i_i32[3]);
			t11.x.m256_f32[4] = *(f32*)(grad->memory + i11.m256i_i32[4]);
			t11.x.m256_f32[5] = *(f32*)(grad->memory + i11.m256i_i32[5]);
			t11.x.m256_f32[6] = *(f32*)(grad->memory + i11.m256i_i32[6]);
			t11.x.m256_f32[7] = *(f32*)(grad->memory + i11.m256i_i32[7]);

			t11.y.m256_f32[0] = *(f32*)(grad->memory + i11.m256i_i32[0] + sizeof(f32));
			t11.y.m256_f32[1] = *(f32*)(grad->memory + i11.m256i_i32[1] + sizeof(f32));
			t11.y.m256_f32[2] = *(f32*)(grad->memory + i11.m256i_i32[2] + sizeof(f32));
			t11.y.m256_f32[3] = *(f32*)(grad->memory + i11.m256i_i32[3] + sizeof(f32));
			t11.y.m256_f32[4] = *(f32*)(grad->memory + i11.m256i_i32[4] + sizeof(f32));
			t11.y.m256_f32[5] = *(f32*)(grad->memory + i11.m256i_i32[5] + sizeof(f32));
			t11.y.m256_f32[6] = *(f32*)(grad->memory + i11.m256i_i32[6] + sizeof(f32));
			t11.y.m256_f32[7] = *(f32*)(grad->memory + i11.m256i_i32[7] + sizeof(f32));


			__m256 a = smoothBlend2(dot(t00, { du, dv }), dot(t10, { du - _mm256_set1_ps(1.f), dv }), du);
			__m256 b = smoothBlend2(dot(t01, { du, dv - _mm256_set1_ps(1.f) }), dot(t11, { du - _mm256_set1_ps(1.f), dv - _mm256_set1_ps(1.f) }), du);
			__m256 c = smoothBlend2(a, b, dv);

			pixelValue = pixelValue + scale * c;

			range.x = _mm256_min_ps(pixelValue, range.x);
			range.y = _mm256_max_ps(pixelValue, range.y);

			_mm256_store_ps(pixel, pixelValue);
			pixel += 8;
		}
		row += image->pitch;
	}

	v2 result = { 1e10f, -1e10f };
	for (u32 simdIndex = 0; simdIndex < 8; ++simdIndex)
	{
		result.x = MIN(result.x, range.x.m256_f32[simdIndex]);
		result.y = MAX(result.y, range.y.m256_f32[simdIndex]);
	}

	return result;
}

static void scaleImageAVX(Image2D* image, v2 fromScale, v2 toScale)
{
	f32 _a = (toScale.y - toScale.x) / (fromScale.y - fromScale.x);
	f32 _b = toScale.x - _a * fromScale.x;

	__m256 a = _mm256_set1_ps(_a);
	__m256 b = _mm256_set1_ps(_b);

	u8* row = image->memory;
	ASSERT(image->width % 8 == 0); //just for simplicity
	for (u32 y = 0; y < image->height; ++y)
	{
		__m256* pixel = (__m256*)row;
		for (u32 x = 0; x < image->width; x += 8)
		{
			*pixel++ = a * (*pixel) + b;
		}
		row += image->pitch;
	}
}

static void combineGrayScaledImagesAVX(Image2D* dest, Image2D* red, Image2D* green, Image2D* blue)
{
	//make sky colored, use fractal height and normal map for sphere, 3D perlin noise

	ASSERT(dest->width == red->width && dest->width == green->width && dest->width == blue->width);
	ASSERT(dest->height == red->height && dest->height == green->height && dest->height == blue->height);

	ASSERT((dest->pitch & 31) == 0);
	ASSERT((red->pitch & 31) == 0);
	ASSERT((green->pitch & 31) == 0);
	ASSERT((blue->pitch & 31) == 0);

	u8* destRow = dest->memory;  
	u8* redRow = red->memory; 
	u8* greenRow = green->memory; 
	u8* blueRow = blue->memory;  

	__m256i alpha = _mm256_slli_epi32(_mm256_set1_epi32(/*255*/0), 24);

	for (u32 y = 0; y < dest->height; ++y)
	{
		u32* destPixel = (u32*)destRow;
		f32* redPixel = (f32*)redRow;
		f32* greenPixel = (f32*)greenRow;
		f32* bluePixel = (f32*)blueRow;
		for (u32 x = 0; x < dest->width; x += 8)
		{
			__m256 rf = _mm256_load_ps(redPixel);
			__m256 gf = _mm256_load_ps(greenPixel);
			__m256 bf = _mm256_load_ps(bluePixel);

			rf = rf * _mm256_set1_ps(255.f);
			gf = gf * _mm256_set1_ps(255.f);
			bf = bf * _mm256_set1_ps(255.f);

			rf = _mm256_max_ps(_mm256_set1_ps(0.f), _mm256_min_ps(_mm256_set1_ps(255.f), rf));
			gf = _mm256_max_ps(_mm256_set1_ps(0.f), _mm256_min_ps(_mm256_set1_ps(255.f), gf));
			bf = _mm256_max_ps(_mm256_set1_ps(0.f), _mm256_min_ps(_mm256_set1_ps(255.f), bf));

			__m256i ri = _mm256_cvtps_epi32(_mm256_round_ps(rf, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
			__m256i gi = _mm256_cvtps_epi32(_mm256_round_ps(gf, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
			__m256i bi = _mm256_cvtps_epi32(_mm256_round_ps(bf, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));

			gi = _mm256_slli_epi32(gi, 8);
			bi = _mm256_slli_epi32(bi, 16);
			__m256i color = _mm256_or_si256(_mm256_or_si256(_mm256_or_si256(ri, gi), bi), alpha);
			_mm256_store_si256((__m256i*)destPixel, color);
			
			destPixel += 8;
			redPixel += 8;
			greenPixel += 8;
			bluePixel += 8;
		}
		destRow += dest->pitch;
		redRow += red->pitch;
		greenRow += green->pitch;
		blueRow += blue->pitch;
	}
}

static void createFractal(MemoryArena* arena, Fractal* result, f32 zoomSpeed, u32 seed, u32 width, u32 height, u32 maxTileSize)
{
	*result = {};

	result->zoomFactor = 1.f;
	result->zoomSpeed = zoomSpeed;
	result->imageState = IMAGE_STATE_OBSOLETE;
	result->shouldRecompute = true;

	result->im = pushImage2DLod(arena, width, height, f32, 2);

	u32 computeTileSizeX = 1024;
	u32 computeTileSizeY = 512;
	u32 workCountX = (width + computeTileSizeX - 1) / computeTileSizeX;
	u32 workCountY = (height + computeTileSizeY - 1) / computeTileSizeY;
	
	result->workCount = workCountX * workCountY;
	result->works = pushArray(arena, result->workCount, ComputeFractalWork);

	ComputeFractalWork* work = result->works;
	for (u32 workIndexX = 0; workIndexX < workCountX; ++workIndexX)
	{
		for (u32 workIndexY = 0; workIndexY < workCountY; ++workIndexY)
		{
			work->fractal = result;
			work->clipRect.minX = computeTileSizeX * workIndexX;
			work->clipRect.minY = computeTileSizeY * workIndexY;
			work->clipRect.maxX = MIN(computeTileSizeX * (workIndexX + 1), width);
			work->clipRect.maxY = MIN(computeTileSizeY * (workIndexY + 1), height);

			++work;
		}
	}


	for (u32 gradIndex = 0; gradIndex < ARRAY_SIZE(result->grads); ++gradIndex)
	{
		result->grads[gradIndex].grad = pushImage2D(arena, 64, 64, v2);
		result->grads[gradIndex].gridAlignX = result->im.lod[0].width / 2;
		result->grads[gradIndex].gridAlignY = result->im.lod[0].height / 2;
		fillWithRandomGradients(&result->grads[gradIndex].grad, seed + gradIndex);
	}

	clearImage2D(&result->im.lod[0]);
	result->maxTileSize = maxTileSize;
	u32 tileSize = result->maxTileSize;
	v2 range = {};
	f32 scale = 1.f;
	for (u32 iter = 0; tileSize > 0; ++iter)
	{
		ASSERT(iter < ARRAY_SIZE(result->grads));
		FractalGrad* grad = result->grads + iter;
		range = addPerlinNoiseAVX(&result->im.lod[0], &grad->grad, grad->gridAlignX, grad->gridAlignY, tileSize, scale);
		tileSize >>= 1;
		scale /= 2.f;
		++result->layerCount;
	}
	scaleImageAVX(&result->im.lod[0], range, { 0.f, 1.f });
}

static void createColoredFractal(MemoryArena* arena, ColoredFractal* result, f32 zoomSpeed, u32 seed, u32 width, u32 height, u32 maxTileSize)
{
	result->zoomFactor = 1.f;
	result->imageState = IMAGE_STATE_OBSOLETE;
	result->shouldRecompute = false;


	for (u32 channelIndex = 0; channelIndex < 3; ++channelIndex)
	{
		createFractal(arena, &result->channels[channelIndex], zoomSpeed, seed + channelIndex, width, height, maxTileSize);
	}

	result->works = result->red.works; //just stealing it from one channel TODO:should we separate the parts (interface) of a fractal which used by the GPU fractal?
	result->workCount = result->blue.workCount;
	result->im = pushImage2DLod(arena, width, height, u32, 2);
	combineGrayScaledImagesAVX(&result->im.lod[0], &result->red.im.lod[0], &result->green.im.lod[0], &result->blue.im.lod[0]);
}

static void createHeightMapFractal(MemoryArena* arena, HeightMapFractal* result, f32 zoomSpeed, u32 seed, u32 width, u32 height, u32 maxTileSize)
{
	result->imageState = IMAGE_STATE_OBSOLETE;
	result->shouldRecompute = false;

	createFractal(arena, &result->height, zoomSpeed, seed, width, height, maxTileSize);

	result->normal = pushImage2DLod(arena, width, height, u32, 2);
}

static GPUFractal createGPUFractal(ResourceManager* resourceManager, Fractal* fractal)
{
	GPUFractal result = {};

	D3D12_RESOURCE_DESC desc = createResourceDescTex2D(DXGI_FORMAT_R32_FLOAT, fractal->im.lod[0].width, fractal->im.lod[0].height, (u16)fractal->im.lodCount);
	
	for (u32 imageIndex = 0; imageIndex < ARRAY_SIZE(result.storageImages); ++imageIndex)
	{
		TrackedResource* storageImage = result.storageImages + imageIndex;
		ASSERT(resourceManager->device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&storageImage->d12Resource)) == S_OK);
		
		storageImage->stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	}

	uploadToTextureLod(resourceManager, &result.storageImages[0], &fractal->im, DXGI_FORMAT_R32_FLOAT);

	return result;
}

static GPUFractal createGPUFractal(ResourceManager* resourceManager, ColoredFractal* fractal)
{
	GPUFractal result = {};

	D3D12_RESOURCE_DESC desc = createResourceDescTex2D(DXGI_FORMAT_R8G8B8A8_UNORM, fractal->im.lod[0].width, fractal->im.lod[0].height, (u16)fractal->im.lodCount);

	for (u32 imageIndex = 0; imageIndex < ARRAY_SIZE(result.storageImages); ++imageIndex)
	{
		TrackedResource* storageImage = result.storageImages + imageIndex;
		ASSERT(resourceManager->device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&storageImage->d12Resource)) == S_OK);

		storageImage->stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	}

	uploadToTextureLod(resourceManager, &result.storageImages[0], &fractal->im, DXGI_FORMAT_R8G8B8A8_UNORM);

	return result;
}

static GPUHeightMapFractal createGPUHeightMapFractal(ResourceManager* resourceManager, HeightMapFractal* fractal)
{
	GPUHeightMapFractal result = {};

	D3D12_RESOURCE_DESC heightDesc = createResourceDescTex2D(DXGI_FORMAT_R32_FLOAT, fractal->normal.lod[0].width, fractal->normal.lod[0].height, (u16)fractal->normal.lodCount);
	D3D12_RESOURCE_DESC normalDesc = createResourceDescTex2D(DXGI_FORMAT_R8G8B8A8_UNORM, fractal->normal.lod[0].width, fractal->normal.lod[0].height, (u16)fractal->normal.lodCount);


	for (u32 heightMapIndex = 0; heightMapIndex < ARRAY_SIZE(result.storedHeightMaps); ++heightMapIndex)
	{
		GPUHeightMap* heightMap = result.storedHeightMaps + heightMapIndex;

		ASSERT(resourceManager->device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
			&heightDesc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&heightMap->height.d12Resource)) == S_OK);

		ASSERT(resourceManager->device->CreateCommittedResource(&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
			&normalDesc, D3D12_RESOURCE_STATE_COPY_DEST, 0, IID_PPV_ARGS(&heightMap->normal.d12Resource)) == S_OK);

		heightMap->height.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
		heightMap->normal.stateAfterModification = D3D12_RESOURCE_STATE_COPY_DEST;
	}

	uploadToTextureLod(resourceManager, &result.storedHeightMaps[0].height, &fractal->height.im, DXGI_FORMAT_R32_FLOAT);
	uploadToTextureLod(resourceManager, &result.storedHeightMaps[0].normal, &fractal->normal, DXGI_FORMAT_R8G8B8A8_UNORM);

	return result;
}

static void precomputeFractal(void* data)
{
	Fractal* fractal = (Fractal*)data;

	//copy the middle of lod0 to lod1
	{
		u8* lod1Row = fractal->im.lod[1].memory;
		u8* lod0Row = fractal->im.lod[0].memory + fractal->im.lod[0].pitch*fractal->im.lod[0].height / 4;
		for (u32 y = 0; y < fractal->im.lod[1].height; ++y)
		{
			f32* lod1Pixel = (f32*)lod1Row;
			f32* lod0Pixel = (f32*)lod0Row + fractal->im.lod[0].width / 4;
			for (u32 x = 0; x < fractal->im.lod[1].width; ++x)
			{
				*lod1Pixel++ = *lod0Pixel++;
			}
			lod1Row += fractal->im.lod[1].pitch;
			lod0Row += fractal->im.lod[0].pitch;
		}
	}

	fractal->currentBaseGradIndex = (fractal->currentBaseGradIndex + 1) % ARRAY_SIZE(fractal->grads);

	FractalGrad* newGrad = fractal->grads + ((fractal->currentBaseGradIndex + fractal->layerCount - 1) % ARRAY_SIZE(fractal->grads));
	
	//beginTicketMutex(&g_randMutex);
	fillWithRandomGradients(&newGrad->grad, rand());
	//endTicketMutex(&g_randMutex);

	clearImage2D(&fractal->im.lod[0]);

	_InterlockedDecrement((volatile LONG*)&fractal->partsInFlightCount);
}

static void computeFractal(void* data)
{
	ComputeFractalWork* work = (ComputeFractalWork*)data;
	Fractal* fractal = work->fractal;
	ClipRect* clipRect = &work->clipRect;

	u32 tileSize = fractal->maxTileSize;
	f32 scale = 1.f;
	v2 range = {};
	for (u32 iter = 0; tileSize > 0; ++iter)
	{
		FractalGrad* grad = fractal->grads + (fractal->currentBaseGradIndex + iter) % ARRAY_SIZE(fractal->grads);
		range = addPerlinNoiseAVX(&fractal->im.lod[0], &grad->grad, grad->gridAlignX, grad->gridAlignY, tileSize, scale, clipRect);
		scale /= 2.f;
		tileSize >>= 1;
	}
	work->range = range;

_InterlockedDecrement((volatile LONG*)&fractal->partsInFlightCount);
}

static void postComputeFractal(void* data)
{
	Fractal* fractal = (Fractal*)data;

	v2 range = { 1e10f, -1e10f };

	for (u32 workIndex = 0; workIndex < fractal->workCount; ++workIndex)
	{
		ComputeFractalWork* work = fractal->works + workIndex;
		range.x = MIN(range.x, work->range.x);
		range.y = MAX(range.y, work->range.y);
	}

	scaleImageAVX(&fractal->im.lod[0], range, { 0.f, 1.f });
	_InterlockedDecrement((volatile LONG*)&fractal->partsInFlightCount);
}




static void updateFractal(WorkQueue* queue, Fractal* fractal, f32 dt)
{
	fractal->zoomFactor *= MAX(0.5f, 1.f - dt * fractal->zoomSpeed);

	b32 repeat = true;

	while (repeat)
	{
		repeat = false;
		if (fractal->imageState == IMAGE_STATE_OBSOLETE && fractal->shouldRecompute)
		{
			ASSERT(fractal->partsInFlightCount == 0);
			fractal->partsInFlightCount = 1;
			_WriteBarrier();
			pushEntry(queue, fractal, precomputeFractal);
			fractal->imageState = IMAGE_STATE_PRECOMPUTING;
			fractal->shouldRecompute = false;

			fractal->DEBUGstartComputeTime = Win32GetWallClock();

		}
		else if (fractal->imageState == IMAGE_STATE_PRECOMPUTING && fractal->partsInFlightCount == 0)
		{
			fractal->partsInFlightCount = fractal->workCount;
			_WriteBarrier();
			for (u32 workIndex = 0; workIndex < fractal->workCount; ++workIndex)
			{
				pushEntry(queue, fractal->works + workIndex, computeFractal);
			}
			fractal->imageState = IMAGE_STATE_COMPUTING;
		}
		else if (fractal->imageState == IMAGE_STATE_COMPUTING && fractal->partsInFlightCount == 0)
		{
			fractal->partsInFlightCount = 1;
			_WriteBarrier();
			pushEntry(queue, fractal, postComputeFractal);
			fractal->imageState = IMAGE_STATE_POSTCOMPUTING;
		}
		else if (fractal->imageState == IMAGE_STATE_POSTCOMPUTING && fractal->partsInFlightCount == 0)
		{
			fractal->imageState = IMAGE_STATE_READY;
			f32 computeTime = Win32GetSecondsElapsed(fractal->DEBUGstartComputeTime, Win32GetWallClock());
			char buff[256];
			sprintf_s(buff, "Fractal compute time: %fs\n", computeTime);
			OutputDebugStringA(buff);
		}

		if (fractal->zoomFactor < 0.5f)
		{
			if (fractal->imageState == IMAGE_STATE_PRECOMPUTING ||
				fractal->imageState == IMAGE_STATE_COMPUTING ||
				fractal->imageState == IMAGE_STATE_POSTCOMPUTING
				)
			{
				//NOTE: this shouldn't happen, but if it does, we have to block until the image is ready

				START_TIMER(WaitForFractalImage);
				busyWaitWhile(fractal->partsInFlightCount != 0);
				END_TIMER(WaitForFractalImage);
				repeat = true;
			}
			else
			{
				fractal->zoomFactor *= 2.f;
				fractal->resetHappened = true;
				ASSERT(fractal->shouldRecompute == false);
				fractal->shouldRecompute = true;
			}
		}
	}
}

static void postComputeColoredFractal(void* data)
{
	ColoredFractal* fractal = (ColoredFractal*)data;

	//copy the middle of lod0 to lod1
	{
		u8* lod1Row = fractal->im.lod[1].memory;
		u8* lod0Row = fractal->im.lod[0].memory + fractal->im.lod[0].pitch*fractal->im.lod[0].height / 4;
		for (u32 y = 0; y < fractal->im.lod[1].height; ++y)
		{
			u32* lod1Pixel = (u32*)lod1Row;
			u32* lod0Pixel = (u32*)lod0Row + fractal->im.lod[0].width / 4;
			memcpy(lod1Pixel, lod0Pixel, sizeof(u32)*fractal->im.lod[1].width);
			
			lod1Row += fractal->im.lod[1].pitch;
			lod0Row += fractal->im.lod[0].pitch;
		}
	}
	combineGrayScaledImagesAVX(&fractal->im.lod[0], &fractal->red.im.lod[0], &fractal->green.im.lod[0], &fractal->blue.im.lod[0]);

	_InterlockedDecrement((volatile LONG*)&fractal->partsInFlightCount);
}

static void computeFractalNormalMap(void* data)
{
	HeightMapFractal* fractal = (HeightMapFractal*)data;

	{
		//TODO: make an image copy region function
		u8* lod1Row = fractal->normal.lod[1].memory;
		u8* lod0Row = fractal->normal.lod[0].memory + fractal->normal.lod[0].pitch*fractal->normal.lod[0].height / 4;
		for (u32 y = 0; y < fractal->normal.lod[1].height; ++y)
		{
			u32* lod1Pixel = (u32*)lod1Row;
			u32* lod0Pixel = (u32*)lod0Row + fractal->normal.lod[0].width / 4;
			for (u32 x = 0; x < fractal->normal.lod[1].width; ++x)
			{
				v3 n = unpackNormal(*lod0Pixel++);
				n /= n.z;
				n.x *= 0.5f;
				n.y *= 0.5f;
				*lod1Pixel++ = packNormal(n);
			}
		
			lod1Row += fractal->normal.lod[1].pitch;
			lod0Row += fractal->normal.lod[0].pitch;
		}
	}

	fillNormalMapForHeightMap(&fractal->height.im.lod[0], &fractal->normal.lod[0]);

	_InterlockedDecrement((volatile LONG*)&fractal->partsInFlightCount);
}

static void updateFractal(WorkQueue* queue, HeightMapFractal* fractal, f32 dt)
{
	updateFractal(queue, &fractal->height, dt);

	b32 repeat = true;
	while (repeat)
	{
		repeat = false;

		if (fractal->imageState == IMAGE_STATE_OBSOLETE && fractal->shouldRecompute)
		{
			fractal->shouldRecompute = false;

			ASSERT(fractal->height.imageState == IMAGE_STATE_READY);
			fractal->height.shouldRecompute = true;
			fractal->height.imageState = IMAGE_STATE_OBSOLETE;

			fractal->imageState = IMAGE_STATE_COMPUTING_HEIGHT_MAP;
		}
		if (fractal->imageState == IMAGE_STATE_COMPUTING_HEIGHT_MAP && fractal->height.imageState == IMAGE_STATE_READY)
		{
			fractal->partsInFlightCount = 1;
			_WriteBarrier();
			pushEntry(queue, fractal, computeFractalNormalMap);
			fractal->imageState = IMAGE_STATE_COMPUTING_NORMAL_MAP;
		}
		if (fractal->imageState == IMAGE_STATE_COMPUTING_NORMAL_MAP && fractal->partsInFlightCount == 0)
		{
			fractal->imageState = IMAGE_STATE_READY;
		}
		if (fractal->height.resetHappened)
		{
			if (fractal->imageState == IMAGE_STATE_COMPUTING_HEIGHT_MAP ||
				fractal->imageState == IMAGE_STATE_COMPUTING_NORMAL_MAP
				)
			{
				ASSERT(fractal->height.imageState == IMAGE_STATE_READY);

				//NOTE: this shouldn't happen, but if it does, we have to block until the image is ready

				START_TIMER(WaitForHeightMapFractalImage);
				busyWaitWhile(fractal->partsInFlightCount != 0);
				END_TIMER(WaitForHeightMapFractalImage);
				repeat = true;
			}
			else
			{
				fractal->height.resetHappened = false;

				fractal->resetHappened = true;
				ASSERT(fractal->shouldRecompute == false);
				fractal->shouldRecompute = true;
			}
		}
	}
}

static void updateFractal(WorkQueue* queue, ColoredFractal* fractal, f32 dt)
{
	updateFractal(queue, &fractal->channels[0], dt);
	updateFractal(queue, &fractal->channels[1], dt);
	updateFractal(queue, &fractal->channels[2], dt);

	fractal->zoomFactor = fractal->red.zoomFactor;

	b32 repeat = true;
	while (repeat)
	{
		repeat = false;
		if (fractal->shouldRecompute && fractal->imageState == IMAGE_STATE_OBSOLETE)
		{
			fractal->shouldRecompute = false;

			ASSERT(fractal->channels[0].imageState == IMAGE_STATE_READY);
			ASSERT(fractal->channels[1].imageState == IMAGE_STATE_READY);
			ASSERT(fractal->channels[2].imageState == IMAGE_STATE_READY);

			fractal->channels[0].imageState = IMAGE_STATE_OBSOLETE;
			fractal->channels[1].imageState = IMAGE_STATE_OBSOLETE;
			fractal->channels[2].imageState = IMAGE_STATE_OBSOLETE;

			fractal->channels[0].shouldRecompute = true;
			fractal->channels[1].shouldRecompute = true;
			fractal->channels[2].shouldRecompute = true;

			fractal->imageState = IMAGE_STATE_COMPUTING_CHANNELS;

			fractal->DEBUGstartComputeTime = Win32GetWallClock();
		}

		if (fractal->imageState == IMAGE_STATE_COMPUTING_CHANNELS &&
			fractal->channels[0].imageState == IMAGE_STATE_READY &&
			fractal->channels[1].imageState == IMAGE_STATE_READY &&
			fractal->channels[2].imageState == IMAGE_STATE_READY
			)
		{
			fractal->partsInFlightCount = 1;
			_WriteBarrier();
			pushEntry(queue, fractal, postComputeColoredFractal);
			fractal->imageState = IMAGE_STATE_POSTCOMPUTING;
		}
		if (fractal->imageState == IMAGE_STATE_POSTCOMPUTING && fractal->partsInFlightCount == 0)
		{
			fractal->imageState = IMAGE_STATE_READY;
			f32 computeTime = Win32GetSecondsElapsed(fractal->DEBUGstartComputeTime, Win32GetWallClock());
			char buff[256];
			sprintf_s(buff, "Colored fractal compute time: %fs\n", computeTime);
			OutputDebugStringA(buff);
		}

		if (fractal->red.resetHappened)
		{
			ASSERT(fractal->blue.resetHappened && fractal->green.resetHappened);
			

			if (fractal->imageState == IMAGE_STATE_COMPUTING_CHANNELS ||
				fractal->imageState == IMAGE_STATE_POSTCOMPUTING
				)
			{
				ASSERT(fractal->channels[0].imageState == IMAGE_STATE_READY);
				ASSERT(fractal->channels[1].imageState == IMAGE_STATE_READY);
				ASSERT(fractal->channels[2].imageState == IMAGE_STATE_READY);

				//NOTE: this shouldn't happen, but if it does, we have to block until the image is ready

				START_TIMER(WaitForColoredFractalImage);
				busyWaitWhile(fractal->partsInFlightCount != 0);
				END_TIMER(WaitForColoredFractalImage);
				repeat = true;
			}
			else
			{
				fractal->channels[0].resetHappened = false;
				fractal->channels[1].resetHappened = false;
				fractal->channels[2].resetHappened = false;

				fractal->resetHappened = true;
				ASSERT(fractal->shouldRecompute == false);
				fractal->shouldRecompute = true;
			}


		}
	}
}

inline f32 sample2D1F32(Image2D* image, SampleParams2D s)
{
	f32 c00 = fetchSample(image, s.u0, s.v0, f32);
	f32 c10 = fetchSample(image, s.u1, s.v0, f32);
	f32 c01 = fetchSample(image, s.u0, s.v1, f32);
	f32 c11 = fetchSample(image, s.u1, s.v1, f32);

	f32 a = lerp(c00, c10, s.du);
	f32 b = lerp(c01, c11, s.du);
	f32 c = lerp(a, b, s.dv);
	return c;
}

static HeightMap createHeightMapForSphere(MemoryArena* arena, u32 width, u32 height, u32 gradSeed, f32 heightScale, u32 maxIterCount = 0xffffffff)
{
	TIMED_BLOCK();

	HeightMap result = {};

	result.height = pushImage2DLod(arena, width, height, f32);
	result.normal = pushImage2DLod(arena, width, height, u32);

	TempMemory temp = startTempMemory(arena);
	Image2D north = pushImage2D(arena, width, height, f32);
	Image2D south = pushImage2D(arena, width, height, f32);

	Image2D northGrad = pushImage2D(arena, 64, 64, v2);
	Image2D southGrad = pushImage2D(arena, 64, 64, v2);

	fillWithRandomGradients(&northGrad, gradSeed);
	fillWithRandomGradients(&southGrad, gradSeed);

	clearImage2D(&result.height.lod[0]);
	u32 tileSize = 1024;
	f32 scale = heightScale;
	while (tileSize)
	{
		addPerlinNoiseAVX(&north, &northGrad, 0, 0, tileSize, scale);
		addPerlinNoiseAVX(&south, &southGrad, 0, 0, tileSize, scale);
		tileSize >>= 1;
		scale /= 2.f;
	}

	u8* rowNorth = north.memory;
	u8* rowDst= result.height.lod[0].memory;

	for (u32 y = 0; y < height; ++y)
	{
		f32* pixelNorth = (f32*)rowNorth;
		f32* pixelDst= (f32*)rowDst;
		for (u32 x = 0; x < width; ++x)
		{
			f32 northSampl = *pixelNorth++;
			f32 southSampl = 0.f;

			v2 uv = { (f32)x / (f32)width , (f32)y / (f32)height };
			uv = 2.f*uv - v2{ 1.f,1.f };
			f32 r = length(uv);
			if (r > 0.f)
			{

				v2 uvInv = (MAX(0.f, 0.99f-r)/r)*uv;
				uvInv = 0.5f*uvInv + v2{ 0.5f, 0.5f };
				SampleParams2D s = getSampleParams(width, height, uvInv.x, uvInv.y);
				southSampl = sample2D1F32(&south, s);
			}

			f32 t = CLAMP(0.f, 1.f, 2.f*(r - 0.5f) + 0.5f);
			*pixelDst++ = smoothBlend2(northSampl, southSampl, r);
		}
		rowNorth += north.pitch;
		rowDst += result.height.lod[0].pitch;
	}

	START_TIMER(GenerateMipLevelsForHeightMap);
	generateMipLevels1F32AVX(&result.height);
	END_TIMER(GenerateMipLevelsForHeightMap);

	START_TIMER(GenerateNormalMapFromHeightMap);
	for (u32 lod = 0; lod < result.height.lodCount; ++lod)
	{
		fillNormalMapForHeightMap(&result.height.lod[lod], &result.normal.lod[lod]);
	}
	END_TIMER(GenerateNormalMapFromHeightMap);

	endTempMemory(&temp);

	return result;
}

static HeightMap createHeightMapForTorus(MemoryArena* arena, u32 width, u32 height, u32 gradSeed, f32 heightScale, u32 maxIterCount = 0xffffffff)
{
	HeightMap result = {};

	TIMED_BLOCK();

	result.height = pushImage2DLod(arena, width, height, f32);
	result.normal = pushImage2DLod(arena, width, height, u32);

	TempMemory temp = startTempMemory(arena);
	Image2D grad = pushImage2D(arena, 64, 64, v2);

	fillWithRandomGradients(&grad, gradSeed);
	clearImage2D(&result.height.lod[0]);
	u32 tileSize = 1024;
	f32 scale = heightScale;
	while (tileSize)
	{
		addPerlinNoiseAVX(&result.height.lod[0], &grad, 0, 0, tileSize, scale);
		tileSize >>= 1;
		scale /= 2.f;
	}

	u32 blendPixelCount = width / 36;
	wrapImage1F32(&result.height.lod[0], false, blendPixelCount);
	wrapImage1F32(&result.height.lod[0], true, blendPixelCount);

	generateMipLevels1F32AVX(&result.height);

	for (u32 lod = 0; lod < result.height.lodCount; ++lod)
	{
		fillNormalMapForHeightMap(&result.height.lod[lod], &result.normal.lod[lod]);
	}

	endTempMemory(&temp);

	return result;
}

static GraphicsPipeline createTerrainPipeline(ID3D12Device2* device)
{
	GraphicsPipeline result = {};

	{
		ID3DBlob* signatureBlob = 0;
		ID3DBlob* errorBlob = 0;

		D3D12_DESCRIPTOR_RANGE texRange = {};
		texRange.BaseShaderRegister = 0;
		texRange.RegisterSpace = 0;
		texRange.NumDescriptors = 3;
		texRange.OffsetInDescriptorsFromTableStart = 0;
		texRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

		D3D12_ROOT_PARAMETER rootParams[] =
		{
			InitAsConstantsBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL),
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
		samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		samplers[1].ShaderRegister = 1;
		samplers[1].MaxLOD = D3D12_FLOAT32_MAX;


		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			//D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			//D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
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
			IID_PPV_ARGS(&result.desc.pRootSignature)) == S_OK);
		signatureBlob->Release();
	}

	{
		result.inputElementDescs[0] = createInputElementDesc("POSITION", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, position));
		result.inputElementDescs[1] = createInputElementDesc("NORMAL", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, normal));
		result.inputElementDescs[2] = createInputElementDesc("TANGENT", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, tangent));
		result.inputElementDescs[3] = createInputElementDesc("BITANGENT", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, bitangent));
		result.inputElementDescs[4] = createInputElementDesc("UV", DXGI_FORMAT_R32G32_FLOAT, offsetof(Vertex, uv));
		result.inputElementDescs[5] = createInputElementDesc("SHAPE_OP", DXGI_FORMAT_R32G32B32A32_FLOAT, offsetof(Vertex, shapeOp));


		result.desc.InputLayout = { result.inputElementDescs, 6 };

		result.desc.DepthStencilState.DepthEnable = true;
		result.desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		result.desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		result.desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

		result.desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
		result.desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		result.desc.RasterizerState.FrontCounterClockwise = true;
		result.desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		result.desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		result.desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		result.desc.RasterizerState.DepthClipEnable = TRUE;
		result.desc.RasterizerState.MultisampleEnable = FALSE;
		result.desc.RasterizerState.AntialiasedLineEnable = FALSE;
		result.desc.RasterizerState.ForcedSampleCount = 0;
		result.desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		result.desc.BlendState.RenderTarget[0] =
		{
			FALSE,FALSE,
			D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
			D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
			D3D12_LOGIC_OP_NOOP,
			D3D12_COLOR_WRITE_ENABLE_ALL,
		};

		result.desc.SampleMask = UINT_MAX;
		result.desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
		result.desc.NumRenderTargets = 1;
		result.desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		result.desc.SampleDesc.Count = 1;

		result.shaders[SHADER_VERTEX].name = L"tessShaderVS.cso";
		result.shaders[SHADER_HULL].name = L"tessShaderHS.cso";
		result.shaders[SHADER_DOMAIN].name = L"tessShaderDS.cso";
		result.shaders[SHADER_PIXEL].name = L"pbrShaderPS.cso";
		
		rebuildGraphicsPipeline(device, &result);
	}
	return result;
}

GraphicsPipeline createFractalPipeline(ID3D12Device2* device)
{
	GraphicsPipeline result = {};

	{
		ID3DBlob* signatureBlob = 0;
		ID3DBlob* errorBlob = 0;

		D3D12_DESCRIPTOR_RANGE texRange = {};
		texRange.BaseShaderRegister = 0;
		texRange.RegisterSpace = 0;
		texRange.RegisterSpace = 0;
		texRange.NumDescriptors = 1;
		texRange.OffsetInDescriptorsFromTableStart = 2;
		texRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

		D3D12_ROOT_PARAMETER rootParams[] =
		{
			InitAsConstantsBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL),
			InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_ALL),
			InitAsConstantsBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL),
		};
		D3D12_STATIC_SAMPLER_DESC samplers[1] = {};
		samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		samplers[0].MaxLOD = D3D12_FLOAT32_MAX;

		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
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
			IID_PPV_ARGS(&result.desc.pRootSignature)) == S_OK);
		signatureBlob->Release();
	}

	{
		result.inputElementDescs[0] = createInputElementDesc("POSITION", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, position));
		result.inputElementDescs[1] = createInputElementDesc("NORMAL", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, normal));
		result.inputElementDescs[2] = createInputElementDesc("TANGENT", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, tangent));
		result.inputElementDescs[3] = createInputElementDesc("BITANGENT", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, bitangent));
		result.inputElementDescs[4] = createInputElementDesc("UV", DXGI_FORMAT_R32G32_FLOAT, offsetof(Vertex, uv));
		result.inputElementDescs[5] = createInputElementDesc("SHAPE_OP", DXGI_FORMAT_R32G32B32A32_FLOAT, offsetof(Vertex, shapeOp));


		result.desc.InputLayout = { result.inputElementDescs, 6 };

		result.desc.DepthStencilState.DepthEnable = true;
		result.desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		result.desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		result.desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

		result.desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		result.desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		result.desc.RasterizerState.FrontCounterClockwise = true;
		result.desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		result.desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		result.desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		result.desc.RasterizerState.DepthClipEnable = TRUE;
		result.desc.RasterizerState.MultisampleEnable = FALSE;
		result.desc.RasterizerState.AntialiasedLineEnable = FALSE;
		result.desc.RasterizerState.ForcedSampleCount = 0;
		result.desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		result.desc.BlendState.RenderTarget[0] =
		{
			FALSE,FALSE,
			D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
			D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
			D3D12_LOGIC_OP_NOOP,
			D3D12_COLOR_WRITE_ENABLE_ALL,
		};

		result.desc.SampleMask = UINT_MAX;
		result.desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		result.desc.NumRenderTargets = 1;
		result.desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		result.desc.SampleDesc.Count = 1;

		result.shaders[SHADER_VERTEX].name = L"simpleShaderVS.cso";
		result.shaders[SHADER_PIXEL].name = L"fractalShaderPS.cso";

		rebuildGraphicsPipeline(device, &result);
	}
	return result;
}

static void createHeightMappingPipeline(GraphicsPipeline* result, ID3D12Device2* device)
{
	*result = {};

	{
		ID3DBlob* signatureBlob = 0;
		ID3DBlob* errorBlob = 0;

		D3D12_DESCRIPTOR_RANGE texRange = {};
		texRange.BaseShaderRegister = 0;
		texRange.RegisterSpace = 0;
		texRange.RegisterSpace = 0;
		texRange.NumDescriptors = 6;
		texRange.OffsetInDescriptorsFromTableStart = 0;
		texRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;

		D3D12_ROOT_PARAMETER rootParams[] =
		{
			InitAsConstantsBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL),
			InitAsDescriptorTable(1, &texRange, D3D12_SHADER_VISIBILITY_ALL),
			InitAsConstantsBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL),
		};
		D3D12_STATIC_SAMPLER_DESC samplers[1] = {};
		samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		samplers[0].MaxLOD = D3D12_FLOAT32_MAX;

		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
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
			IID_PPV_ARGS(&result->desc.pRootSignature)) == S_OK);
		signatureBlob->Release();
	}

	{
		result->inputElementDescs[0] = createInputElementDesc("POSITION", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, position));
		result->inputElementDescs[1] = createInputElementDesc("NORMAL", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, normal));
		result->inputElementDescs[2] = createInputElementDesc("TANGENT", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, tangent));
		result->inputElementDescs[3] = createInputElementDesc("BITANGENT", DXGI_FORMAT_R32G32B32_FLOAT, offsetof(Vertex, bitangent));
		result->inputElementDescs[4] = createInputElementDesc("UV", DXGI_FORMAT_R32G32_FLOAT, offsetof(Vertex, uv));
		result->inputElementDescs[5] = createInputElementDesc("SHAPE_OP", DXGI_FORMAT_R32G32B32A32_FLOAT, offsetof(Vertex, shapeOp));


		result->desc.InputLayout = { result->inputElementDescs, 6 };

		result->desc.DepthStencilState.DepthEnable = true;
		result->desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		result->desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		result->desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

		result->desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		result->desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		result->desc.RasterizerState.FrontCounterClockwise = true;
		result->desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		result->desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		result->desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		result->desc.RasterizerState.DepthClipEnable = TRUE;
		result->desc.RasterizerState.MultisampleEnable = FALSE;
		result->desc.RasterizerState.AntialiasedLineEnable = FALSE;
		result->desc.RasterizerState.ForcedSampleCount = 0;
		result->desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		result->desc.BlendState.RenderTarget[0] =
		{
			FALSE,FALSE,
			D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
			D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
			D3D12_LOGIC_OP_NOOP,
			D3D12_COLOR_WRITE_ENABLE_ALL,
		};

		result->desc.SampleMask = UINT_MAX;
		result->desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		result->desc.NumRenderTargets = 1;
		result->desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		result->desc.SampleDesc.Count = 1;

		result->shaders[SHADER_VERTEX].name = L"heightMappingShaderVS.cso";
		result->shaders[SHADER_PIXEL].name = L"pbrNormalCalcShaderPS.cso";

		rebuildGraphicsPipeline(device, result);
	}
}

struct DrawSetting
{
	D3D12_VERTEX_BUFFER_VIEW vb;
	D3D12_INDEX_BUFFER_VIEW ib;
	u32 indexCount;
	u32 modelBufferIndex;
	D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHandle;
};

struct FrameResource
{
	ConstantBuffer gpuModelBuffers;
	ConstantBuffer gpuSceneBuffer;

	CommandAllocator commandAllocator;
	ID3D12Resource* backbuffer;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv;
};

#define BACKBUFFER_COUNT 3
struct Renderer
{
	DrawSetting* drawSettings;
	u32 maxModelCount;
	u32 modelCount;

	SceneBuffer sceneBuffer;

	v4 clearColor;

	FrameResource frameResources[BACKBUFFER_COUNT];
	ModelBuffer* currentModelBuffers;
	SceneBuffer* currentSceneBuffer;


	CommandQueue renderQueue;
	CommandList renderCommandList;

	ID3D12Resource* uploadHeap;
	u8* uploadHeapBegin;
	u32 modelBuffersOffset;
	u32 sceneBufferOffset;
	u32 uploadHeapSizePerFrame;

	ID3D12DescriptorHeap* descriptorHeap;
	ID3D12DescriptorHeap* dsvHeap;
	ID3D12DescriptorHeap* rtvHeap;
	D3D12_GPU_DESCRIPTOR_HANDLE gpuDescriptorHeapBegin;
	D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorHeapBegin;
	UINT descriptorHandleIncrementSize;

	ID3D12Resource* depthBuffer;

	u32 descriptorCount;
	u32 maxDescriptorCount;

	GraphicsPipeline modelPipeline;

	ID3D12Device2* device;
	IDXGISwapChain4* swapChain;
	u32 backbufferWidth;
	u32 backbufferHeight;
};

static void uploadFractalImageInPieces(ResourceManager* resourceManager, Image2DLod* image, TrackedResource* gpuImage, DXGI_FORMAT format, ComputeFractalWork* uploads, u32 uploadCount)
{
	for (u32 uploadIndex = 0; uploadIndex < uploadCount; ++uploadIndex)
	{
		ClipRect* clipRect = &uploads[uploadIndex].clipRect;

		Image2DLodRegion region = {};
		region.minX = clipRect->minX;
		region.minY = clipRect->minY;
		region.width = clipRect->maxX - clipRect->minX;
		region.height = clipRect->maxY - clipRect->minY;
		uploadToTextureLod(resourceManager, gpuImage, image, format, &region);
	}
}

static void updateGPUFractal(ResourceManager* resourceManager, Renderer* renderer, Fractal* fractal, GPUFractal* gpuFractal)
{
	if (fractal->imageState == IMAGE_STATE_READY)
	{
		u32 nextImageIndex = (gpuFractal->imageIndex + 1) % ARRAY_SIZE(gpuFractal->storageImages);
		TrackedResource* gpuImage = gpuFractal->storageImages + nextImageIndex;
		if(gpuFractal->uploadWorkIndex == 0)
		{
			markModify(gpuImage, &renderer->renderQueue, renderer->renderCommandList.d12CommandList, D3D12_RESOURCE_STATE_COMMON);
			//NOTE: It shouldn't create a deadlock, the renderer won't use this image for drawing, since it is not ready, so the requested draw will just be discarded 
		}

		u32 maxUploadCountPerFrame = MAX(1, fractal->workCount / 5);
		u32 uploadCount = MIN(maxUploadCountPerFrame, fractal->workCount - gpuFractal->uploadWorkIndex);
		uploadFractalImageInPieces(resourceManager, &fractal->im, gpuImage, DXGI_FORMAT_R32_FLOAT,
			fractal->works + gpuFractal->uploadWorkIndex, uploadCount);
		gpuFractal->uploadWorkIndex += uploadCount;

		if (gpuFractal->uploadWorkIndex == fractal->workCount && uploadCount == 0)
		{
			Image2DLodRegion region = {};
			region.width = fractal->im.lod[1].width;
			region.height = fractal->im.lod[1].height;
			region.lod = 1;
			uploadToTextureLod(resourceManager, gpuImage, &fractal->im, DXGI_FORMAT_R32_FLOAT, &region);

			fractal->imageState = IMAGE_STATE_OBSOLETE;
			gpuFractal->uploadWorkIndex = 0;
		}
	}
	if (fractal->resetHappened)
	{
		gpuFractal->imageIndex = (gpuFractal->imageIndex + 1) % ARRAY_SIZE(gpuFractal->storageImages);
		fractal->resetHappened = false;
	}
}

static void updateGPUFractal(ResourceManager* resourceManager, Renderer* renderer, ColoredFractal* fractal, GPUFractal* gpuFractal)
{
	if (fractal->imageState == IMAGE_STATE_READY)
	{
		u32 nextImageIndex = (gpuFractal->imageIndex + 1) % ARRAY_SIZE(gpuFractal->storageImages);
		TrackedResource* gpuImage = gpuFractal->storageImages + nextImageIndex;
		if (gpuFractal->uploadWorkIndex == 0)
		{
			markModify(gpuImage, &renderer->renderQueue, renderer->renderCommandList.d12CommandList, D3D12_RESOURCE_STATE_COMMON);
		}

		u32 maxUploadCountPerFrame = MAX(1, fractal->workCount / 5);
		u32 uploadCount = MIN(maxUploadCountPerFrame, fractal->workCount - gpuFractal->uploadWorkIndex);
		uploadFractalImageInPieces(resourceManager, &fractal->im, gpuImage, DXGI_FORMAT_R8G8B8A8_UNORM,
			fractal->works + gpuFractal->uploadWorkIndex, uploadCount);
		gpuFractal->uploadWorkIndex += uploadCount;

		if (gpuFractal->uploadWorkIndex == fractal->workCount && uploadCount == 0)
		{
			Image2DLodRegion region = {};
			region.width = fractal->im.lod[1].width;
			region.height = fractal->im.lod[1].height;
			region.lod = 1;
			uploadToTextureLod(resourceManager, gpuImage, &fractal->im, DXGI_FORMAT_R8G8B8A8_UNORM, &region);

			fractal->imageState = IMAGE_STATE_OBSOLETE;
			gpuFractal->uploadWorkIndex = 0;
		}
	}
	if (fractal->resetHappened)
	{
		ASSERT(gpuFractal->uploadWorkIndex == 0);
		gpuFractal->imageIndex = (gpuFractal->imageIndex + 1) % ARRAY_SIZE(gpuFractal->storageImages);
		fractal->resetHappened = false;
	}
}

static void updateGPUFractal(ResourceManager* resourceManager, Renderer* renderer, HeightMapFractal* fractal, GPUHeightMapFractal* gpuFractal)
{
	if (fractal->imageState == IMAGE_STATE_READY)
	{
		u32 nextImageIndex = (gpuFractal->imageIndex + 1) % ARRAY_SIZE(gpuFractal->storedHeightMaps);
		GPUHeightMap* gpuHeightMap = gpuFractal->storedHeightMaps + nextImageIndex;
		if (gpuFractal->uploadWorkIndex == 0)
		{
			markModify(&gpuHeightMap->height, &renderer->renderQueue, renderer->renderCommandList.d12CommandList, D3D12_RESOURCE_STATE_COMMON);
			markModify(&gpuHeightMap->normal, &renderer->renderQueue, renderer->renderCommandList.d12CommandList, D3D12_RESOURCE_STATE_COMMON);
		}

		u32 maxUploadCountPerFrame = MAX(1, fractal->height.workCount / 5);
		u32 uploadCount = MIN(maxUploadCountPerFrame, fractal->height.workCount - gpuFractal->uploadWorkIndex);
		
		uploadFractalImageInPieces(resourceManager, &fractal->height.im, &gpuHeightMap->height, DXGI_FORMAT_R32_FLOAT,
			fractal->height.works + gpuFractal->uploadWorkIndex, uploadCount);
		uploadFractalImageInPieces(resourceManager, &fractal->normal, &gpuHeightMap->normal, DXGI_FORMAT_R8G8B8A8_UNORM,
			fractal->height.works + gpuFractal->uploadWorkIndex, uploadCount);

		gpuFractal->uploadWorkIndex += uploadCount;

		if (gpuFractal->uploadWorkIndex == fractal->height.workCount && uploadCount == 0)
		{
			Image2DLodRegion region = {};
			region.width = fractal->normal.lod[1].width;
			region.height = fractal->normal.lod[1].height;
			region.lod = 1;
			uploadToTextureLod(resourceManager, &gpuHeightMap->height, &fractal->height.im, DXGI_FORMAT_R32_FLOAT, &region);
			uploadToTextureLod(resourceManager, &gpuHeightMap->normal, &fractal->normal, DXGI_FORMAT_R8G8B8A8_UNORM, &region);

			fractal->imageState = IMAGE_STATE_OBSOLETE;
			gpuFractal->uploadWorkIndex = 0;
		}
	}
	if (fractal->resetHappened)
	{
		ASSERT(gpuFractal->uploadWorkIndex == 0);
		gpuFractal->imageIndex = (gpuFractal->imageIndex + 1) % ARRAY_SIZE(gpuFractal->storedHeightMaps);
		ASSERT(resourceReady(&(getHeightMap(gpuFractal)->height)));
		ASSERT(resourceReady(&(getHeightMap(gpuFractal)->normal)));
		fractal->resetHappened = false;
	}
}


inline D3D12_CPU_DESCRIPTOR_HANDLE offsetDescriptorHandle(D3D12_CPU_DESCRIPTOR_HANDLE base, u32 offset)
{
	D3D12_CPU_DESCRIPTOR_HANDLE result = base;
	result.ptr += offset;
	return result;
}

inline D3D12_GPU_DESCRIPTOR_HANDLE offsetDescriptorHandle(D3D12_GPU_DESCRIPTOR_HANDLE base, u32 offset)
{
	D3D12_GPU_DESCRIPTOR_HANDLE result = base;
	result.ptr += offset;
	return result;
}
inline D3D12_CPU_DESCRIPTOR_HANDLE getCPUDescriptorHandle(Renderer* renderer, u32 handleIndex)
{
	D3D12_CPU_DESCRIPTOR_HANDLE result = offsetDescriptorHandle(renderer->cpuDescriptorHeapBegin, handleIndex*renderer->descriptorHandleIncrementSize);
	return result;
}
inline D3D12_GPU_DESCRIPTOR_HANDLE getGPUDescriptorHandle(Renderer* renderer, u32 handleIndex)
{
	D3D12_GPU_DESCRIPTOR_HANDLE result = offsetDescriptorHandle(renderer->gpuDescriptorHeapBegin, handleIndex*renderer->descriptorHandleIncrementSize);
	return result;
}

static void setSceneBuffer(Renderer* renderer, SceneBuffer* sceneBuffer)
{
	renderer->sceneBuffer = *sceneBuffer;
}

static void drawModel(Renderer* renderer, ModelBuffer* modelBuffer, GPUMesh* mesh, GPUDescriptorBinding* binding)
{
	ASSERT(renderer->modelCount < renderer->maxModelCount);

	CommandQueue* rq = &renderer->renderQueue;

	if (!binding ||
		(resourceReady(&mesh->indexBuffer.resource, rq) && resourceReady(&mesh->vertexBuffer.resource, rq) &&
		(!binding->heightMap || (resourceReady(&binding->heightMap->height, rq) && resourceReady(&binding->heightMap->normal, rq))) && 
		(!binding->fractalMap || resourceReady(getImage(binding->fractalMap), rq)) &&
		(!binding->heightMapFractal || (resourceReady(&(getHeightMap(binding->heightMapFractal)->height), rq) && resourceReady(&(getHeightMap(binding->heightMapFractal)->normal), rq)))
		))
	{
		ID3D12GraphicsCommandList* commandList = renderer->renderCommandList.d12CommandList;

		if (binding)
		{
			if (binding->heightMap)
			{
				if (binding->heightMap->height.stateAfterModification != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
				{
					markModify(&binding->heightMap->height, &renderer->renderQueue, commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}
				if (binding->heightMap->normal.stateAfterModification != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
				{
					markModify(&binding->heightMap->normal, &renderer->renderQueue, commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}
				markUse(&binding->heightMap->height, &renderer->renderQueue);
				markUse(&binding->heightMap->normal, &renderer->renderQueue);
			}
			if (binding->fractalMap)
			{
				if (getImage(binding->fractalMap)->stateAfterModification != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
				{
					markModify(getImage(binding->fractalMap), &renderer->renderQueue, renderer->renderCommandList.d12CommandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}
				markUse(getImage(binding->fractalMap), &renderer->renderQueue);
			}
			if (binding->heightMapFractal)
			{
				GPUHeightMap* heightMap = getHeightMap(binding->heightMapFractal);
				if (heightMap->height.stateAfterModification != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
				{
					markModify(&heightMap->height, &renderer->renderQueue, commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}
				if (heightMap->normal.stateAfterModification != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
				{
					markModify(&heightMap->normal, &renderer->renderQueue, commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
				}
				markUse(&heightMap->height, &renderer->renderQueue);
				markUse(&heightMap->normal, &renderer->renderQueue);
			}
		}

		if (mesh->vertexBuffer.resource.stateAfterModification != D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
		{
			markModify(&mesh->vertexBuffer.resource, &renderer->renderQueue, commandList, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		}
		if (mesh->indexBuffer.resource.stateAfterModification != D3D12_RESOURCE_STATE_INDEX_BUFFER)
		{
				markModify(&mesh->indexBuffer.resource, &renderer->renderQueue, commandList, D3D12_RESOURCE_STATE_INDEX_BUFFER);
		}

		markUse(&mesh->indexBuffer.resource, &renderer->renderQueue);
		markUse(&mesh->vertexBuffer.resource, &renderer->renderQueue);

		if (binding)
		{
			ASSERT(!binding->heightMapFractal || !binding->heightMap); //we can't have both
		}

		ModelBuffer modelBufferToUpload = *modelBuffer;
		if (!binding || (!binding->heightMap && !binding->heightMapFractal))
		{
			modelBufferToUpload.vertexDisplacement = 0.f;
		}
		if (!binding || !binding->fractalMap)
		{
			modelBufferToUpload.fractalIndex = -1;
		}
		if (!binding || !binding->heightMapFractal)
		{
			modelBufferToUpload.heightMapFractalIndex = -1;
		}

		ModelBuffer* uploadModelBuffer = renderer->currentModelBuffers + renderer->modelCount;
		*uploadModelBuffer = modelBufferToUpload;

		DrawSetting* drawSetting = renderer->drawSettings + renderer->modelCount;

		drawSetting->vb = mesh->vertexBuffer.d12View;
		drawSetting->ib = mesh->indexBuffer.d12View;
		drawSetting->indexCount = mesh->indexCount;
		drawSetting->modelBufferIndex = renderer->modelCount;

		if (binding)
		{
			drawSetting->gpuDescriptorHandle = getGPUDescriptorHandle(renderer, binding->descriptorTableIndexInHeap);
		}
		else
		{
			drawSetting->gpuDescriptorHandle.ptr = 0;
		}

		++renderer->modelCount;
	}
}

static void submitRender(Renderer* renderer)
{
	UINT currentBackBufferIndex = renderer->swapChain->GetCurrentBackBufferIndex();
	FrameResource* frameResource = renderer->frameResources + currentBackBufferIndex;
	frameResource->commandAllocator.requiredFenceValueForReset = renderer->renderQueue.lastSignaledFenceValue + 1;

	ID3D12GraphicsCommandList* commandList = renderer->renderCommandList.d12CommandList;

	//transition backbuffer state
	{
		D3D12_RESOURCE_BARRIER barrier = transition(
			frameResource->backbuffer,
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_RENDER_TARGET
		);
		commandList->ResourceBarrier(1, &barrier);
	}

	D3D12_CPU_DESCRIPTOR_HANDLE dsv = renderer->dsvHeap->GetCPUDescriptorHandleForHeapStart();

	D3D12_VIEWPORT vp = {};
	vp.Width = (f32)renderer->backbufferWidth;
	vp.Height = (f32)renderer->backbufferHeight;
	vp.TopLeftX = 0.f;
	vp.TopLeftY = 0.f;
	vp.MinDepth = 0.f;
	vp.MaxDepth = 1.f;

	D3D12_RECT scissor = {};
	scissor.bottom = LONG_MAX;
	scissor.left = 0;
	scissor.right = LONG_MAX;
	scissor.top = 0;

	commandList->SetDescriptorHeaps(1, &renderer->descriptorHeap);

	commandList->ClearRenderTargetView(frameResource->rtv, (f32*)&renderer->clearColor, 0, 0);
	commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, 0);

	commandList->SetPipelineState(renderer->modelPipeline.pipeline);
	commandList->SetGraphicsRootSignature(renderer->modelPipeline.desc.pRootSignature);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	*renderer->currentSceneBuffer = renderer->sceneBuffer;

	//TODO: unify scene and model buffers
	commandList->ResourceBarrier(1, &transition(frameResource->gpuSceneBuffer.resource.d12Resource, 
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		D3D12_RESOURCE_STATE_COPY_DEST)
	);
	commandList->ResourceBarrier(1, &transition(frameResource->gpuModelBuffers.resource.d12Resource,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		D3D12_RESOURCE_STATE_COPY_DEST)
	);

	u32 modelBuffersOffset = renderer->modelBuffersOffset + renderer->uploadHeapSizePerFrame*currentBackBufferIndex;
	u32 sceneBufferOffset = renderer->sceneBufferOffset + renderer->uploadHeapSizePerFrame*currentBackBufferIndex;
	commandList->CopyBufferRegion(frameResource->gpuSceneBuffer.resource.d12Resource, 0, renderer->uploadHeap, sceneBufferOffset, sizeof(SceneBuffer));
	commandList->CopyBufferRegion(frameResource->gpuModelBuffers.resource.d12Resource, 0, renderer->uploadHeap, modelBuffersOffset, sizeof(ModelBuffer)*renderer->modelCount);
	
	commandList->ResourceBarrier(1, &transition(frameResource->gpuSceneBuffer.resource.d12Resource,
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
	);
	commandList->ResourceBarrier(1, &transition(frameResource->gpuModelBuffers.resource.d12Resource,
		D3D12_RESOURCE_STATE_COPY_DEST,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
	);

	commandList->RSSetViewports(1, &vp);
	commandList->RSSetScissorRects(1, &scissor);
	commandList->OMSetRenderTargets(1, &frameResource->rtv, FALSE, &dsv);
	commandList->SetGraphicsRootConstantBufferView(0, frameResource->gpuSceneBuffer.gpuVirtualAddress);

	for (u32 modelIndex = 0; modelIndex < renderer->modelCount; ++modelIndex)
	{
		DrawSetting* drawSetting = renderer->drawSettings + modelIndex;

		commandList->SetGraphicsRootConstantBufferView(2, frameResource->gpuModelBuffers.gpuVirtualAddress+ drawSetting->modelBufferIndex*sizeof(ModelBuffer));
		
		if (drawSetting->gpuDescriptorHandle.ptr)
		{
			commandList->SetGraphicsRootDescriptorTable(1, drawSetting->gpuDescriptorHandle);
		}

		commandList->IASetIndexBuffer(&drawSetting->ib);
		commandList->IASetVertexBuffers(0, 1, &drawSetting->vb);
		commandList->DrawIndexedInstanced(drawSetting->indexCount, 1, 0, 0, 0);
	}

	//transition backbuffer state
	{
		D3D12_RESOURCE_BARRIER barrier = transition(
			frameResource->backbuffer,
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PRESENT
		);
		commandList->ResourceBarrier(1, &barrier);
	}

	ASSERT(commandList->Close() == S_OK);

	ID3D12CommandList* commandListsToSubmit[] = { commandList };
	renderer->renderQueue.d12commandQueue->ExecuteCommandLists(ARRAY_SIZE(commandListsToSubmit), commandListsToSubmit);
	signalNext(&renderer->renderQueue);

	ASSERT(renderer->swapChain->Present(1, 0) == S_OK);

	renderer->modelCount = 0;

	//wait until next command allocator is available and reset command list from it
	{
		currentBackBufferIndex = renderer->swapChain->GetCurrentBackBufferIndex();
		frameResource = renderer->frameResources + currentBackBufferIndex;
		renderer->currentModelBuffers = (ModelBuffer*)(renderer->uploadHeapBegin + renderer->modelBuffersOffset + renderer->uploadHeapSizePerFrame*currentBackBufferIndex);
		renderer->currentSceneBuffer = (SceneBuffer*)(renderer->uploadHeapBegin + renderer->sceneBufferOffset + renderer->uploadHeapSizePerFrame*currentBackBufferIndex);

		//START_TIMER(WaitForRenderFence);
		waitForFenceValue(renderer->renderQueue.d12fence, frameResource->commandAllocator.requiredFenceValueForReset);
		//END_TIMER(WaitForRenderFence);
		ASSERT(frameResource->commandAllocator.d12CommandAllocator->Reset() == S_OK);
		ASSERT(commandList->Reset(frameResource->commandAllocator.d12CommandAllocator, 0) == S_OK);

	}
}

static void createRenderer(Renderer* result, HWND window, ID3D12Device2* device, u32 maxModelCount, u32 maxDescriptorCount,
	u32 backbufferWidth, u32 backbufferHeight, MemoryArena* arena)
{
	*result = {};
	result->maxModelCount = maxModelCount;
	result->drawSettings = pushArray(arena, result->maxModelCount, DrawSetting);
	result->device = device;

	result->backbufferHeight = backbufferHeight;
	result->backbufferWidth = backbufferWidth;
	result->maxDescriptorCount = maxDescriptorCount;
	result->descriptorCount = 1;

	//create command queue
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
	ASSERT(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&result->renderQueue.d12commandQueue)) == S_OK);
	ASSERT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result->renderQueue.d12fence)) == S_OK);

	//create swap chain
	IDXGIFactory4* dxgiFactory = 0;
	UINT adapterFlags = 0;
#ifdef  _DEBUG
	adapterFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
	ASSERT(CreateDXGIFactory2(adapterFlags, IID_PPV_ARGS(&dxgiFactory)) == S_OK);


	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.Width = result->backbufferWidth;
	swapChainDesc.Height = result->backbufferHeight;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.Stereo = FALSE;
	swapChainDesc.SampleDesc = { 1, 0 };
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = BACKBUFFER_COUNT;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	swapChainDesc.Flags = 0;

	IDXGISwapChain1* swapChain1 = 0;
	ASSERT(dxgiFactory->CreateSwapChainForHwnd(
		result->renderQueue.d12commandQueue,
		window,
		&swapChainDesc,
		0,
		0,
		&swapChain1
	) == S_OK);
	ASSERT(dxgiFactory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER) == S_OK);
	ASSERT(swapChain1->QueryInterface(IID_PPV_ARGS(&result->swapChain)) == S_OK);

	dxgiFactory->Release();

	result->rtvHeap = createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, BACKBUFFER_COUNT);
	result->dsvHeap = createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
	result->descriptorHeap = createDescriptorHeap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, result->maxDescriptorCount, true);
	result->descriptorHandleIncrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	result->cpuDescriptorHeapBegin = result->descriptorHeap->GetCPUDescriptorHandleForHeapStart();
	result->gpuDescriptorHeapBegin = result->descriptorHeap->GetGPUDescriptorHandleForHeapStart();

	UINT rtvDescriptorHandleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = result->rtvHeap->GetCPUDescriptorHandleForHeapStart();

	ASSERT(BACKBUFFER_COUNT == ARRAY_SIZE(result->frameResources));
	for (UINT backbufferIndex = 0; backbufferIndex < BACKBUFFER_COUNT; ++backbufferIndex)
	{
		FrameResource* frameResource = result->frameResources + backbufferIndex;

		ASSERT(result->swapChain->GetBuffer(backbufferIndex, IID_PPV_ARGS(&frameResource->backbuffer)) == S_OK);
		device->CreateRenderTargetView(frameResource->backbuffer, 0, rtv);
		frameResource->rtv = rtv;

		ASSERT(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameResource->commandAllocator.d12CommandAllocator)) == S_OK);

		frameResource->gpuSceneBuffer = createConstantBuffer(device, sizeof(SceneBuffer), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		frameResource->gpuModelBuffers = createConstantBuffer(device, sizeof(ModelBuffer)*result->maxModelCount, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

		rtv.ptr += rtvDescriptorHandleSize;

	}

	D3D12_CLEAR_VALUE clearValue = {};
	clearValue.Format = DXGI_FORMAT_D32_FLOAT;
	clearValue.DepthStencil = { 1.f, 0 };
	ASSERT(device->CreateCommittedResource(
		&createHeapProperties(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&createResourceDescTex2D(DXGI_FORMAT_D32_FLOAT, result->backbufferWidth, result->backbufferHeight, 0, 
			D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&clearValue,
		IID_PPV_ARGS(&result->depthBuffer)
	) == S_OK);

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Texture2D.MipSlice = 0;
	device->CreateDepthStencilView(result->depthBuffer, &dsvDesc, result->dsvHeap->GetCPUDescriptorHandleForHeapStart());

	//create command list
	u32 currentBackBufferIndex = result->swapChain->GetCurrentBackBufferIndex();
	ASSERT(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, 
		result->frameResources[currentBackBufferIndex].commandAllocator.d12CommandAllocator, 0,
		IID_PPV_ARGS(&result->renderCommandList.d12CommandList)) == S_OK);


	//create upload heap
	result->sceneBufferOffset = 0;
	result->modelBuffersOffset = sizeof(SceneBuffer);
	result->uploadHeapSizePerFrame = result->modelBuffersOffset + maxModelCount * sizeof(ModelBuffer);
	ASSERT(device->CreateCommittedResource(
		&createHeapProperties(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&createResourceDescBuffer(result->uploadHeapSizePerFrame*BACKBUFFER_COUNT, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		0,
		IID_PPV_ARGS(&result->uploadHeap)
	) == S_OK);

	ASSERT(result->uploadHeap->Map(0, 0, (void**)&result->uploadHeapBegin) == S_OK);

	result->currentModelBuffers = (ModelBuffer*)(result->uploadHeapBegin + result->modelBuffersOffset + result->uploadHeapSizePerFrame*currentBackBufferIndex);
	result->currentSceneBuffer = (SceneBuffer*)(result->uploadHeapBegin + result->sceneBufferOffset + result->uploadHeapSizePerFrame*currentBackBufferIndex);

	createHeightMappingPipeline(&result->modelPipeline, device);
}

static void allocateDescriptors(Renderer* renderer, GPUDescriptorBinding* binding)
{
	ASSERT(binding->descriptorTableIndexInHeap == 0); //not allocated already
	ASSERT(!binding->heightMap || !binding->heightMapFractal); //cannot have both

	u32 requiredDescriptorCount = 0;
	if (binding->heightMap) requiredDescriptorCount = 4;
	if (binding->heightMapFractal) requiredDescriptorCount = 4;
	if (binding->fractalMap) requiredDescriptorCount = 6;
	ASSERT(requiredDescriptorCount > 0);

	ASSERT(renderer->descriptorCount + requiredDescriptorCount <= renderer->maxDescriptorCount);
	binding->descriptorTableIndexInHeap = renderer->descriptorCount;
	renderer->descriptorCount += requiredDescriptorCount;

	if (binding->heightMap)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE normalHandle = getCPUDescriptorHandle(renderer, binding->descriptorTableIndexInHeap);
		D3D12_CPU_DESCRIPTOR_HANDLE heightHandle = getCPUDescriptorHandle(renderer, binding->descriptorTableIndexInHeap + 2);
	
		renderer->device->CreateShaderResourceView(binding->heightMap->normal.d12Resource, 0, normalHandle);
		renderer->device->CreateShaderResourceView(binding->heightMap->height.d12Resource, 0, heightHandle);
	}
	if (binding->heightMapFractal)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE normalHandle0 = getCPUDescriptorHandle(renderer, binding->descriptorTableIndexInHeap);
		D3D12_CPU_DESCRIPTOR_HANDLE normalHandle1 = getCPUDescriptorHandle(renderer, binding->descriptorTableIndexInHeap+1);
		
		D3D12_CPU_DESCRIPTOR_HANDLE heightHandle0 = getCPUDescriptorHandle(renderer, binding->descriptorTableIndexInHeap + 2);
		D3D12_CPU_DESCRIPTOR_HANDLE heightHandle1 = getCPUDescriptorHandle(renderer, binding->descriptorTableIndexInHeap + 3);

		renderer->device->CreateShaderResourceView(binding->heightMapFractal->storedHeightMaps[0].normal.d12Resource, 0, normalHandle0);
		renderer->device->CreateShaderResourceView(binding->heightMapFractal->storedHeightMaps[1].normal.d12Resource, 0, normalHandle1);
		
		renderer->device->CreateShaderResourceView(binding->heightMapFractal->storedHeightMaps[0].height.d12Resource, 0, heightHandle0);
		renderer->device->CreateShaderResourceView(binding->heightMapFractal->storedHeightMaps[1].height.d12Resource, 0, heightHandle1);
	}
	if (binding->fractalMap)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE imageHandle0 = getCPUDescriptorHandle(renderer, binding->descriptorTableIndexInHeap + 4);
		D3D12_CPU_DESCRIPTOR_HANDLE imageHandle1 = getCPUDescriptorHandle(renderer, binding->descriptorTableIndexInHeap + 5);

		renderer->device->CreateShaderResourceView(binding->fractalMap->storageImages[0].d12Resource, 0, imageHandle0);
		renderer->device->CreateShaderResourceView(binding->fractalMap->storageImages[1].d12Resource, 0, imageHandle1);
	}

}

static void queueTestJob(void* data)
{
	f64 n = (f64)(umm)data;
	f64 res = 1;
	for (u32 i = 1; i <= n; ++i)
	{
		res *= (f64)i;
	}
	char buff[512];
	sprintf_s(buff, ARRAY_SIZE(buff), "thread %d: %f!=%f\n", GetCurrentThreadId(), n, res);
	OutputDebugStringA(buff);
}




int CALLBACK WinMain(
	HINSTANCE hInstance,
	HINSTANCE hPrevInstance,
	LPSTR     lpCmdLine,
	int       nShowCmd
)
{
	WorkQueue hotQueue;
	WorkQueue coldQueue;
	initWorkQueue(&hotQueue, 7);
	initWorkQueue(&coldQueue, 4);


	ASSERT(QueryPerformanceFrequency(&g_perfCounterFrequency) == TRUE);
	umm storageSize = 1024 * 1024 * 1024;
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
	WINDOWPLACEMENT windowPlacement = { sizeof(windowPlacement) };

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

	ResourceManager resourceManager = createResourceManager(device);
	Renderer renderer = {};
	createRenderer(&renderer, window, device, 1024, 128, 1920, 1080, &arena);

	//GraphicsPipeline terrainPipeline = createTerrainPipeline(device);
	//GraphicsPipeline fractalPipeline = createFractalPipeline(device);


	//Terrain terrain = createTerrain(&resourceManager, &arena, 128, 5.f, 40.f, 64);

	//create srv for textures
	//D3D12_CPU_DESCRIPTOR_HANDLE cpuTexView = srvHeap->GetCPUDescriptorHandleForHeapStart();
	//device->CreateShaderResourceView(terrain.normalMap.d12Resource, 0, cpuTexView);
	//cpuTexView.ptr += srvHeapIncrementSize;
	//device->CreateShaderResourceView(terrain.heightMap.d12Resource, 0, cpuTexView);
	//cpuTexView.ptr += srvHeapIncrementSize;
	//device->CreateShaderResourceView(fractal.tex.d12Resource, 0, cpuTexView);
	//cpuTexView.ptr += srvHeapIncrementSize;
	//device->CreateShaderResourceView(sphere.normalMap.d12Resource, 0, cpuTexView);
	//cpuTexView.ptr += srvHeapIncrementSize;
	//device->CreateShaderResourceView(sphere.heightMap.d12Resource, 0, cpuTexView);
	//cpuTexView.ptr += srvHeapIncrementSize;


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

	LightBuffer lights[4] = {};
	lights[0].intensity = { 5.f, 0.5f, 0.5f };
	lights[1].intensity = { 0.5f, 5.f, 0.5f };
	lights[2].intensity = { 0.5f, 0.5f, 5.f };

	lights[3].pos = { 20.f, 20.f, 20.f };
	lights[3].intensity = { 300.f, 300.f, 300.f };
	

	ModelBuffer lightModelBuffers[4];
	for (u32 lightModelBufferIndex = 0; lightModelBufferIndex < ARRAY_SIZE(lightModelBuffers); ++lightModelBufferIndex)
	{
		ModelBuffer* lightModelBuffer = lightModelBuffers + lightModelBufferIndex;
		LightBuffer* light = lights + lightModelBufferIndex;

		lightModelBuffer->model = translation(light->pos);
		lightModelBuffer->scale = 0.02f;
		lightModelBuffer->color = V4(light->intensity, 1.f);
		lightModelBuffer->emissionScale = 1.f;
	}
	lightModelBuffers[3].scale = 0.5f;

	Fractal fractal;
	createFractal(&arena, &fractal, 0.1f, 354434, 4096, 4096, 256);
	GPUFractal gpuFractal = createGPUFractal(&resourceManager, &fractal);

	ColoredFractal coloredFractal;
	createColoredFractal(&arena, &coloredFractal, 0.5f, 121, 1024, 1024, 128);
	GPUFractal gpuColoredFractal = createGPUFractal(&resourceManager, &coloredFractal);

	HeightMapFractal heightMapFractal;
	createHeightMapFractal(&arena, &heightMapFractal, 0.1f, 6784, 4096, 4096, 512);
	GPUHeightMapFractal gpuHeightMapFractal = createGPUHeightMapFractal(&resourceManager, &heightMapFractal);

	TempMemory tempMem = startTempMemory(&arena);
	Mesh meshes[4] =
	{
		createSphereMesh(&arena, 256, 256),
		createTorusMesh(&arena, 256, 256, 1.f),
		createCubeMesh(&arena),
		createPlaneMesh(&arena, {200.f, 200.f}, 1, 1)
	};
	GPUMesh gpuMeshes[4] =
	{
		createGPUMesh(&resourceManager, meshes + 0),
		createGPUMesh(&resourceManager, meshes + 1),
		createGPUMesh(&resourceManager, meshes + 2),
		createGPUMesh(&resourceManager, meshes + 3),
	};
	Mesh lightMesh = createSphereMesh(&arena, 16, 16);
	GPUMesh gpuLightMesh = createGPUMesh(&resourceManager, &lightMesh);

	Mesh skyMesh = createSphereMesh(&arena, 32, 32);
	reverseOrientation(&skyMesh);
	GPUMesh gpuSkyMesh = createGPUMesh(&resourceManager, &skyMesh);

	endTempMemory(&tempMem);


	tempMem = startTempMemory(&arena);
	HeightMap heightMaps[2] =
	{
		createHeightMapForSphere(&arena, 4096, 4096, 13, 0.4f),
		createHeightMapForTorus(&arena, 4096, 4096, 789, 0.7f),
	};
	GPUHeightMap gpuHeightMaps[2] =
	{
		createGPUHeightMap(&resourceManager, heightMaps + 0),
		createGPUHeightMap(&resourceManager, heightMaps + 1),
	};
	endTempMemory(&tempMem);

	GPUDescriptorBinding gpuBindings[5] = {};
	gpuBindings[0].heightMap = gpuHeightMaps + 0;
	gpuBindings[1].heightMap = gpuHeightMaps + 1;
	gpuBindings[2].fractalMap = &gpuColoredFractal;
	gpuBindings[3].fractalMap = &gpuFractal;
	gpuBindings[4].heightMapFractal = &gpuHeightMapFractal;

	for (u32 bindingIndex = 0; bindingIndex < ARRAY_SIZE(gpuBindings); ++bindingIndex)
	{
		allocateDescriptors(&renderer, gpuBindings + bindingIndex);
	}

	f32 angle = 0.f;
	f32 angleVelocity = pi32 / 2.f;

	Input input = {};

	m4 proj = projection((f32)renderer.backbufferWidth / (f32)renderer.backbufferHeight, pi32 * 0.5f, 0.1f, 500.f);

	f32 walkSpeed = 8.f;
	f32 elevationSpeed = 8.f;
	f32 rotationSpeed = pi32 * 0.25f;
	v3 cameraPos = {};
	v2 cameraRot = {};
	f32 maxCameraRotXAbs = pi32 * 0.5f * 0.9f;

	f32 dt = 0;
	while (g_running)
	{
		updateInput(&input);
		Win32ProcessPendingMessages(&input);

		if (wasPressed(&input.tab))
		{
			Win32ToggleFullScreen(window, &windowPlacement);
		}

		if (shouldRebuildGraphicspipeline(&input, &renderer.modelPipeline))
		{
			flushCommandQueue(&renderer.renderQueue);
			rebuildGraphicsPipeline(device, &renderer.modelPipeline);
		}

		updateFractal(&coldQueue, &fractal, dt);
		updateFractal(&hotQueue, &coloredFractal, dt);
		updateFractal(&coldQueue, &heightMapFractal, dt);

		updateGPUFractal(&resourceManager, &renderer, &fractal, &gpuFractal);
		updateGPUFractal(&resourceManager, &renderer, &coloredFractal, &gpuColoredFractal);
		updateGPUFractal(&resourceManager, &renderer, &heightMapFractal, &gpuHeightMapFractal);

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
			m4 models[2] =
			{
				rotationY(angle*0.125f)*rotationX(angle*0.125f),
				identityM4()
			};

			for (s32 modelIndexZ = 0; modelIndexZ < modelCountZ; ++modelIndexZ)
			{
				for (s32 modelIndexY = 0; modelIndexY < modelCountY; ++modelIndexY)
				{
					for (s32 modelIndexX = 0; modelIndexX < modelCountX; ++modelIndexX)
					{
						ModelBuffer* modelBuffer = &modelBuffers[modelIndexZ][modelIndexY][modelIndexX];
						modelBuffer->model = translation(V3(modelIndexX*5, modelIndexY*5+10, modelIndexZ*5)) * 
							models[MIN(modelIndexX/5, 1)];
						modelBuffer->scale = 1.f;
						modelBuffer->vertexDisplacement  = 0.5f*(1.f + sinf(angle*0.3f));
						
						modelBuffer->fractalZoomScale = coloredFractal.zoomFactor;
						modelBuffer->fractalIndex = gpuColoredFractal.imageIndex;

						modelBuffer->heightMapFractalZoomScale = heightMapFractal.height.zoomFactor;
						modelBuffer->heightMapFractalIndex = gpuHeightMapFractal.imageIndex;

					}
				}
			}
		}

		lights[0].pos = (cameraModel*v4{ 0.f, 0.25f, -0.5f, 1.f }).xyz;
		lights[1].pos = (cameraModel*v4{ -0.45f, 0.f, -0.5f, 1.f }).xyz;
		lights[2].pos = (cameraModel*v4{ 0.45f, 0.f, -0.5f, 1.f }).xyz;

		SceneBuffer sceneBuffer = {};
		m4 view = invertOrtho3Translation(cameraModel);
		m4 projview = proj * view;
		sceneBuffer.projview = projview;
		sceneBuffer.viewPos = cameraPos;

		sceneBuffer.lightCount = ARRAY_SIZE(lights);
		for (u32 lightIndex = 0; lightIndex < sceneBuffer.lightCount; ++lightIndex)
		{
			sceneBuffer.lights[lightIndex] = lights[lightIndex];
		}

		renderer.sceneBuffer = sceneBuffer;
		renderer.clearColor = { 0.4f, 0.6f, 0.9f, 1.0f };

		//for (u32 modelIndexY = 0; modelIndexY < modelCountY; ++modelIndexY)
		{
			u32 modelIndexY = 0;
			for (u32 modelIndexX = 0; modelIndexX < modelCountX; ++modelIndexX)
			{
				u32 modelIndex = (modelIndexX + modelIndexY) % 3;

				if (modelIndexX == 9)
				{
					ModelBuffer* mb = &modelBuffers[0][modelIndexY][modelIndexX];
					mb->vertexDisplacement = 0.7f;
					//mb->roughness += 0.1f;
					//mb->metalness = 0.7f;
					drawModel
					(
						&renderer,
						&modelBuffers[0][modelIndexY][modelIndexX],
						gpuMeshes + 0,
						gpuBindings + 4
					);
				}
				else
				{
					drawModel
					(
						&renderer,
						&modelBuffers[0][modelIndexY][modelIndexX],
						gpuMeshes + modelIndex,
						gpuBindings + modelIndex
					);
				}
			}
		}

		ModelBuffer planeBuffer = {};
		planeBuffer.model = translation({ 50.f, 0.f, -20.f })*rotationY(pi32)*rotationX(-pi32 * 0.5f);
		planeBuffer.fractalZoomScale = fractal.zoomFactor;
		planeBuffer.fractalIndex = gpuFractal.imageIndex;
		planeBuffer.scale = 1.f;
		planeBuffer.color = renderer.clearColor;

		//drawModel
		//(
		//	&renderer,
		//	&planeBuffer,
		//	gpuMeshes + 3,
		//	gpuBindings + 2
		//);

		//draw lights
		{
			for (u32 lightIndex = 0; lightIndex < ARRAY_SIZE(lightModelBuffers); ++lightIndex)
			{
				LightBuffer* light = lights + lightIndex;
				ModelBuffer* lightModelBuffer = lightModelBuffers + lightIndex;
				lightModelBuffer->model = translation(light->pos);

				drawModel(&renderer, lightModelBuffer, &gpuLightMesh, 0);
			}
		}

		//draw sky
		{
			ModelBuffer skyModel = {};
			skyModel.model = translation(cameraPos);
			skyModel.scale = 100.f;
			skyModel.fractalZoomScale = fractal.zoomFactor;
			skyModel.fractalIndex = gpuFractal.imageIndex;
			skyModel.color = renderer.clearColor;
			skyModel.emissionScale = 1.f;

			drawModel
			(
				&renderer,
				&skyModel,
				&gpuSkyMesh,
				gpuBindings + 3
			);
		}

		submitRender(&renderer);
		
		updateDebugInfo();

		LARGE_INTEGER frameEnd = Win32GetWallClock();
		dt = Win32GetSecondsElapsed(frameStart, frameEnd);
		frameStart = frameEnd;
		if (dt > 0.02f)
		{
			char buffer[256];
			sprintf_s(buffer, "Frame time: %fms\n", dt*1000.f);
			OutputDebugStringA(buffer);
		}
		dt = MIN(0.033f, dt);

	}

	return 0;
}