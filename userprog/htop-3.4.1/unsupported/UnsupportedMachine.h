#ifndef HEADER_UnsupportedMachine
#define HEADER_UnsupportedMachine
/*
htop - UnsupportedMachine.h
(C) 2014 Hisham H. Muhammad
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "Machine.h"


typedef struct UnsupportedMachine_ {
   Machine super;
   unsigned long long int prevUser;
   unsigned long long int prevSystem;
   unsigned long long int prevIdle;
   double userPeriod;
   double systemPeriod;
   double totalPeriod;
   time_t btime;
} UnsupportedMachine;

#endif
