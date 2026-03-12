// This is iProgramInCpp's implementation of the "le" command (Load Elf)
#include "openiboot.h"
#include "commands.h"
#include "util.h"

#include "rtl/elf.h"

BSTATUS BlLoadElfFile(uint8_t* ElfFile)
{
	PELF_HEADER header = (PELF_HEADER) ElfFile;
	BSTATUS Status;
	
	Status = RtlCheckValidity(header);
	if (BFAILED(Status)) {
		return Status;
	}
	
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
		
		memset((void*) phdr->PhysicalAddress, 0, phdr->SizeInMemory);
		
		if (phdr->SizeInFile)
			memcpy((void*) phdr->PhysicalAddress, ElfFile + phdr->Offset, phdr->SizeInFile);
	}
	
	DbgPrint("loaded all phdrs. I think that's it. Jumping to entry...!");
	
	header->EntryPoint ();
	
	DbgPrint("well, it returned. Cool I guess");
	return STATUS_SUCCESS;
}

#define ELF_BASE_ADDRESS 0x09000000

error_t cmd_loadelf(int argc, char** argv)
{
	DbgPrint("Loading elf...");
	
	uint8_t* ElfFile = (uint8_t*) ELF_BASE_ADDRESS;
	
	BSTATUS Status = BlLoadElfFile(ElfFile);
	if (FAILED(Status)) {
		DbgPrint("failed... %s", RtlGetStatusString(Status));
		return EINVAL;
	}
	
	
	return SUCCESS;
}
