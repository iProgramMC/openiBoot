#pragma once

typedef enum
{
	STATUS_SUCCESS,
	STATUS_INVALID_EXECUTABLE,
	STATUS_INVALID_ARCHITECTURE,
}
BSTATUS;

#define BSUCCEEDED(Status) ((Status) == STATUS_SUCCESS)
#define BFAILED(Status) (!BSUCCEEDED(Status))

const char* RtlGetStatusString(BSTATUS Status);
