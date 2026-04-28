#include <lebirun/lke.h>

LKE_NAME("LkeTest");
LKE_DESC("Sample Lebirun Kernel Extension module");
LKE_LICENSE("MIT");
LKE_AUTHOR("Lebirun");
LKE_VERSION_STR("1.0");

extern int printf(const char *fmt, ...);

static int lketest_init(void) {
    printf("LkeTest: module loaded\n");
    return 0;
}

static void lketest_exit(void) {
    printf("LkeTest: module unloaded\n");
}

module_init(lketest_init);
module_exit(lketest_exit);
