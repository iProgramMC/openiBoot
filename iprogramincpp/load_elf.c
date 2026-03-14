// This is iProgramInCpp's implementation of the "le" command (Load Elf)
#include "openiboot.h"
#include "commands.h"
#include "util.h"

#include "lcd.h"
#include "arm/arm.h"

#include "rtl/elf.h"
#include "lpb.h"

// TODO: allow configuring cmdline
static char DefaultKernelCommandLine[] = "Root=/initrd.tar Init=/bin/init.exe InitArguments=\"--config /etc/boroninit.cfg\" NoInit=yes";
static char* KernelCommandLine = DefaultKernelCommandLine;

static char OpenIBootName[] = "OpeniBoot";
static char OpenIBootVersion[] = OPENIBOOT_VERSION_STR;

BSTATUS BlLoadElfFile(uint8_t* ElfFile, ELF_ENTRY_POINT* EntryPointOut, uintptr_t* KernelMinAddress, uintptr_t* KernelMaxAddress)
{
	PELF_HEADER header = (PELF_HEADER) ElfFile;
	BSTATUS Status;
	
	Status = RtlCheckValidity(header);
	if (BFAILED(Status)) {
		return Status;
	}
	
	*KernelMinAddress = 0xFFFFFFFF;
	*KernelMaxAddress = 0x00000000;
	
	// look through each program header.
	size_t i;
	for (i = 0; i < header->ProgramHeaderCount; i++)
	{
		PELF_PROGRAM_HEADER phdr = (PELF_PROGRAM_HEADER)(ElfFile + header->ProgramHeadersOffset + header->ProgramHeaderSize * i);
		
		if (phdr->Type != PROG_LOAD)
			continue;
		
		if (!phdr->PhysicalAddress) {
			DbgPrint("ignoring phdr because PAddr = 0");
			continue;
		}
		
		if (!phdr->SizeInMemory) {
			DbgPrint("ignoring phdr because SizeInMemory = 0");
			continue;
		}
		
		DbgPrint("loading phdr. PAddr: %p, SIF: %u, SIM: %u", phdr->PhysicalAddress, phdr->SizeInFile, phdr->SizeInMemory);
		
		if (*KernelMinAddress > phdr->PhysicalAddress)
			*KernelMinAddress = phdr->PhysicalAddress;
		
		if (*KernelMaxAddress < phdr->PhysicalAddress + phdr->SizeInMemory)
			*KernelMaxAddress = phdr->PhysicalAddress + phdr->SizeInMemory;
		
		memset((void*) phdr->PhysicalAddress, 0, phdr->SizeInMemory);
		
		if (phdr->SizeInFile)
			memcpy((void*) phdr->PhysicalAddress, ElfFile + phdr->Offset, phdr->SizeInFile);
	}
	
	DbgPrint("All the program headers have been loaded.");
	*EntryPointOut = header->EntryPoint;
	return STATUS_SUCCESS;
}

#define ELF_BASE_ADDRESS 0x09000000

static LOADER_PARAMETER_BLOCK LoaderParameterBlock;
static LOADER_MEMORY_REGION MemoryRegions[64];
static LOADER_FRAMEBUFFER LoaderFramebuffer;
static LOADER_AP Ap;

static char KernelPath[] = "kernel";

extern char __end__[];

void BlSetupLoaderParameterBlock(uintptr_t KernelMinAddress, uintptr_t KernelMaxAddress)
{
	PLOADER_PARAMETER_BLOCK Lpb = &LoaderParameterBlock;
	
	// ----- Memory Setup -----
	
	// round the addresses to 4KB
	KernelMinAddress &= ~0xFFF;
	KernelMaxAddress = (KernelMaxAddress + 0xFFF) & ~0xFFF;
	
	uintptr_t OIBMinAddress = 0;
	uintptr_t OIBMaxAddress = (uintptr_t) __end__;
	OIBMaxAddress = (OIBMaxAddress + 0xFFF) & ~0xFFF;
	
	DbgPrint("Kernel: %p-%p    OpeniBoot: %p-%p", KernelMinAddress, KernelMaxAddress, OIBMinAddress, OIBMaxAddress);
	
	// Set up region covering the kernel
	PLOADER_MEMORY_REGION Region = &MemoryRegions[0];
	
	Region->Base = KernelMinAddress;
	Region->Size = KernelMaxAddress - KernelMinAddress;
	Region->Type = LOADER_MEM_LOADED_PROGRAM;
	Region++;
	
	// Set up region covering OpeniBoot
	Region->Base = OIBMinAddress;
	Region->Size = OIBMaxAddress - OIBMinAddress;
	Region->Type = LOADER_MEM_LOADER_RECLAIMABLE;
	Region++;
	
	// Set up region covering the area between OpeniBoot and the kernel.
	if (KernelMinAddress > OIBMaxAddress) {
		Region->Base = OIBMaxAddress;
		Region->Size = KernelMinAddress - OIBMaxAddress;
		Region->Type = LOADER_MEM_FREE;
		Region++;
	}
	
	// Set up region covering the rest of main memory.
#if defined CONFIG_IPOD_TOUCH_1G || defined CONFIG_IPHONE_2G || CONFIG_IPHONE_3G || CONFIG_IPOD_TOUCH_2G
	const uintptr_t MainMemorySize = 0x08000000; // 128 MB
#elif defined CONFIG_IPHONE_3GS || defined CONFIG_IPAD_1G || defined CONFIG_IPOD_TOUCH_3G
	const uintptr_t MainMemorySize = 0x10000000; // 256 MB
#elif defined CONFIG_IPHONE_4
	const uintptr_t MainMemorySize = 0x20000000; // 512 MB
#else
	#error Specify the amount of memory you have!
#endif

#if !(defined CONFIG_IPOD_TOUCH_1G)
	#warning This config is untested!
#endif

	Region->Base = KernelMaxAddress;
	Region->Size = MainMemorySize - KernelMaxAddress;
	Region->Type = LOADER_MEM_FREE;
	Region++;
	
	size_t RegionCount = Region - MemoryRegions;
	
	Lpb->MemoryRegions = MemoryRegions;
	Lpb->MemoryRegionCount = RegionCount;
	
	// ----- Frame Buffer Setup -----
	Window* window = currentWindow;
	
	PLOADER_FRAMEBUFFER Fb = &LoaderFramebuffer;
	Fb->Address = (void*) window->framebuffer.buffer;
	Fb->Width = window->framebuffer.width;
	Fb->Height = window->framebuffer.height;
	Fb->IsPhysicalAddress = true;
	
	switch (window->framebuffer.colorSpace) {
		default:
			DbgPrint("Unsupported color space value %d, just passing RGB565", window->framebuffer.colorSpace);
		
		case RGB565:
			Fb->Pitch = window->framebuffer.lineWidth * 2;
			Fb->BitDepth = 16;
			Fb->RedMaskSize = 5;
			Fb->RedMaskShift = 11;
			Fb->GreenMaskSize = 6;
			Fb->GreenMaskShift = 5;
			Fb->BlueMaskSize = 5;
			Fb->BlueMaskShift = 0;
			break;
		
		case RGB888:
			Fb->Pitch = window->framebuffer.lineWidth * 4;
			Fb->BitDepth = 32;
			Fb->RedMaskSize = 8;
			Fb->RedMaskShift = 16;
			Fb->GreenMaskSize = 8;
			Fb->GreenMaskShift = 8;
			Fb->BlueMaskSize = 8;
			Fb->BlueMaskShift = 0;
			break;
	}
	
	Lpb->Framebuffers = Fb;
	Lpb->FramebufferCount = 1;
	
	// ----- MP Setup -----
	Ap.ProcessorId = 0;
	Ap.HardwareId = 0;
	Ap.TrampolineJumpAddress = NULL;
	Ap.ExtraArgument = NULL;
	
	Lpb->Multiprocessor.BootstrapHardwareId = Ap.HardwareId;
	Lpb->Multiprocessor.Count = 1;
	Lpb->Multiprocessor.List = &Ap;
	
	// ----- Module Setup -----
	Lpb->ModuleInfo.Kernel.Path = KernelPath;
	Lpb->ModuleInfo.Kernel.String = KernelCommandLine;
	Lpb->ModuleInfo.Kernel.Address = (void*) KernelMinAddress;
	Lpb->ModuleInfo.Kernel.Size = KernelMaxAddress - KernelMinAddress;
	
	// TODO: support loading modules...
	Lpb->ModuleInfo.List = NULL;
	Lpb->ModuleInfo.Count = 0;
	
	// ----- Command Line Setup -----
	Lpb->CommandLine = KernelCommandLine;
	
	// ----- Loader Info Setup -----
	Lpb->LoaderInfo.Name = OpenIBootName;
	Lpb->LoaderInfo.Version = OpenIBootVersion;
}

error_t cmd_loadelf(int argc, char** argv)
{
	DbgPrint("Loading elf...");
	
	uint8_t* ElfFile = (uint8_t*) ELF_BASE_ADDRESS;
	ELF_ENTRY_POINT EntryPoint = NULL;
	uintptr_t KernelMinAddress, KernelMaxAddress;
	
	BSTATUS Status = BlLoadElfFile(ElfFile, &EntryPoint, &KernelMinAddress, &KernelMaxAddress);
	if (FAILED(Status)) {
		DbgPrint("Loading ELF failed: %s", RtlGetStatusString(Status));
		return EINVAL;
	}
	
	BlSetupLoaderParameterBlock(KernelMinAddress, KernelMaxAddress);
	
	DbgPrint("Okay, entering kernel...");
	EnterCriticalSection();
	
	// bye bye OpeniBoot
	EntryPoint(&LoaderParameterBlock);
	
	DbgPrint("YOU WERE NOT MEANT TO EXIT!");
	while (1) {
		__asm__ volatile("wfi");
	}
}

COMMAND("le", "loads an ELF file from 0x09000000", cmd_loadelf);
