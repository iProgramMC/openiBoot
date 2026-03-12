#include "../status.h"

const char* RtlGetStatusString(BSTATUS Status)
{
	switch (Status)
	{
		case STATUS_SUCCESS:              return "Success";
		case STATUS_INVALID_EXECUTABLE:   return "The program is invalid.";
		case STATUS_INVALID_ARCHITECTURE: return "The program is for a different architecture than the current machine.";
	}
	
	return "Unknown Error";
}
