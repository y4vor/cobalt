#include "stdio_impl.h"
#include <sys/ioctl.h>

size_t __stdout_write(FILE *f, const unsigned char *buf, size_t len)
{
	struct winsize wsz;
	f->write = __stdio_write;
#if defined(STARBOARD)
	if (!(f->flags & F_SVB) && ioctl( f->fd, TIOCGWINSZ, &wsz))
#else
	if (!(f->flags & F_SVB) && __syscall(SYS_ioctl, f->fd, TIOCGWINSZ, &wsz))
#endif
		f->lbf = -1;
	return __stdio_write(f, buf, len);
}
