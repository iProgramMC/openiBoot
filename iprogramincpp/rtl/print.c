#include <openiboot.h>
#include <util.h>
#include <stdarg.h>

void LogMsg(const char* fmt, ...)
{
	va_list vl;
	va_start(vl, fmt);
	static char buff[1024];
	vsprintf(buff, fmt, vl); // <sigh> unsafe, but what can you do
	va_end(vl);
	strcpy(buff + strlen(buff), "\n");
	bufferPrintf(buff);
}
