#ifndef DEBUG_H
#define DEBUG_H

#if !defined(__cplusplus) && !defined(__bool_true_false_are_defined)
typedef unsigned char bool;
#define true  1
#define false 0
#define __bool_true_false_are_defined 1
#endif

static bool debugMode = false;

#endif