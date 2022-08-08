#ifndef COMPAT_NONBLOCK_H
#define COMPAT_NONBLOCK_H

/*
 * Enable non-blocking I/O for the passed-in descriptor. Note that this is a
 * noop on systems without O_NONBLOCK, like Windows! Use with caution.
 */
int enable_nonblock(int fd);

#endif
