/*
htop - UnsupportedMachine.c
(C) 2014 Hisham H. Muhammad
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "UnsupportedMachine.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "Machine.h"


Machine* Machine_new(UsersTable* usersTable, uid_t userId) {
   UnsupportedMachine* this = xCalloc(1, sizeof(UnsupportedMachine));
   Machine* super = &this->super;

   Machine_init(super, usersTable, userId);

   super->existingCPUs = 1;
   super->activeCPUs = 1;

   return super;
}

void Machine_delete(Machine* super) {
   UnsupportedMachine* this = (UnsupportedMachine*) super;
   Machine_done(super);
   free(this);
}

bool Machine_isCPUonline(const Machine* host, unsigned int id) {
   assert(id < host->existingCPUs);

   (void) host; (void) id;

   return true;
}

void Machine_scan(Machine* super) {
   UnsupportedMachine* this = (UnsupportedMachine*) super;
   FILE* f;
   char line[256];
   unsigned long long val;
   unsigned long long curUser, curNice, curSystem, curIdle;
   unsigned long long totalNew, totalOld;

   super->existingCPUs = 1;
   super->activeCPUs = 1;

   super->totalMem = 0;
   super->usedMem = 0;
   super->buffersMem = 0;
   super->cachedMem = 0;
   super->sharedMem = 0;
   super->availableMem = 0;
   super->totalSwap = 0;
   super->usedSwap = 0;
   super->cachedSwap = 0;

   f = fopen("/proc/meminfo", "r");
   if (f) {
      while (fgets(line, sizeof(line), f)) {
         if (sscanf(line, "MemTotal: %llu kB", &val) == 1)
            super->totalMem = val;
         else if (sscanf(line, "MemFree: %llu kB", &val) == 1)
            super->availableMem = val;
         else if (sscanf(line, "MemAvailable: %llu kB", &val) == 1)
            super->availableMem = val;
         else if (sscanf(line, "Buffers: %llu kB", &val) == 1)
            super->buffersMem = val;
         else if (sscanf(line, "Cached: %llu kB", &val) == 1)
            super->cachedMem = val;
         else if (sscanf(line, "Shmem: %llu kB", &val) == 1)
            super->sharedMem = val;
         else if (sscanf(line, "SwapTotal: %llu kB", &val) == 1)
            super->totalSwap = val;
         else if (sscanf(line, "SwapFree: %llu kB", &val) == 1)
            super->usedSwap = super->totalSwap - val;
      }
      fclose(f);
      super->usedMem = super->totalMem - super->availableMem;
   }

   curUser = 0;
   curNice = 0;
   curSystem = 0;
   curIdle = 0;
   f = fopen("/proc/stat", "r");
   if (f) {
      while (fgets(line, sizeof(line), f)) {
         if (line[0] == 'c' && line[1] == 'p' && line[2] == 'u' && line[3] == ' ') {
            sscanf(line, "cpu  %llu %llu %llu %llu",
               &curUser, &curNice, &curSystem, &curIdle);
            curUser += curNice;
         } else if (sscanf(line, "btime %llu", &val) == 1) {
            this->btime = (time_t)val;
         }
      }
      fclose(f);
   }

   totalNew = curUser + curSystem + curIdle;
   totalOld = this->prevUser + this->prevSystem + this->prevIdle;
   this->totalPeriod = (double)(totalNew - totalOld);
   if (this->totalPeriod < 1.0)
      this->totalPeriod = 1.0;
   this->userPeriod = (double)(curUser - this->prevUser);
   this->systemPeriod = (double)(curSystem - this->prevSystem);
   this->prevUser = curUser;
   this->prevSystem = curSystem;
   this->prevIdle = curIdle;
}
