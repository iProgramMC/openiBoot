//
//  This is iProgramInCpp's implementation of the Boron OS loader for OpeniBoot.
//
//  It takes in a TAR file located at 0x09000000, extracts all the contents into modules,
//  then loads the kernel located at "kernel.elf" or "./kernel.elf".
//
//  For simplicity, this implementation of tar doesn't support directories or symlinks.
//  They shouldn't really appear in OS images though.
//
//  A typical OS image contains:
//
//   [kernel.elf] - The Boron kernel itself.
//   [initrd.tar] - The initial ramdisk used by the kernel.  Optional, but I don't see myself
//                  implementing support for NAND reading in boron anytime soon
//   [drivers...] - The drivers.  These are passed as boot modules to kernel.elf, starting with
//                  the HAL, which is always first.
//
//  There should be a couple of commands:
//
//   !image.arm.tar - Through oibc, this sends the boron OS image to the iDevice.
//   borongo        - Runs the OS image.
//   boronke        - Sets the kernel file name.  By default it's "kernel.elf"
//   boronhal       - Sets the HAL for the device.  By default it's "halipod1,1.sys" for the iPod 1G
//   boroncmd       - Sets the command line parameters for the OS kernel.
//   boronbase      - Sets the base address of the boron OS tar image.  By default, 0x09000000 (TODO)
//
#include "openiboot.h"
#include "commands.h"
#include "util.h"

#include "lcd.h"
#include "arm/arm.h"
#include "mmu.h"
#include "wdt.h"

#include "rtl/elf.h"
#include "lpb.h"
#include "tar.h"

#define MAX_MEMORY_REGIONS 64
#define MAX_KERNEL_MODULES 16
#define PAGE_SIZE 4096

// The emulator can't send files through ACM so what I made it do is just dump the file
// directly into memory.  Since I'm lazy, I don't feel like transmitting it a size (yet)
// so this is what it is.  Raise if you need to.
#define DEFAULT_OS_IMAGE_SIZE 4097152

static uint32_t OSImageBaseAddress = 0x09000000;

static char OSImageHalPath[64] = "halipod1,1.sys";
static char OSImageKernelPath[64] = "kernel.elf";
static char OSImageCommandLine[512] = "Root=/initrd.tar Init=/bin/init.exe InitArguments=\"--config /etc/boroninit.cfg\" NoInit=yes";

static char OpenIBootName[] = "OpeniBoot";
static char OpenIBootVersion[] = OPENIBOOT_VERSION_STR;

static char BlankString[] = "";

static LOADER_PARAMETER_BLOCK BlParameterBlock;
static LOADER_MEMORY_REGION BlMemoryRegions[MAX_MEMORY_REGIONS];
static LOADER_MODULE BlKernelModules[MAX_KERNEL_MODULES];
static LOADER_FRAMEBUFFER BlFramebuffer;
static LOADER_AP BlAp;

extern char __end__[]; // End of OpeniBoot

static void BlAddAreaToMemMap(uintptr_t StartAddress, size_t Size, int Type)
{
	//DbgPrint("BlAddAreaToMemMap(%p, %u, %d)", StartAddress, Size, Type);
	PLOADER_PARAMETER_BLOCK Lpb = &BlParameterBlock;
	if (Lpb->MemoryRegionCount >= MAX_MEMORY_REGIONS)
		return;
	
	PLOADER_MEMORY_REGION NewRegion = &BlMemoryRegions[Lpb->MemoryRegionCount++];
	NewRegion->Type = Type;
	NewRegion->Base = StartAddress;
	NewRegion->Size = Size;
}

static void BlRemoveAreaFromMemMap(uintptr_t StartAddress, size_t Size, int ReclaimableType)
{
	//DbgPrint("BlRemoveAreaFromMemMap(%p, %u, %d)", StartAddress, Size, ReclaimableType);
	PLOADER_PARAMETER_BLOCK Lpb = &BlParameterBlock;
	
	// Ensure the start address and size greedily cover page boundaries.
	Size += StartAddress & (PAGE_SIZE - 1);
	StartAddress &= ~(PAGE_SIZE - 1);
	Size = (Size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	
	uintptr_t Start = StartAddress;
	uintptr_t End   = StartAddress + Size;
	
	size_t i;
	for (i = 0; i < Lpb->MemoryRegionCount; i++)
	{
		PLOADER_MEMORY_REGION OtherRegion = &BlMemoryRegions[i];
		if (OtherRegion->Type != LOADER_MEM_FREE)
			continue;
		
		uintptr_t OrStart = OtherRegion->Base;
		uintptr_t OrEnd   = OtherRegion->Base + OtherRegion->Size;
		
		if (OrEnd <= Start || End <= OrStart)
		{
			// Not overlapping
			continue;
		}
		
		if (Start <= OrStart && OrEnd <= End)
		{
			// region completely swallowed, so nuke it
			OtherRegion->Type = LOADER_MEM_RESERVED;
			continue;
		}
		
		if (OrStart <= Start && End <= OrEnd)
		{
			// The region we are trying to erase is completely within
			// this memory region.
			
			// We need to create two memory regions:
			// OrStart -- Start - End -- OrEnd
			
			// First, check the trivial cases
			if (OrStart == Start)
			{
				// just set the start to the end
				OtherRegion->Base = End;
				OtherRegion->Size = OrEnd - End;
			}
			else if (OrEnd == End)
			{
				// just set the end to the start
				OtherRegion->Size = Start - OrStart;
			}
			else
			{
				// need to create a separate region
				OtherRegion->Size = Start - OrStart;
				
				if (Lpb->MemoryRegionCount >= MAX_MEMORY_REGIONS)
					continue;
				
				PLOADER_MEMORY_REGION NewRegion = &BlMemoryRegions[Lpb->MemoryRegionCount++];
				NewRegion->Type = LOADER_MEM_FREE;
				NewRegion->Base = End;
				NewRegion->Size = OrEnd - End;
			}
			
			continue;
		}
		
		if (OrStart < End && End < OrEnd && Start < OrStart)
			OrStart = End;
		
		if (Start < OrEnd && OrEnd < End && OrStart < Start)
			OrEnd = Start;
		
		if (OrEnd < OrStart)
		{
			OtherRegion->Type = LOADER_MEM_RESERVED;
			continue;
		}
		
		OtherRegion->Base = OrStart;
		OtherRegion->Size = OrEnd - OrStart;
	}
	
	if (ReclaimableType >= 0)
		BlAddAreaToMemMap(StartAddress, Size, ReclaimableType);
}

void BlSetupLoaderParameterBlock(
	uintptr_t KernelMinAddress,
	uintptr_t KernelMaxAddress,
	uintptr_t OSImageMinAddress,
	uintptr_t OSImageMaxAddress
)
{
	uintptr_t OIBMinAddress = 0;
	uintptr_t OIBMaxAddress = (uintptr_t) __end__;
	OIBMaxAddress = (OIBMaxAddress + 0xFFF) & ~0xFFF;
	
	PLOADER_PARAMETER_BLOCK Lpb = &BlParameterBlock;
	
	// ----- Memory Setup -----
	Lpb->MemoryRegionCount = 0;
	Lpb->MemoryRegions = BlMemoryRegions;
	
	// round the addresses to 4KB
	KernelMinAddress &= ~0xFFF;
	KernelMaxAddress = (KernelMaxAddress + 0xFFF) & ~0xFFF;
	
	DbgPrint("Kernel: %p-%p    OpeniBoot: %p-%p", KernelMinAddress, KernelMaxAddress, OIBMinAddress, OIBMaxAddress);
	
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

	BlAddAreaToMemMap(0x00000000, MainMemorySize, LOADER_MEM_FREE);
	
	// remove kernel
	BlRemoveAreaFromMemMap(KernelMinAddress, KernelMaxAddress - KernelMinAddress, LOADER_MEM_LOADED_PROGRAM);
	BlRemoveAreaFromMemMap(OIBMinAddress, OIBMaxAddress - OIBMinAddress, LOADER_MEM_LOADER_RECLAIMABLE);

	// ----- Frame Buffer Setup -----
	Window* window = currentWindow;
	
	PLOADER_FRAMEBUFFER Fb = &BlFramebuffer;
	Fb->Address = (void*) window->framebuffer.buffer;
	Fb->Width = window->framebuffer.width;
	Fb->Height = window->framebuffer.height;
	Fb->IsPhysicalAddress = true;
	
	switch (window->framebuffer.colorSpace)
	{
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
	
	// NOTE: the framebuffer's address is actually in the upper mirror of main memory
	uintptr_t FramebufferAddress = (uintptr_t) Fb->Address;
	if (MainMemorySize <= 0x08000000) {
		FramebufferAddress &= 0x7FFFFFF;
	}
	
	BlRemoveAreaFromMemMap(FramebufferAddress, Fb->Pitch * Fb->Height, LOADER_MEM_RESERVED);
	
	Lpb->Framebuffers = Fb;
	Lpb->FramebufferCount = 1;
	
	// ----- MP Setup -----
	BlAp.ProcessorId = 0;
	BlAp.HardwareId = 0;
	BlAp.TrampolineJumpAddress = NULL;
	BlAp.ExtraArgument = NULL;
	
	Lpb->Multiprocessor.BootstrapHardwareId = BlAp.HardwareId;
	Lpb->Multiprocessor.Count = 1;
	Lpb->Multiprocessor.List = &BlAp;
	
	// ----- Module Setup -----
	Lpb->ModuleInfo.Kernel.Path = OSImageKernelPath;
	Lpb->ModuleInfo.Kernel.String = OSImageCommandLine;
	Lpb->ModuleInfo.Kernel.Address = (void*) KernelMinAddress;
	Lpb->ModuleInfo.Kernel.Size = KernelMaxAddress - KernelMinAddress;
	
	// TODO: support loading modules...
	Lpb->ModuleInfo.List = BlKernelModules;
	Lpb->ModuleInfo.Count = 0;
	
	// ----- Command Line Setup -----
	Lpb->CommandLine = OSImageCommandLine;
	
	// ----- Loader Info Setup -----
	Lpb->LoaderInfo.Name = OpenIBootName;
	Lpb->LoaderInfo.Version = OpenIBootVersion;
}

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
	
	*EntryPointOut = header->EntryPoint;
	DbgPrint("All the program headers have been loaded.  EntryPoint: %p", header->EntryPoint);
	return STATUS_SUCCESS;
}

static uint32_t BlOctToBin(char *Data, uint32_t Size)
{
	uint32_t Value = 0;
	while (Size > 0)
	{
		Size--;
		Value *= 8;
		Value += *Data++ - '0';
	}
	return Value;
}

static size_t BlGetOSImageSize()
{
	// If ACM sent us a file, received_file_size should be > 0.
	// Otherwise, we either didn't get a file, or we're in an emulator which already
	// put the file in memory so just assume like 1 MB because we're hacky like that
	if (received_file_size)
		return received_file_size;
	
	return DEFAULT_OS_IMAGE_SIZE;
}

// This is a double-edged sword.
//
// If FileName is not NULL, then starts lookup from the beginning and searches for FileName.
//
// Otherwise, gets the next entry without comparing anything, and allows you to continuously
// call the function to get the next file in the list.
static void* BlScanOSImageForFile(
	void* OSImage,
	size_t OSImageSize,
	const char* FileName,
	size_t* OutFileSize,
	const char** OutFileName,
	void** Save
)
{
	PTAR_UNIT CurrentBlock;
	PTAR_UNIT EndBlock = (void*) OSImage + OSImageSize;
	
	if (FileName != NULL || *Save == NULL) {
		// start from beginning
		CurrentBlock = OSImage;
	}
	else {
		CurrentBlock = *Save;
	}
	
	while (CurrentBlock < EndBlock)
	{
		const char* Path = CurrentBlock->Name;
		uint32_t FileSize = BlOctToBin(CurrentBlock->Size, 11);
		
		// If the path starts with "./", skip it.
		if (memcmp(Path, "./", 2) == 0)
			Path += 2;
		
		if (*Path == 0)
			goto Skip;
		
		if (FileName != NULL && strcmp(Path, FileName) == 0)
		{
			// this is the one!
			if (OutFileSize)
				*OutFileSize = FileSize;
			
			if (OutFileName)
				*OutFileName = FileName;
			
			if (Save)
				*Save = NULL;
			
			return &CurrentBlock[1];
		}
		
		if (FileName == NULL)
		{
			// stop here
			*OutFileSize = FileSize;
			*OutFileName = Path;
			*Save = &CurrentBlock[1 + (FileSize + 511) / 512];
			return &CurrentBlock[1];
		}
		
	Skip:
		// Move on to the next entry
		CurrentBlock += 1 + (FileSize + 511) / 512;
	}
	
	if (FileName)
		DbgPrint("Not found: '%s'", FileName);
	
	return NULL;
}

static void BlAddKernelModule(const char* ModuleName, void* ModuleData, size_t ModuleSize)
{
	PLOADER_PARAMETER_BLOCK Lpb = &BlParameterBlock;
	
	if (Lpb->ModuleInfo.Count >= MAX_KERNEL_MODULES)
	{
		DbgPrint("Dropping module '%s' because it goes above the limit.", ModuleName);
		return;
	}
	
	// We only need to add it to this list.  The range should already be excluded
	// from the memory map by excluding the OS image.
	
	PLOADER_MODULE Module = &BlKernelModules[Lpb->ModuleInfo.Count++];
	Module->Path    = (char*) ModuleName;
	Module->String  = BlankString;
	Module->Address = ModuleData;
	Module->Size    = ModuleSize;
}

error_t cmd_boron_go(int argc, char** argv)
{
	void* OSImage = (void*) OSImageBaseAddress;
	size_t OSImageSize = BlGetOSImageSize();
	
	// ---- Load Kernel ----
	void* KernelData = NULL;
	size_t KernelSize = 0;
	
	KernelData = BlScanOSImageForFile(OSImage, OSImageSize, OSImageKernelPath, &KernelSize, NULL, NULL);
	if (!KernelData)
	{
		bufferPrintf("The tar file provided does not contain '%s'.\n", OSImageKernelPath);
		return EINVAL;
	}
	
	ELF_ENTRY_POINT KernelEntryPoint = NULL;
	uintptr_t KernelMinAddress, KernelMaxAddress;
	
	BSTATUS Status = BlLoadElfFile(
		KernelData,
		&KernelEntryPoint,
		&KernelMinAddress,
		&KernelMaxAddress
	);
	(void) KernelSize;
	
	if (BFAILED(Status))
	{
		bufferPrintf("The kernel file '%s' is invalid: %s\n", OSImageHalPath, RtlGetStatusString(Status));
		return EINVAL;
	}
	
	// ---- Setup Loader Parameter Block ----
	BlSetupLoaderParameterBlock(
		KernelMinAddress,
		KernelMaxAddress,
		OSImageBaseAddress,
		OSImageBaseAddress + OSImageSize
	);
	
	// ---- Load HAL ----
	void* HalData = NULL;
	size_t HalSize = 0;
	const char* HalPath = NULL;
	
	HalData = BlScanOSImageForFile(OSImage, OSImageSize, OSImageHalPath, &HalSize, &HalPath, NULL);
	if (!HalData)
	{
		bufferPrintf(
			"WARNING: The tar file provided does not contain '%s', which is the HAL "
			"for your device.  The kernel may not boot properly.\n", OSImageKernelPath
		);
	}
	else
	{
		DbgPrint("Using HAL '%s' from address %p, size %u.", HalPath, HalData, HalSize);
		BlAddKernelModule(HalPath, HalData, HalSize);
	}
	
	// ---- Load Other Modules ----
	void* Data = NULL;
	const char* FileName = NULL;
	size_t FileSize = 0;
	void* Save = NULL;
	
	while ((Data = BlScanOSImageForFile(OSImage, BlGetOSImageSize(), NULL, &FileSize, &FileName, &Save)))
	{
		if (strcmp(FileName, OSImageKernelPath) == 0)
			continue;
		
		if (strcmp(FileName, OSImageHalPath) == 0)
			continue;
		
		DbgPrint("Using module '%s' from address %p, size %u.", FileName, Data, FileSize);
		BlAddKernelModule(FileName, Data, FileSize);
	}
	
	// ---- Jump To Kernel ----
	DbgPrint("Jumping to kernel now ...  Entry point: %p", KernelEntryPoint);
	
	// copy of chainload
	EnterCriticalSection();
#ifndef MALLOC_NO_WDT
	wdt_disable();
#endif
	arm_disable_caches();
	mmu_disable();
	KernelEntryPoint(&BlParameterBlock);
	
	DbgPrint("Unexpected return to bootloader, sorry, we don't support this.");
	while (1) {
		__asm__ volatile("wfi");
	}
}

COMMAND("borongo", "loads the Boron OS image from the specified base address (default 0x09000000)", cmd_boron_go);

// ---- Miscellaneous Commands ----

error_t cmd_boron_hal(int argc, char** argv)
{
	if (argc < 2) {
		DbgPrint("Usage: boronhal [hal file name in OS image]");
		DbgPrint("The current value is '%s'.", OSImageHalPath);
		return EINVAL;
	}
	
	if (strlen(argv[1]) >= sizeof(OSImageHalPath)) {
		DbgPrint("Sorry, the HAL file name is too big, maximum %d chars", sizeof(OSImageHalPath) - 1);
		return EINVAL;
	}
	
	strcpy(OSImageHalPath, argv[1]);
	DbgPrint("Set HAL name to '%s'.", OSImageHalPath);
	return SUCCESS;
}

COMMAND("boronhal", "[boron loader] sets the HAL image file name", cmd_boron_hal);

error_t cmd_boron_cmd(int argc, char** argv)
{
	if (argc < 2) {
		DbgPrint("Usage: boroncmd [command line for OS kernel]");
		DbgPrint("The current value is '%s'.", OSImageCommandLine);
		return EINVAL;
	}
	
	if (strlen(argv[1]) >= sizeof(OSImageCommandLine)) {
		DbgPrint("Sorry, the command line is too big, maximum %d chars", sizeof(OSImageCommandLine) - 1);
		return EINVAL;
	}
	
	strcpy(OSImageCommandLine, argv[1]);
	DbgPrint("Set command line to '%s'.", OSImageCommandLine);
	return SUCCESS;
}

COMMAND("boroncmd", "[boron loader] sets the command line for the kernel", cmd_boron_cmd);

error_t cmd_boron_ke(int argc, char** argv)
{
	if (argc < 2) {
		DbgPrint("Usage: boronke [kernel file name in OS image]");
		DbgPrint("The current value is '%s'.", OSImageCommandLine);
		return EINVAL;
	}
	
	if (strlen(argv[1]) >= sizeof(OSImageKernelPath)) {
		DbgPrint("Sorry, the kernel name is too big, maximum %d chars", sizeof(OSImageKernelPath) - 1);
		return EINVAL;
	}
	
	strcpy(OSImageKernelPath, argv[1]);
	DbgPrint("Set kernel name to '%s'.", OSImageKernelPath);
	return SUCCESS;
}

COMMAND("boronke", "[boron loader] sets the kernel file name", cmd_boron_ke);

#ifdef CONFIG_IPOD_TOUCH_1G

// module to auto-boot on emulator
static void boron_autoboot_check()
{
	uint32_t* checkPtr = (uint32_t*) (OSImageBaseAddress - 4);
	
	// my fork of the iPod touch 1G emulator (https://github.com/iProgramMC/qemu-ios)
	// puts this sentinel value here to initiate the auto-boot process
	if (*checkPtr == 0x4E524241)
	{
		bufferPrintf("Auto-booting Boron ...\n");
		cmd_boron_go(0, NULL);
	}
	else
	{
		bufferPrintf("Not trying to auto-boot, either real hardware or not set up right\n");
	}
}

MODULE_INIT_BOOT(boron_autoboot_check);

#endif
