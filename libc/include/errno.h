#ifndef _ERRNO_H
#define _ERRNO_H 1

#ifdef __cplusplus
extern "C" {
#endif

extern int errno;

int* __errno_location(void);

#define EDOM        1   
#define ERANGE      2  
#define EILSEQ      3  
#define EINVAL      4 
#define ENOMEM      5  
#define EOVERFLOW   6 
#define ENOSYS      7   

#ifdef __cplusplus
}
#endif

#endif