#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#define futex(A, B, C, D, E, F)		syscall(__NR_futex, A, B, C, D, E, F)

char *testcase_description = "futex(FUTEX_WAIT)";

void testcase(unsigned long long *iterations, unsigned long nr)
{
	while (1) {
		unsigned int addr = 0;

		futex(&addr, FUTEX_WAIT, 1, NULL, NULL, 0);

		(*iterations)++;
	}
}
