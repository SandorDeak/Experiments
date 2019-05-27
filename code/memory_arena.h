#include "platform.h"


#define pushStruct(arena, type) (type*)pushSize(arena, sizeof(type), alignof(type)) 
#define pushArray(arena, count, type) (type*)pushSize(arena, sizeof(type) * (count), alignof(type)) 

struct MemoryArena
{
	u8* base;
	umm offset;
	umm size;
	u32 tempMemoryCount;
};

struct TempMemory
{
	MemoryArena* arena;
	umm offset;
};

static TempMemory startTempMemory(MemoryArena* arena)
{
	TempMemory result = {};
	result.arena = arena;
	result.offset = arena->offset;
	++arena->tempMemoryCount;
	return result;
}

static void endTempMemory(TempMemory* tempMemory)
{
	MemoryArena* arena = tempMemory->arena;
	ASSERT(arena->offset >= tempMemory->offset);
	arena->offset = tempMemory->offset;
	ASSERT(arena->tempMemoryCount);
	--arena->tempMemoryCount;
}

static MemoryArena createMemoryArena(void* base, umm size)
{
	MemoryArena result = {};
	result.base = (u8*)base;
	result.size = size;
	return result;
}

static void validateArena(MemoryArena* arena)
{
	ASSERT(arena->tempMemoryCount == 0);
}

static void* pushSize(MemoryArena* arena, umm size, umm alignment = 4)
{
	u8* result = 0;

	u8* at = arena->base + arena->offset;
	ASSERT(IS_POW2(alignment));
	u8* aligned = ALIGN_PTR(at, alignment);
	umm alignedSize = (aligned - at) + size;
	if (arena->offset + alignedSize <= arena->size)
	{
		arena->offset += alignedSize;
		result = aligned;
	}

	return result;
}

static umm getOffset(MemoryArena* arena, void* ptr)
{
	umm result = (u8*)ptr - arena->base;
	ASSERT(result < arena->size);
	return result;
}

static void* pushAlignment(MemoryArena* arena, umm alignment)
{
	return pushSize(arena, 0, alignment);
}
