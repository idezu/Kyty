#include "Emulator/Kernel/Memory.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Graphics/GpuMemory.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/VirtualMemory.h"

#include <algorithm>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::LibKernel::Memory {

namespace VirtualMemory = Loader::VirtualMemory;

LIB_NAME("libkernel", "libkernel");

class PhysicalMemory
{
public:
	struct AllocatedBlock
	{
		uint64_t                start_addr;
		uint64_t                size;
		uint64_t                map_vaddr;
		uint64_t                map_size;
		int                     prot;
		VirtualMemory::Mode     mode;
		Graphics::GpuMemoryMode gpu_mode;
	};

	PhysicalMemory() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~PhysicalMemory() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(PhysicalMemory);

	static uint64_t Size() { return static_cast<uint64_t>(5376) * 1024 * 1024; }

	bool Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment, uint64_t* phys_addr_out);
	bool Release(uint64_t start, size_t len, uint64_t* vaddr, uint64_t* size, Graphics::GpuMemoryMode* gpu_mode);
	bool Map(uint64_t vaddr, uint64_t phys_addr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode);
	bool Unmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode);

private:
	Vector<AllocatedBlock> m_allocated;
	Core::Mutex            m_mutex;
};

class FlexibleMemory
{
public:
	struct AllocatedBlock
	{
		uint64_t                map_vaddr;
		uint64_t                map_size;
		int                     prot;
		VirtualMemory::Mode     mode;
		Graphics::GpuMemoryMode gpu_mode;
	};

	FlexibleMemory() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~FlexibleMemory() { KYTY_NOT_IMPLEMENTED; }

	KYTY_CLASS_NO_COPY(FlexibleMemory);

	bool Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode);
	bool Unmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode);
	bool Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode, Graphics::GpuMemoryMode* gpu_mode);

private:
	Vector<AllocatedBlock> m_allocated;
	Core::Mutex            m_mutex;
};

static PhysicalMemory* g_physical_memory = nullptr;
static FlexibleMemory* g_flexible_memory = nullptr;

KYTY_SUBSYSTEM_INIT(Memory)
{
	g_physical_memory = new PhysicalMemory;
	g_flexible_memory = new FlexibleMemory;
}

KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Memory) {}

KYTY_SUBSYSTEM_DESTROY(Memory) {}

static uint64_t get_aligned_pos(uint64_t pos, size_t align)
{
	return (align != 0 ? (pos + (align - 1)) & ~(align - 1) : pos);
}

bool PhysicalMemory::Alloc(uint64_t search_start, uint64_t search_end, size_t len, size_t alignment, uint64_t* phys_addr_out)
{
	if (phys_addr_out == nullptr)
	{
		return false;
	}

	Core::LockGuard lock(m_mutex);

	uint64_t free_pos = 0;

	for (const auto& b: m_allocated)
	{
		uint64_t n = b.start_addr + b.size;
		if (n > free_pos)
		{
			free_pos = n;
		}
	}

	free_pos = get_aligned_pos(free_pos, alignment);

	if (free_pos >= search_start && free_pos + len <= search_end)
	{
		AllocatedBlock b {};
		b.size       = len;
		b.start_addr = free_pos;
		b.gpu_mode   = Graphics::GpuMemoryMode::NoAccess;
		b.map_size   = 0;
		b.map_vaddr  = 0;
		b.prot       = 0;
		b.mode       = VirtualMemory::Mode::NoAccess;

		m_allocated.Add(b);

		*phys_addr_out = free_pos;
		return true;
	}

	return false;
}

bool PhysicalMemory::Release(uint64_t start, size_t len, uint64_t* vaddr, uint64_t* size, Graphics::GpuMemoryMode* gpu_mode)
{
	EXIT_IF(vaddr == nullptr);
	EXIT_IF(size == nullptr);
	EXIT_IF(gpu_mode == nullptr);

	Core::LockGuard lock(m_mutex);

	uint32_t index = 0;
	for (auto& b: m_allocated)
	{
		if (start == b.start_addr && len == b.size)
		{
			*vaddr    = b.map_vaddr;
			*size     = b.map_size;
			*gpu_mode = b.gpu_mode;

			m_allocated.RemoveAt(index);
			return true;
		}
		index++;
	}

	return false;
}

bool PhysicalMemory::Map(uint64_t vaddr, uint64_t phys_addr, size_t len, int prot, VirtualMemory::Mode mode,
                         Graphics::GpuMemoryMode gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	for (auto& b: m_allocated)
	{
		if (phys_addr >= b.start_addr && phys_addr < b.start_addr + b.size)
		{
			if (b.map_vaddr != 0 || b.map_size != 0)
			{
				return false;
			}

			b.map_vaddr = vaddr;
			b.map_size  = len;
			b.prot      = prot;
			b.mode      = mode;
			b.gpu_mode  = gpu_mode;

			return true;
		}
	}

	return false;
}

bool PhysicalMemory::Unmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode)
{
	EXIT_IF(gpu_mode == nullptr);

	Core::LockGuard lock(m_mutex);

	for (auto& b: m_allocated)
	{
		if (b.map_vaddr == vaddr && b.map_size == size)
		{
			*gpu_mode = b.gpu_mode;

			b.gpu_mode  = Graphics::GpuMemoryMode::NoAccess;
			b.map_size  = 0;
			b.map_vaddr = 0;
			b.prot      = 0;
			b.mode      = VirtualMemory::Mode::NoAccess;

			return true;
		}
	}

	return false;
}

bool PhysicalMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
                          Graphics::GpuMemoryMode* gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	return std::any_of(m_allocated.begin(), m_allocated.end(),
	                   [vaddr, base_addr, len, prot, mode, gpu_mode](auto& b)
	                   {
		                   if (vaddr >= b.map_vaddr && vaddr < b.map_vaddr + b.map_size)
		                   {
			                   if (base_addr != nullptr)
			                   {
				                   *base_addr = b.map_vaddr;
			                   }
			                   if (len != nullptr)
			                   {
				                   *len = b.map_size;
			                   }
			                   if (prot != nullptr)
			                   {
				                   *prot = b.prot;
			                   }
			                   if (mode != nullptr)
			                   {
				                   *mode = b.mode;
			                   }
			                   if (gpu_mode != nullptr)
			                   {
				                   *gpu_mode = b.gpu_mode;
			                   }

			                   return true;
		                   }
		                   return false;
	                   });
}

bool FlexibleMemory::Map(uint64_t vaddr, size_t len, int prot, VirtualMemory::Mode mode, Graphics::GpuMemoryMode gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	AllocatedBlock b {};
	b.map_vaddr = vaddr;
	b.map_size  = len;
	b.prot      = prot;
	b.mode      = mode;
	b.gpu_mode  = gpu_mode;

	m_allocated.Add(b);

	return true;
}

bool FlexibleMemory::Unmap(uint64_t vaddr, uint64_t size, Graphics::GpuMemoryMode* gpu_mode)
{
	EXIT_IF(gpu_mode == nullptr);

	Core::LockGuard lock(m_mutex);

	uint32_t index = 0;
	for (auto& b: m_allocated)
	{
		if (b.map_vaddr == vaddr && b.map_size == size)
		{
			*gpu_mode = b.gpu_mode;

			m_allocated.RemoveAt(index);
			return true;
		}
		index++;
	}

	return false;
}

bool FlexibleMemory::Find(uint64_t vaddr, uint64_t* base_addr, size_t* len, int* prot, VirtualMemory::Mode* mode,
                          Graphics::GpuMemoryMode* gpu_mode)
{
	Core::LockGuard lock(m_mutex);

	return std::any_of(m_allocated.begin(), m_allocated.end(),
	                   [vaddr, base_addr, len, prot, mode, gpu_mode](auto& b)
	                   {
		                   if (vaddr >= b.map_vaddr && vaddr < b.map_vaddr + b.map_size)
		                   {
			                   if (base_addr != nullptr)
			                   {
				                   *base_addr = b.map_vaddr;
			                   }
			                   if (len != nullptr)
			                   {
				                   *len = b.map_size;
			                   }
			                   if (prot != nullptr)
			                   {
				                   *prot = b.prot;
			                   }
			                   if (mode != nullptr)
			                   {
				                   *mode = b.mode;
			                   }
			                   if (gpu_mode != nullptr)
			                   {
				                   *gpu_mode = b.gpu_mode;
			                   }

			                   return true;
		                   }
		                   return false;
	                   });
}

int32_t KYTY_SYSV_ABI KernelMapNamedFlexibleMemory(void** addr_in_out, size_t len, int prot, int flags, const char* name)
{
	PRINT_NAME();

	EXIT_IF(g_flexible_memory == nullptr);

	EXIT_NOT_IMPLEMENTED(addr_in_out == nullptr);
	EXIT_NOT_IMPLEMENTED(flags != 0);

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	switch (prot)
	{
		case 0: mode = VirtualMemory::Mode::NoAccess; break;
		case 1: mode = VirtualMemory::Mode::Read; break;
		case 2:
		case 3: mode = VirtualMemory::Mode::ReadWrite; break;
		case 4: mode = VirtualMemory::Mode::Execute; break;
		case 5: mode = VirtualMemory::Mode::ExecuteRead; break;
		case 6:
		case 7: mode = VirtualMemory::Mode::ExecuteReadWrite; break;
		default: EXIT("unknown prot: %d\n", prot);
	}

	auto in_addr  = reinterpret_cast<uint64_t>(*addr_in_out);
	auto out_addr = VirtualMemory::Alloc(in_addr, len, mode);
	*addr_in_out  = reinterpret_cast<void*>(out_addr);

	if (!g_flexible_memory->Map(out_addr, len, prot, mode, gpu_mode))
	{
		printf(FG_RED "\t[Fail]\n" FG_DEFAULT);
		VirtualMemory::Free(out_addr);
		return KERNEL_ERROR_ENOMEM;
	}

	printf("\tin_addr  = 0x%016" PRIx64 "\n", in_addr);
	printf("\tout_addr = 0x%016" PRIx64 "\n", out_addr);
	printf("\tsize     = %" PRIu64 "\n", len);
	printf("\tmode     = %s\n", Core::EnumName(mode).C_Str());
	printf("\tname     = %s\n", name);

	if (out_addr == 0)
	{
		return KERNEL_ERROR_ENOMEM;
	}

	return OK;
}

int KYTY_SYSV_ABI KernelMunmap(uint64_t vaddr, size_t len)
{
	PRINT_NAME();

	printf("\t start = 0x%016" PRIx64 "\n", vaddr);
	printf("\t len   = 0x%016" PRIx64 "\n", len);

	EXIT_IF(g_physical_memory == nullptr);
	EXIT_IF(g_flexible_memory == nullptr);

	if (vaddr < 0 || len == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	bool result = g_physical_memory->Unmap(vaddr, len, &gpu_mode);

	if (!result)
	{
		result = g_flexible_memory->Unmap(vaddr, len, &gpu_mode);
	}

	EXIT_NOT_IMPLEMENTED(!result);

	if (vaddr != 0 || len != 0)
	{
		VirtualMemory::Free(vaddr);
	}

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		Graphics::GraphicsRunWait();
		Graphics::GpuMemoryFree(Graphics::WindowGetGraphicContext(), vaddr, len);
	}

	return OK;
}

size_t KYTY_SYSV_ABI KernelGetDirectMemorySize()
{
	PRINT_NAME();

	return PhysicalMemory::Size();
}

int KYTY_SYSV_ABI KernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len, size_t alignment, int memory_type,
                                             int64_t* phys_addr_out)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	printf("\t search_start = 0x%016" PRIx64 "\n", search_start);
	printf("\t search_end   = 0x%016" PRIx64 "\n", search_end);
	printf("\t len          = 0x%016" PRIx64 "\n", len);
	printf("\t alignment    = 0x%016" PRIx64 "\n", alignment);
	printf("\t memory_type  = %d\n", memory_type);

	if (search_start < 0 || search_end <= search_start || len == 0 || phys_addr_out == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t addr = 0;
	if (!g_physical_memory->Alloc(search_start, search_end, len, alignment, &addr))
	{
		printf(FG_RED "\t[Fail]\n" FG_DEFAULT);
		return KERNEL_ERROR_EAGAIN;
	}

	*phys_addr_out = static_cast<int64_t>(addr);

	printf("\tphys_addr    = %016" PRIx64 "\n", addr);
	printf(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

int KYTY_SYSV_ABI KernelReleaseDirectMemory(int64_t start, size_t len)
{
	PRINT_NAME();

	printf("\t start = 0x%016" PRIx64 "\n", start);
	printf("\t len   = 0x%016" PRIx64 "\n", len);

	EXIT_IF(g_physical_memory == nullptr);

	if (start < 0 || len == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	uint64_t                vaddr    = 0;
	uint64_t                size     = 0;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	bool result = g_physical_memory->Release(start, len, &vaddr, &size, &gpu_mode);

	EXIT_NOT_IMPLEMENTED(!result);

	if (vaddr != 0 || size != 0)
	{
		VirtualMemory::Free(vaddr);
	}

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		Graphics::GraphicsRunWait();
		Graphics::GpuMemoryFree(Graphics::WindowGetGraphicContext(), vaddr, size);
	}

	return OK;
}

int KYTY_SYSV_ABI KernelMapDirectMemory(void** addr, size_t len, int prot, int flags, int64_t direct_memory_start, size_t alignment)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);

	// EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());

	EXIT_NOT_IMPLEMENTED(addr == nullptr);
	EXIT_NOT_IMPLEMENTED(flags != 0);

	VirtualMemory::Mode     mode     = VirtualMemory::Mode::NoAccess;
	Graphics::GpuMemoryMode gpu_mode = Graphics::GpuMemoryMode::NoAccess;

	switch (prot)
	{
		case 0x00: mode = VirtualMemory::Mode::NoAccess; break;
		case 0x01: mode = VirtualMemory::Mode::Read; break;
		case 0x02:
		case 0x03: mode = VirtualMemory::Mode::ReadWrite; break;
		case 0x04: mode = VirtualMemory::Mode::Execute; break;
		case 0x05: mode = VirtualMemory::Mode::ExecuteRead; break;
		case 0x06:
		case 0x07: mode = VirtualMemory::Mode::ExecuteReadWrite; break;
		case 0x32:
		case 0x33:
			mode     = VirtualMemory::Mode::ReadWrite;
			gpu_mode = Graphics::GpuMemoryMode::ReadWrite;
			break;
		default: EXIT("unknown prot: %d\n", prot);
	}

	auto in_addr  = reinterpret_cast<uint64_t>(*addr);
	auto out_addr = VirtualMemory::AllocAligned(in_addr, len, mode, alignment);
	*addr         = reinterpret_cast<void*>(out_addr);

	printf("\tin_addr  = 0x%016" PRIx64 "\n", in_addr);
	printf("\tout_addr = 0x%016" PRIx64 "\n", out_addr);
	printf("\tsize     = 0x%016" PRIx64 "\n", len);
	printf("\tmode     = %s\n", Core::EnumName(mode).C_Str());
	printf("\talign    = 0x%016" PRIx64 "\n", alignment);
	printf("\tgpu_mode   = %s\n", Core::EnumName(gpu_mode).C_Str());

	if (out_addr == 0)
	{
		return KERNEL_ERROR_ENOMEM;
	}

	if (!g_physical_memory->Map(out_addr, direct_memory_start, len, prot, mode, gpu_mode))
	{
		printf(FG_RED "\t[Fail]\n" FG_DEFAULT);
		VirtualMemory::Free(out_addr);
		return KERNEL_ERROR_EBUSY;
	}

	if (gpu_mode != Graphics::GpuMemoryMode::NoAccess)
	{
		Graphics::GpuMemorySetAllocatedRange(out_addr, len);
	}

	printf(FG_GREEN "\t[Ok]\n" FG_DEFAULT);

	return OK;
}

int KYTY_SYSV_ABI KernelQueryMemoryProtection(void* addr, void** start, void** end, int* prot)
{
	PRINT_NAME();

	EXIT_IF(g_physical_memory == nullptr);
	EXIT_IF(g_flexible_memory == nullptr);

	EXIT_NOT_IMPLEMENTED(addr == nullptr);

	size_t   len  = 0;
	int      p    = 0;
	uint64_t base = 0;

	if (!g_physical_memory->Find(reinterpret_cast<uint64_t>(addr), &base, &len, &p, nullptr, nullptr))
	{
		if (!g_flexible_memory->Find(reinterpret_cast<uint64_t>(addr), &base, &len, &p, nullptr, nullptr))
		{
			return KERNEL_ERROR_EACCES;
		}
	}

	if (start != nullptr)
	{
		*start = reinterpret_cast<void*>(base);
	}
	if (end != nullptr)
	{
		*end = reinterpret_cast<void*>(base + len - 1);
	}
	if (prot != nullptr)
	{
		*prot = p;
	}

	return OK;
}

} // namespace Kyty::Libs::LibKernel::Memory

#endif // KYTY_EMU_ENABLED