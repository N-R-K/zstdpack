/*
 * Test payload: prints its arguments, where it was executed from (a memfd
 * path when unpacked by zstdpack) and how many file descriptors above
 * stderr it inherited, then exits 42 so the caller can check that
 * arguments and exit status survive the trip.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static int extra_fds(void)
{
	int n = 0, fd;

	for (fd = 3; fd < 64; fd++)
		if (fcntl(fd, F_GETFD) != -1)
			n++;
	return n;
}

int main(int argc, char **argv)
{
	char where[256];
	ssize_t n = readlink("/proc/self/exe", where, sizeof where - 1);

	if (n < 0)
		n = 0;
	where[n] = '\0';
	for (int i = 1; i < argc; i++)
		printf("%s ", argv[i]);
	printf("| exe=%s | fds=%d\n", where, extra_fds());
	return 42;
}
