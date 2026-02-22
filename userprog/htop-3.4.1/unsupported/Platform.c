/*
htop - unsupported/Platform.c
(C) 2014 Hisham H. Muhammad
(C) 2015 David C. Hunt
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "unsupported/Platform.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "CPUMeter.h"
#include "ClockMeter.h"
#include "DateMeter.h"
#include "DateTimeMeter.h"
#include "FileDescriptorMeter.h"
#include "HostnameMeter.h"
#include "LoadAverageMeter.h"
#include "Macros.h"
#include "MemoryMeter.h"
#include "MemorySwapMeter.h"
#include "SwapMeter.h"
#include "SysArchMeter.h"
#include "TasksMeter.h"
#include "UptimeMeter.h"
#include "UnsupportedMachine.h"
#include "XUtils.h"


const ScreenDefaults Platform_defaultScreens[] = {
   {
      .name = "Main",
      .columns = "PID USER PRIORITY NICE M_VIRT M_RESIDENT STATE PERCENT_CPU PERCENT_MEM TIME Command",
      .sortKey = "PERCENT_CPU",
   },
};

const unsigned int Platform_numberOfDefaultScreens = ARRAYSIZE(Platform_defaultScreens);

const SignalItem Platform_signals[] = {
   { .name = " 0 Cancel",    .number =  0 },
};

const unsigned int Platform_numberOfSignals = ARRAYSIZE(Platform_signals);

const MeterClass* const Platform_meterTypes[] = {
   &CPUMeter_class,
   &ClockMeter_class,
   &DateMeter_class,
   &DateTimeMeter_class,
   &LoadAverageMeter_class,
   &LoadMeter_class,
   &MemoryMeter_class,
   &SwapMeter_class,
   &MemorySwapMeter_class,
   &TasksMeter_class,
   &BatteryMeter_class,
   &HostnameMeter_class,
   &SysArchMeter_class,
   &UptimeMeter_class,
   &AllCPUsMeter_class,
   &AllCPUs2Meter_class,
   &AllCPUs4Meter_class,
   &AllCPUs8Meter_class,
   &LeftCPUsMeter_class,
   &RightCPUsMeter_class,
   &LeftCPUs2Meter_class,
   &RightCPUs2Meter_class,
   &LeftCPUs4Meter_class,
   &RightCPUs4Meter_class,
   &LeftCPUs8Meter_class,
   &RightCPUs8Meter_class,
   &FileDescriptorMeter_class,
   &BlankMeter_class,
   NULL
};

static const char Platform_unsupported[] = "unsupported";

bool Platform_init(void) {
   /* no platform-specific setup needed */
   return true;
}

void Platform_done(void) {
   /* no platform-specific cleanup needed */
}

void Platform_setBindings(Htop_Action* keys) {
   /* no platform-specific key bindings */
   (void) keys;
}

int Platform_getUptime(void) {
   char buf[64];
   double uptime;
   double idle;
   ssize_t r;

   memset(buf, 0, sizeof(buf));
   r = xReadfile("/proc/uptime", buf, sizeof(buf));
   if (r < 1)
      return 0;
   uptime = 0;
   idle = 0;
   if (sscanf(buf, "%lf %lf", &uptime, &idle) < 1)
      return 0;
   return (int)uptime;
}

void Platform_getLoadAverage(double* one, double* five, double* fifteen) {
   char buf[128];
   ssize_t r;

   *one = 0;
   *five = 0;
   *fifteen = 0;

   memset(buf, 0, sizeof(buf));
   r = xReadfile("/proc/loadavg", buf, sizeof(buf));
   if (r < 1)
      return;
   sscanf(buf, "%lf %lf %lf", one, five, fifteen);
}

pid_t Platform_getMaxPid(void) {
   return INT_MAX;
}

double Platform_setCPUValues(Meter* this, unsigned int cpu) {
   const UnsupportedMachine* uhost = (const UnsupportedMachine*) this->host;
   double* v = this->values;
   double total;
   double percent;

   (void) cpu;

   v[CPU_METER_FREQUENCY] = NAN;
   v[CPU_METER_TEMPERATURE] = NAN;

   total = uhost->totalPeriod;
   if (total < 1.0) total = 1.0;

   v[CPU_METER_NICE] = 0.0;
   v[CPU_METER_NORMAL] = uhost->userPeriod / total * 100.0;
   v[CPU_METER_KERNEL] = uhost->systemPeriod / total * 100.0;
   v[CPU_METER_IRQ] = 0.0;

   this->curItems = 4;

   percent = v[CPU_METER_NORMAL] + v[CPU_METER_KERNEL];
   if (percent > 100.0) percent = 100.0;

   return percent;
}

void Platform_setMemoryValues(Meter* this) {
   const Machine* host = this->host;

   this->total = host->totalMem;
   this->values[MEMORY_METER_USED] = host->usedMem;
   this->values[MEMORY_METER_SHARED] = host->sharedMem;
   this->values[MEMORY_METER_COMPRESSED] = 0;
   this->values[MEMORY_METER_BUFFERS] = host->buffersMem;
   this->values[MEMORY_METER_CACHE] = host->cachedMem;
   this->values[MEMORY_METER_AVAILABLE] = host->availableMem;
}

void Platform_setSwapValues(Meter* this) {
   const Machine* host = this->host;

   this->total = host->totalSwap;
   this->values[SWAP_METER_USED] = host->usedSwap;
   this->values[SWAP_METER_CACHE] = host->cachedSwap;
   this->values[SWAP_METER_FRONTSWAP] = 0;
}

char* Platform_getProcessEnv(pid_t pid) {
   (void) pid;
   return NULL;
}

FileLocks_ProcessData* Platform_getProcessLocks(pid_t pid) {
   (void)pid;
   return NULL;
}

void Platform_getFileDescriptors(double* used, double* max) {
   *used = 1337;
   *max = 4711;
}

bool Platform_getDiskIO(DiskIOData* data) {
   (void)data;
   return false;
}

bool Platform_getNetworkIO(NetworkIOData* data) {
   (void)data;
   return false;
}

void Platform_getBattery(double* percent, ACPresence* isOnAC) {
   *percent = NAN;
   *isOnAC = AC_ERROR;
}

void Platform_getHostname(char* buffer, size_t size) {
   if (gethostname(buffer, size) != 0)
      String_safeStrncpy(buffer, "leb", size);
}

void Platform_getRelease(char** string) {
   *string = xStrdup(Platform_unsupported);
}
