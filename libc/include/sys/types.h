#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H 1

#include <sys/cdefs.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t pid_t;        
#define __pid_t_defined 1
typedef uint32_t uid_t;       
typedef uint32_t gid_t;       

typedef int32_t off_t;        
typedef uint32_t ino_t;      
typedef uint32_t dev_t;         
typedef uint32_t mode_t;   
typedef uint32_t nlink_t;     
typedef uint32_t blksize_t; 
typedef uint32_t blkcnt_t;   

typedef int32_t ssize_t;   

typedef int32_t time_t;       
typedef int32_t suseconds_t;  
typedef uint32_t useconds_t;   
typedef uint32_t clock_t;      

typedef int32_t key_t;        

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;  
typedef uint16_t in_port_t;     
typedef uint32_t in_addr_t;    

typedef int32_t id_t;   
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;

typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;

typedef long register_t;

typedef uintptr_t caddr_t;

#ifdef __cplusplus
}
#endif

#endif
