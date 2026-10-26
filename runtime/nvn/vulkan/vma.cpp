// Vulkan Memory Allocator implementation (header-only library, MSYS2 package vulkan-memory-allocator).
// VMA_VULKAN_VERSION 1.2: the Vulkan loader that older drivers install (System32\vulkan-1.dll, loaded before any other
// copy) has no 1.3 entry points, and statically imported ones stop the program from starting (0xC0000139).
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_VULKAN_VERSION 1002000
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif
