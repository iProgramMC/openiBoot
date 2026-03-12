/***
	The Boron Operating System
	Copyright (C) 2023 iProgramInCpp

Module name:
	rtl/assert.c
	
Abstract:
	This module implements the handler for failed assertions
	in debug mode.
	
Author:
	iProgramInCpp - 11 November 2023
***/
#include <openiboot.h>
#include <util.h>
#include "../main.h"

NO_RETURN
void RtlAbort()
{
	DbgPrint("** ABORTED\n");
	while (1) {
		ASM("wfi");
	}
}

#ifdef DEBUG

NO_RETURN
bool RtlAssert(const char* Condition, const char* File, int Line, const char* Message)
{
	// TODO
	DbgPrint("Assertion failed: %s%s%s%s\nAt %s:%d",
	         Condition,
	         Message ? " (" : "",
	         Message ? Message : "",
	         Message ? ")" : "",
	         File,
	         Line);
	
	RtlAbort();
}

#endif // DEBUG
