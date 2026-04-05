#include <stdint.h>

typedef struct {
    const char *name;
    uint64_t addr;
} lke_ksym_auto_t;

const lke_ksym_auto_t ksym_auto_table[] = {
    {0, 0}
};
const int ksym_auto_count = 0;
