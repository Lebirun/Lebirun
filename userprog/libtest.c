__attribute__((visibility("default"))) int test_add(int a, int b) {
    return a + b;
}

__attribute__((visibility("default"))) int test_multiply(int a, int b) {
    return a * b;
}

__attribute__((visibility("default"))) int test_function(void) {
    return 42;
}

__attribute__((visibility("default"))) int libtest_global_value = 12345;
