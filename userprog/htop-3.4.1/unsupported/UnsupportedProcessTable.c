/*
htop - UnsupportedProcessTable.c
(C) 2014 Hisham H. Muhammad
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "UnsupportedProcessTable.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <ctype.h>

#include "ProcessTable.h"
#include "UnsupportedProcess.h"
#include "UnsupportedMachine.h"
#include "UsersTable.h"


ProcessTable* ProcessTable_new(Machine* host, Hashtable* pidMatchList) {
   UnsupportedProcessTable* this = xCalloc(1, sizeof(UnsupportedProcessTable));
   Object_setClass(this, Class(ProcessTable));

   ProcessTable* super = &this->super;
   ProcessTable_init(super, Class(Process), host, pidMatchList);

   return super;
}

void ProcessTable_delete(Object* cast) {
   UnsupportedProcessTable* this = (UnsupportedProcessTable*) cast;
   ProcessTable_done(&this->super);
   free(this);
}

static ProcessState proc_state_char(char c) {
   switch (c) {
      case 'R': return RUNNING;
      case 'D': return UNINTERRUPTIBLE_WAIT;
      case 'Z': return ZOMBIE;
      case 'T': return STOPPED;
      default:  return SLEEPING;
   }
}

void ProcessTable_goThroughEntries(ProcessTable* super) {
   DIR* dir;
   struct dirent* entry;
   Machine* host = super->super.host;

   dir = opendir("/proc");
   if (!dir)
      return;

   while ((entry = readdir(dir)) != NULL) {
      int pid;
      FILE* f;
      char path[256];
      char buf[1024];
      char* loc;
      char* end;
      char comm[64];
      char state_c;
      int ppid, pgrp, session, tty_nr, tpgid;
      unsigned long flags_val, minflt, cminflt, majflt, cmajflt;
      unsigned long utime_val, stime_val;
      long cutime, cstime, priority_val, nice_val, nlwp_val;
      long itrealvalue;
      unsigned long long starttime;
      unsigned long vsize;
      long rss;
      bool preExisting;
      Process* proc;
      int comm_len;
      UnsupportedProcess* up;
      UnsupportedMachine* uhost;
      unsigned long long int prevTime;
      unsigned int st_uid;

      if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
         continue;

      pid = atoi(entry->d_name);
      if (pid <= 0)
         continue;

      snprintf(path, sizeof(path), "/proc/%d/stat", pid);
      f = fopen(path, "r");
      if (!f)
         continue;

      if (!fgets(buf, sizeof(buf), f)) {
         fclose(f);
         continue;
      }
      fclose(f);

      loc = strchr(buf, '(');
      if (!loc) continue;
      loc++;
      end = strrchr(loc, ')');
      if (!end) continue;

      comm_len = (int)(end - loc);
      if (comm_len >= (int)sizeof(comm))
         comm_len = (int)sizeof(comm) - 1;
      memcpy(comm, loc, (size_t)comm_len);
      comm[comm_len] = '\0';

      loc = end + 2;
      state_c = *loc;
      loc += 2;

      ppid = 0; pgrp = 0; session = 0; tty_nr = 0; tpgid = 0;
      flags_val = 0; minflt = 0; cminflt = 0; majflt = 0; cmajflt = 0;
      utime_val = 0; stime_val = 0; cutime = 0; cstime = 0;
      priority_val = 20; nice_val = 0; nlwp_val = 1; itrealvalue = 0;
      starttime = 0; vsize = 0; rss = 0;

      sscanf(loc, "%d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %llu %lu %ld",
         &ppid, &pgrp, &session, &tty_nr, &tpgid,
         &flags_val, &minflt, &cminflt, &majflt, &cmajflt,
         &utime_val, &stime_val, &cutime, &cstime,
         &priority_val, &nice_val, &nlwp_val, &itrealvalue,
         &starttime, &vsize, &rss);

      preExisting = true;
      proc = ProcessTable_getProcess(super, pid, &preExisting, UnsupportedProcess_new);

      Process_setPid(proc, pid);
      Process_setParent(proc, ppid);
      Process_setThreadGroup(proc, pid);
      proc->isUserlandThread = false;

      Process_updateComm(proc, comm);
      Process_updateCmdline(proc, comm, 0, comm_len);

      proc->state = proc_state_char(state_c);
      proc->pgrp = pgrp;
      proc->session = session;
      proc->tty_nr = (unsigned long)tty_nr;
      proc->tpgid = tpgid;
      proc->processor = 0;
      proc->isKernelThread = false;

      proc->time = (utime_val + stime_val);
      proc->minflt = minflt;
      proc->majflt = majflt;
      proc->priority = priority_val;
      proc->nice = nice_val;
      proc->nlwp = nlwp_val;
      proc->m_virt = (long)(vsize / 1024);
      proc->m_resident = rss * 4;

      st_uid = 0;
      snprintf(path, sizeof(path), "/proc/%d/status", pid);
      f = fopen(path, "r");
      if (f) {
         int kt = 0;
         while (fgets(buf, sizeof(buf), f)) {
            if (sscanf(buf, "Uid:\t%u", &st_uid) == 1)
               ;
            if (sscanf(buf, "KernelTask:\t%d", &kt) == 1)
               proc->isKernelThread = (kt != 0);
         }
         fclose(f);
      }
      if (proc->st_uid != st_uid) {
         proc->st_uid = st_uid;
         proc->user = UsersTable_getRef(host->usersTable, st_uid);
      }

      if (host->totalMem > 0)
         proc->percent_mem = (float)(proc->m_resident) / (float)(host->totalMem) * 100.0f;
      else
         proc->percent_mem = 0.0f;

      up = (UnsupportedProcess*)proc;
      uhost = (UnsupportedMachine*)host;
      prevTime = up->prevTime;
      up->prevTime = proc->time;
      if (preExisting && uhost->totalPeriod > 0.0) {
         proc->percent_cpu = (float)((double)(proc->time - prevTime) / uhost->totalPeriod * 100.0);
      } else {
         proc->percent_cpu = 0.0f;
      }
      Process_updateCPUFieldWidths(proc->percent_cpu);

      if (proc->starttime_ctime == 0) {
         proc->starttime_ctime = uhost->btime + (time_t)(starttime / 100);
         Process_fillStarttimeBuffer(proc);
      }

      proc->super.updated = true;
      proc->super.show = true;

      if (!preExisting)
         ProcessTable_add(super, proc);
   }

   closedir(dir);
}
