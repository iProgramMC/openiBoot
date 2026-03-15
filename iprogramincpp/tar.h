#pragma once

typedef union
{
	struct
	{
		char Name[100];
		char Mode[8];
		char Uid[8];
		char Gid[8];
		char Size[12];
		char ModifyTime[12];
		char Check[8];
		char Type;
		char LinkName[100];
		char Ustar[8];
		char Owner[32];
		char Group[32];
		char Major[8];
		char Minor[8];
		char Prefix[155];
	}
	PACKED;
	
	char Block[512];
}
PACKED
TAR_UNIT, *PTAR_UNIT;
