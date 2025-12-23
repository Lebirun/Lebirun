#include <stddef.h>
#include <unistd.h>

extern int main(int argc, char** argv);
extern void exit(int status);

extern char __bss_start[];
extern char __bss_end[];

void __attribute__((section(".text.entry"))) _start(void) {
	for (char *p = __bss_start; p < __bss_end; p++) {
		*p = 0;
	}
	
	char *argv[] = { "", NULL };
	int ret = main(1, argv);
	exit(ret);
	__builtin_unreachable();
}
