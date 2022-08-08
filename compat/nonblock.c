#include "git-compat-util.h"
#include "nonblock.h"

#ifdef O_NONBLOCK

int enable_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return -1;
	flags |= O_NONBLOCK;
	return fcntl(fd, F_SETFL, flags);
}

#else

int enable_nonblock(int fd)
{
	return 0;
}

#endif
