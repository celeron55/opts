#pragma once
#ifdef __WIN32__
#  include "windows_includes.hpp"
#else
#endif

static ss_ read_any(int fd, bool *dst_error=NULL)
{
#ifndef __WIN32__
	struct pollfd fds;
	int ret;
	fds.fd = fd;
	fds.events = POLLIN;
	ret = poll(&fds, 1, 0);
	if(ret == 1){
		char buf[1000];
		ssize_t n = read(fd, buf, 1000);
		if(n == 0)
			return "";
		return ss_(buf, n);
	} else if(ret == 0){
		return "";
	} else {
		// Error
		if(dst_error)
			*dst_error = true;
		return "";
	}
#else
	if(fd != 0)
		return "";
	static HANDLE stdinHandle;
	stdinHandle = GetStdHandle(STD_INPUT_HANDLE);
	switch( WaitForSingleObject( stdinHandle, 1 ) ) // timeout ms
	{
	case( WAIT_TIMEOUT ):
		break; // return from this function to allow thread to terminate
	case( WAIT_OBJECT_0 ):
		if( _kbhit() ) // _kbhit() always returns immediately
		{
			int i = _getch();
			if(i == '\r')
				printf_("\r\n");
			else
				printf_("%c", i);
			return ss_()+(char)i;
		}
		else // some sort of other events , we need to clear it from the queue
		{
			// clear events
			INPUT_RECORD r[512];
			DWORD read;
			ReadConsoleInput( stdinHandle, r, 512, &read );
		}
		break;
	case( WAIT_FAILED ):
		printf_("INFO: stdin WAIT_FAILED\n");
		break;
	case( WAIT_ABANDONED ): 
		printf_("INFO: stdin WAIT_ABANDONED\n");
		break;
	default:
		printf_("INFO: stdin: Unknown return value from WaitForSingleObject\n");
	}
	return "";
#endif
}

