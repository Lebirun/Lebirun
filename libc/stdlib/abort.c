#include <stdio.h>
#include <stdlib.h>

__attribute__((__noreturn__))
void abort(void) {
#if defined(__is_libk)
	printf("\n\n!!! KERNEL PANIC !!!\n");
	printf("abort() called - system halted.\n");
	__asm__ volatile ("cli");
	while (1) {
		__asm__ volatile ("hlt");
	}
#else
	/* Userspace: abnormally terminate the process.
	 * Ideally this would raise SIGABRT, but signals are not yet implemented.
	 * For now, print a message and loop forever.
	 */
	printf("abort()\n");
	while (1) { }
#endif
	__builtin_unreachable();
}
