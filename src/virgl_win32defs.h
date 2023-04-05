/*
Usage:
#ifdef _WIN32
#include "virgl_win32defs.h"
#endif
*/
#ifndef VIRGL_WIN32DEFS_H
#define VIRGL_WIN32DEFS_H
#include <process.h>
#include <io.h>
#include <stdlib.h>
#define getpid _getpid

/// Begin setenv
static int setenv(const char *var, const char *value, int overwrite)
{
	if (!overwrite && getenv(var)) return 0;
	int ret=_putenv_s(var, value);
    return ret;
}
static int unsetenv(const char* var){
    return _putenv_s(var, "");
}
/// End setenv

#endif