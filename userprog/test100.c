#include <stdint.h>
#include <stddef.h>

typedef unsigned long pthread_t;
typedef struct { int __data; } pthread_mutex_t;
typedef struct { int __data; } pthread_cond_t;
typedef struct { int __data; } pthread_attr_t;
typedef struct { int __data; } pthread_mutexattr_t;
typedef struct { int __data; } pthread_condattr_t;
typedef int key_t;

extern int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine)(void *), void *arg);
extern void pthread_exit(void *retval);
extern int pthread_join(pthread_t thread, void **retval);
extern pthread_t pthread_self(void);
extern int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
extern int pthread_mutex_destroy(pthread_mutex_t *mutex);
extern int pthread_mutex_lock(pthread_mutex_t *mutex);
extern int pthread_mutex_unlock(pthread_mutex_t *mutex);
extern int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
extern int pthread_cond_destroy(pthread_cond_t *cond);
extern int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
extern int pthread_cond_signal(pthread_cond_t *cond);

extern int shmget(key_t key, size_t size, int shmflg);
extern void *shmat(int shmid, const void *shmaddr, int shmflg);
extern int shmdt(const void *shmaddr);
extern int shmctl(int shmid, int cmd, void *buf);

extern void *dlopen(const char *filename, int flags);
extern void *dlsym(void *handle, const char *symbol);
extern int dlclose(void *handle);
extern char *dlerror(void);

extern int write(int fd, const void *buf, size_t count);
extern int getpid(void);
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void exit(int status);
extern void *sbrk(intptr_t inc);

#define IPC_CREAT  01000
#define IPC_RMID   0
#define IPC_PRIVATE 0
#define RTLD_LAZY 0x00001

static void print(const char *s) {
    int len = 0;
    while (s[len]) len++;
    write(1, s, len);
}

static void print_num(int n) {
    char buf[16];
    int i = 15;
    buf[i] = 0;
    int neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    if (n == 0) { buf[--i] = '0'; }
    while (n > 0) {
        buf[--i] = '0' + (n % 10);
        n /= 10;
    }
    if (neg) buf[--i] = '-';
    print(&buf[i]);
}

static void print_hex(unsigned int n) {
    char buf[12] = "0x00000000";
    const char *hex = "0123456789abcdef";
    for (int i = 9; i >= 2; i--) {
        buf[i] = hex[n & 0xf];
        n >>= 4;
    }
    print(buf);
}

static pthread_mutex_t counter_mutex;
static volatile int counter = 0;
static volatile int threads_done = 0;
static pthread_cond_t done_cond;
static pthread_mutex_t done_mutex;

static void *worker_thread(void *arg) {
    int id = (int)(intptr_t)arg;
    
    print("  Thread ");
    print_num(id);
    print(" started (tid=");
    print_num((int)pthread_self());
    print(")\n");
    
    for (int i = 0; i < 10; i++) {
        pthread_mutex_lock(&counter_mutex);
        counter++;
        pthread_mutex_unlock(&counter_mutex);
    }
    
    print("  Thread ");
    print_num(id);
    print(" done\n");
    
    pthread_mutex_lock(&done_mutex);
    threads_done++;
    pthread_cond_signal(&done_cond);
    pthread_mutex_unlock(&done_mutex);
    
    pthread_exit((void *)(intptr_t)(id * 100));
    return (void*)0;
}

static void test_threading(void) {
    print("\n=== Multi-Threading Test ===\n");
    
    pthread_mutex_init(&counter_mutex, (void*)0);
    pthread_mutex_init(&done_mutex, (void*)0);
    pthread_cond_init(&done_cond, (void*)0);
    
    counter = 0;
    threads_done = 0;
    
    pthread_t threads[4];
    int num_threads = 4;
    
    print("Creating ");
    print_num(num_threads);
    print(" threads...\n");
    
    for (int i = 0; i < num_threads; i++) {
        int ret = pthread_create(&threads[i], (void*)0, worker_thread, (void*)(intptr_t)(i + 1));
        if (ret != 0) {
            print("  ERROR: pthread_create failed for thread ");
            print_num(i + 1);
            print(" (ret=");
            print_num(ret);
            print(")\n");
        } else {
            print("  Created thread ");
            print_num(i + 1);
            print(" (handle=");
            print_num((int)threads[i]);
            print(")\n");
        }
    }
    
    print("Waiting for threads to finish...\n");
    
    for (int i = 0; i < num_threads; i++) {
        void *retval = (void*)0;
        int ret = pthread_join(threads[i], &retval);
        if (ret != 0) {
            print("  ERROR: pthread_join failed for thread ");
            print_num(i + 1);
            print("\n");
        } else {
            print("  Thread ");
            print_num(i + 1);
            print(" joined, retval=");
            print_num((int)(intptr_t)retval);
            print("\n");
        }
    }
    
    print("Final counter value: ");
    print_num(counter);
    print(" (expected: ");
    print_num(num_threads * 10);
    print(")\n");
    
    if (counter == num_threads * 10) {
        print("PASS: Counter matches expected value\n");
    } else {
        print("FAIL: Counter mismatch\n");
    }
    
    pthread_mutex_destroy(&counter_mutex);
    pthread_mutex_destroy(&done_mutex);
    pthread_cond_destroy(&done_cond);
}

static void test_shared_memory(void) {
    print("\n=== Shared Memory Test ===\n");
    
    int shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0666);
    if (shmid < 0) {
        print("  ERROR: shmget failed (ret=");
        print_num(shmid);
        print(")\n");
        return;
    }
    print("  shmget returned shmid=");
    print_num(shmid);
    print("\n");
    
    void *addr = shmat(shmid, (void*)0, 0);
    if ((intptr_t)addr < 0) {
        print("  ERROR: shmat failed\n");
        shmctl(shmid, IPC_RMID, (void*)0);
        return;
    }
    print("  shmat returned addr=");
    print_hex((unsigned int)(uintptr_t)addr);
    print("\n");
    
    int *shared_int = (int *)addr;
    *shared_int = 12345;
    print("  Wrote value ");
    print_num(*shared_int);
    print(" to shared memory\n");
    
    int read_val = *shared_int;
    print("  Read back value: ");
    print_num(read_val);
    print("\n");
    
    if (read_val == 12345) {
        print("PASS: Shared memory read/write works\n");
    } else {
        print("FAIL: Shared memory value mismatch\n");
    }
    
    int ret = shmdt(addr);
    print("  shmdt returned: ");
    print_num(ret);
    print("\n");
    
    ret = shmctl(shmid, IPC_RMID, (void*)0);
    print("  shmctl(IPC_RMID) returned: ");
    print_num(ret);
    print("\n");
}

static void test_dlopen(void) {
    print("\n=== Dynamic Loading Test ===\n");
    
    void *handle = dlopen("/lib/libtest.so", RTLD_LAZY);
    if (handle) {
        print("  dlopen returned handle=");
        print_hex((unsigned int)(uintptr_t)handle);
        print("\n");
        
        typedef int (*test_func_t)(void);
        typedef int (*add_func_t)(int, int);
        typedef int (*mul_func_t)(int, int);
        
        void *sym = dlsym(handle, "test_function");
        if (sym) {
            print("  dlsym found test_function at ");
            print_hex((unsigned int)(uintptr_t)sym);
            print("\n");
            
            test_func_t func = (test_func_t)sym;
            int result = func();
            print("  test_function() returned: ");
            print_num(result);
            print("\n");
            
            if (result == 42) {
                print("PASS: test_function returned expected value\n");
            } else {
                print("FAIL: test_function returned wrong value\n");
            }
        } else {
            char *err = dlerror();
            if (err) {
                print("  dlsym error for test_function: ");
                print(err);
                print("\n");
            }
        }
        
        sym = dlsym(handle, "test_add");
        if (sym) {
            print("  dlsym found test_add at ");
            print_hex((unsigned int)(uintptr_t)sym);
            print("\n");
            
            add_func_t add = (add_func_t)sym;
            int result = add(10, 32);
            print("  test_add(10, 32) returned: ");
            print_num(result);
            print("\n");
            
            if (result == 42) {
                print("PASS: test_add returned expected value\n");
            } else {
                print("FAIL: test_add returned wrong value\n");
            }
        } else {
            char *err = dlerror();
            if (err) {
                print("  dlsym error for test_add: ");
                print(err);
                print("\n");
            }
        }
        
        sym = dlsym(handle, "test_multiply");
        if (sym) {
            print("  dlsym found test_multiply at ");
            print_hex((unsigned int)(uintptr_t)sym);
            print("\n");
            
            mul_func_t mul = (mul_func_t)sym;
            int result = mul(6, 7);
            print("  test_multiply(6, 7) returned: ");
            print_num(result);
            print("\n");
            
            if (result == 42) {
                print("PASS: test_multiply returned expected value\n");
            } else {
                print("FAIL: test_multiply returned wrong value\n");
            }
        } else {
            char *err = dlerror();
            if (err) {
                print("  dlsym error for test_multiply: ");
                print(err);
                print("\n");
            }
        }
        
        sym = dlsym(handle, "nonexistent_symbol");
        if (!sym) {
            char *err = dlerror();
            if (err) {
                print("  Expected error for nonexistent: ");
                print(err);
                print("\n");
            }
            print("PASS: nonexistent symbol correctly not found\n");
        } else {
            print("FAIL: found nonexistent symbol\n");
        }
        
        int ret = dlclose(handle);
        print("  dlclose returned: ");
        print_num(ret);
        print("\n");
        
        if (ret == 0) {
            print("PASS: dlclose succeeded\n");
        } else {
            print("FAIL: dlclose failed\n");
        }
    } else {
        char *err = dlerror();
        if (err) {
            print("  dlopen error: ");
            print(err);
            print("\n");
        } else {
            print("  dlopen failed with no error message\n");
        }
        print("NOTE: Make sure /lib/libtest.so exists\n");
    }
}

static void test_environment(void) {
    print("\n=== Environment Variables Test ===\n");
    
    extern char *getenv(const char *name);
    extern int setenv(const char *name, const char *value, int overwrite);
    extern int unsetenv(const char *name);
    
    int ret = setenv("TEST100_VAR", "hello_world", 1);
    print("  setenv(TEST100_VAR=hello_world) returned: ");
    print_num(ret);
    print("\n");
    
    char *val = getenv("TEST100_VAR");
    if (val) {
        print("  getenv(TEST100_VAR) = ");
        print(val);
        print("\n");
        
        int match = 1;
        const char *expected = "hello_world";
        for (int i = 0; expected[i] || val[i]; i++) {
            if (expected[i] != val[i]) { match = 0; break; }
        }
        if (match) {
            print("PASS: Environment variable matches\n");
        } else {
            print("FAIL: Environment variable mismatch\n");
        }
    } else {
        print("  ERROR: getenv returned NULL\n");
    }
    
    ret = unsetenv("TEST100_VAR");
    print("  unsetenv returned: ");
    print_num(ret);
    print("\n");
    
    val = getenv("TEST100_VAR");
    if (val) {
        print("  FAIL: Variable still exists after unsetenv\n");
    } else {
        print("  PASS: Variable removed after unsetenv\n");
    }
}

static void test_fork_exec(void) {
    print("\n=== Fork Test ===\n");
    
    int pid = fork();
    if (pid < 0) {
        print("  ERROR: fork failed\n");
        return;
    }
    
    if (pid == 0) {
        print("  Child process running (pid=");
        print_num(getpid());
        print(")\n");
        exit(42);
    } else {
        print("  Parent process (pid=");
        print_num(getpid());
        print("), child pid=");
        print_num(pid);
        print("\n");
        
        int status = 0;
        int ret = waitpid(pid, &status, 0);
        print("  waitpid returned: ");
        print_num(ret);
        print(", status=");
        print_num(status);
        print("\n");
        
        if (ret == pid) {
            print("PASS: Fork and wait succeeded\n");
        } else {
            print("FAIL: waitpid returned wrong pid\n");
        }
    }
}

static void test_memory(void) {
    print("\n=== Memory Allocation Test ===\n");
    
    void *initial = sbrk(0);
    print("  Initial brk: ");
    print_hex((unsigned int)(uintptr_t)initial);
    print("\n");
    
    void *ptr = sbrk(4096);
    if ((intptr_t)ptr == -1) {
        print("  ERROR: sbrk(4096) failed\n");
        return;
    }
    print("  sbrk(4096) returned: ");
    print_hex((unsigned int)(uintptr_t)ptr);
    print("\n");
    
    void *cur = sbrk(0);
    print("  Current brk: ");
    print_hex((unsigned int)(uintptr_t)cur);
    print("\n");
    
    int *test = (int *)ptr;
    *test = 0xDEADBEEF;
    if (*test == (int)0xDEADBEEF) {
        print("PASS: Memory allocation works\n");
    } else {
        print("FAIL: Memory write/read failed\n");
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    print("========================================\n");
    print("       test100 - Feature Test Suite\n");
    print("========================================\n");
    print("PID: ");
    print_num(getpid());
    print("\n");
    
    test_threading();
    test_shared_memory();
    test_dlopen();
    test_environment();
    test_fork_exec();
    test_memory();
    
    print("\n========================================\n");
    print("          All tests completed!\n");
    print("========================================\n");
    
    return 0;
}
