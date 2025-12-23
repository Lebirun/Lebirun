#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H 1

#include <sys/cdefs.h>
#include <limits.h>

#define MAXPATHLEN 4096
#define PATH_MAX 4096
#define MAXHOSTNAMELEN 256
#define MAXSYMLINKS 20

#define NBBY 8

#define NGROUPS 65536
#define NOGROUP (-1)

#define DEV_BSIZE 512

#define NBPG 4096
#define PGSHIFT 12
#define PGOFSET (NBPG - 1)

#ifndef NOFILE
#define NOFILE 256
#endif

#ifndef HZ
#define HZ 100
#endif

#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif

#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif

#ifndef rounddown
#define rounddown(x, y) (((x) / (y)) * (y))
#endif

#ifndef powerof2
#define powerof2(x) ((((x) - 1) & (x)) == 0)
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef ALIGN
#define ALIGN(x) (((x) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#endif

#define CLSIZE 1
#define CLSIZELOG2 0

#define setbit(a, i) ((a)[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(a, i) ((a)[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(a, i) ((a)[(i) / NBBY] & (1 << ((i) % NBBY)))
#define isclr(a, i) (((a)[(i) / NBBY] & (1 << ((i) % NBBY))) == 0)

#endif
