extern int main(int argc, char** argv);
extern void exit(int status);

void __attribute__((section(".text.entry"))) _start(void) {
	int ret = main(0, (char**)0);
	exit(ret);
	__builtin_unreachable();
}
