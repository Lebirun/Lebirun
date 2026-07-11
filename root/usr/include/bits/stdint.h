typedef int64_t int_fast16_t;
typedef int64_t int_fast32_t;
typedef uint64_t uint_fast16_t;
typedef uint64_t uint_fast32_t;

#define INT_FAST16_MIN  INT64_MIN
#define INT_FAST32_MIN  INT64_MIN

#define INT_FAST16_MAX  INT64_MAX
#define INT_FAST32_MAX  INT64_MAX

#define UINT_FAST16_MAX UINT64_MAX
#define UINT_FAST32_MAX UINT64_MAX

#define INTPTR_MIN      INT64_MIN
#define INTPTR_MAX      INT64_MAX
#define UINTPTR_MAX     UINT64_MAX
#define PTRDIFF_MIN     INT64_MIN
#define PTRDIFF_MAX     INT64_MAX
#define SIZE_MAX        UINT64_MAX
