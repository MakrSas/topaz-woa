/*
 * Allocators the WDI TLV generator/parser library (WificxTLVGenParse.lib) expects from the driver
 * (WDK wificx sample memorymanagement.cpp). Our TLV_CONTEXT.AllocationContext is always 0, so the
 * placement variant only exists for completeness. Non-paged, zeroed pool.
 */
#include "wpch.h"

typedef struct _PLACEMENT_NEW_ALLOCATION_CONTEXT
{
    size_t cbMaxSize;
    void  *pbBuffer;
} PLACEMENT_NEW_ALLOCATION_CONTEXT;

static void *PoolAlloc(size_t Size)
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size ? Size : 1, TOPAZ_WIFI_TAG);   /* zeroed */
}

static void PoolFree(void *P)
{
    if (P != nullptr) {
        ExFreePoolWithTag(P, TOPAZ_WIFI_TAG);
    }
}

void *__cdecl operator new(size_t Size) noexcept
{
    return PoolAlloc(Size);
}

void *__cdecl operator new[](size_t Size) noexcept
{
    return PoolAlloc(Size);
}

void *__cdecl operator new(size_t Size, ULONG_PTR AllocationContext) noexcept   /* WIFICX TLV */
{
    if (AllocationContext != 0) {
        auto *c = reinterpret_cast<PLACEMENT_NEW_ALLOCATION_CONTEXT *>(AllocationContext);
        if (Size > c->cbMaxSize) {
            return nullptr;
        }
        RtlZeroMemory(c->pbBuffer, Size);
        return c->pbBuffer;
    }
    return PoolAlloc(Size);
}

void *__cdecl operator new[](size_t Size, ULONG_PTR AllocationContext) noexcept
{
    return operator new(Size, AllocationContext);
}

void __cdecl operator delete(void *P) noexcept
{
    PoolFree(P);
}

/* also the TLV library's delete(void *, ULONG_PTR): ULONG_PTR is size_t on ARM64 */
void __cdecl operator delete(void *P, size_t) noexcept
{
    PoolFree(P);
}

void __cdecl operator delete[](void *P) noexcept
{
    PoolFree(P);
}

void __cdecl operator delete[](void *P, size_t) noexcept
{
    PoolFree(P);
}
