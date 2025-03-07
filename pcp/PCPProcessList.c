/*
htop - PCPProcessList.c
(C) 2014 Hisham H. Muhammad
(C) 2020-2021 htop dev team
(C) 2020-2021 Red Hat, Inc.
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "pcp/PCPProcessList.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "Macros.h"
#include "Machine.h"
#include "Object.h"
#include "Platform.h"
#include "Process.h"
#include "Settings.h"
#include "XUtils.h"

#include "pcp/PCPMachine.h"
#include "pcp/PCPMetric.h"
#include "pcp/PCPProcess.h"


ProcessList* ProcessList_new(Machine* host, Hashtable* pidMatchList) {
   PCPProcessList* this = xCalloc(1, sizeof(PCPProcessList));
   ProcessList* super = &(this->super);

   ProcessList_init(super, Class(PCPProcess), host, pidMatchList);

   return super;
}

void ProcessList_delete(ProcessList* super) {
   PCPProcessList* this = (PCPProcessList*) super;
   ProcessList_done(super);
   free(this);
}

static inline long Metric_instance_s32(int metric, int pid, int offset, long fallback) {
   pmAtomValue value;
   if (PCPMetric_instance(metric, pid, offset, &value, PM_TYPE_32))
      return value.l;
   return fallback;
}

static inline long long Metric_instance_s64(int metric, int pid, int offset, long long fallback) {
   pmAtomValue value;
   if (PCPMetric_instance(metric, pid, offset, &value, PM_TYPE_64))
      return value.l;
   return fallback;
}

static inline unsigned long Metric_instance_u32(int metric, int pid, int offset, unsigned long fallback) {
   pmAtomValue value;
   if (PCPMetric_instance(metric, pid, offset, &value, PM_TYPE_U32))
      return value.ul;
   return fallback;
}

static inline unsigned long long Metric_instance_u64(int metric, int pid, int offset, unsigned long long fallback) {
   pmAtomValue value;
   if (PCPMetric_instance(metric, pid, offset, &value, PM_TYPE_U64))
      return value.ull;
   return fallback;
}

static inline unsigned long long Metric_instance_time(int metric, int pid, int offset) {
   pmAtomValue value;
   if (PCPMetric_instance(metric, pid, offset, &value, PM_TYPE_U64))
      return value.ull / 10;
   return 0;
}

static inline unsigned long long Metric_instance_ONE_K(int metric, int pid, int offset) {
   pmAtomValue value;
   if (PCPMetric_instance(metric, pid, offset, &value, PM_TYPE_U64))
      return value.ull / ONE_K;
   return ULLONG_MAX;
}

static inline char Metric_instance_char(int metric, int pid, int offset, char fallback) {
   pmAtomValue value;
   if (PCPMetric_instance(metric, pid, offset, &value, PM_TYPE_STRING)) {
      char uchar = value.cp[0];
      free(value.cp);
      return uchar;
   }
   return fallback;
}

static char* setUser(UsersTable* this, unsigned int uid, int pid, int offset) {
   char* name = Hashtable_get(this->users, uid);
   if (name)
      return name;

   pmAtomValue value;
   if (PCPMetric_instance(PCP_PROC_ID_USER, pid, offset, &value, PM_TYPE_STRING)) {
      Hashtable_put(this->users, uid, value.cp);
      name = value.cp;
   }
   return name;
}

static inline ProcessState PCPProcessList_getProcessState(char state) {
   switch (state) {
      case '?': return UNKNOWN;
      case 'R': return RUNNING;
      case 'W': return WAITING;
      case 'D': return UNINTERRUPTIBLE_WAIT;
      case 'P': return PAGING;
      case 'T': return STOPPED;
      case 't': return TRACED;
      case 'Z': return ZOMBIE;
      case 'X': return DEFUNCT;
      case 'I': return IDLE;
      case 'S': return SLEEPING;
      default: return UNKNOWN;
   }
}

static void PCPProcessList_updateID(Process* process, int pid, int offset) {
   process->tgid = Metric_instance_u32(PCP_PROC_TGID, pid, offset, 1);
   process->ppid = Metric_instance_u32(PCP_PROC_PPID, pid, offset, 1);
   process->state = PCPProcessList_getProcessState(Metric_instance_char(PCP_PROC_STATE, pid, offset, '?'));
}

static void PCPProcessList_updateInfo(Process* process, int pid, int offset, char* command, size_t commLen) {
   PCPProcess* pp = (PCPProcess*) process;
   pmAtomValue value;

   if (!PCPMetric_instance(PCP_PROC_CMD, pid, offset, &value, PM_TYPE_STRING))
      value.cp = xStrdup("<unknown>");
   String_safeStrncpy(command, value.cp, commLen);
   free(value.cp);

   process->pgrp = Metric_instance_u32(PCP_PROC_PGRP, pid, offset, 0);
   process->session = Metric_instance_u32(PCP_PROC_SESSION, pid, offset, 0);
   process->tty_nr = Metric_instance_u32(PCP_PROC_TTY, pid, offset, 0);
   process->tpgid = Metric_instance_u32(PCP_PROC_TTYPGRP, pid, offset, 0);
   process->minflt = Metric_instance_u32(PCP_PROC_MINFLT, pid, offset, 0);
   pp->cminflt = Metric_instance_u32(PCP_PROC_CMINFLT, pid, offset, 0);
   process->majflt = Metric_instance_u32(PCP_PROC_MAJFLT, pid, offset, 0);
   pp->cmajflt = Metric_instance_u32(PCP_PROC_CMAJFLT, pid, offset, 0);
   pp->utime = Metric_instance_time(PCP_PROC_UTIME, pid, offset);
   pp->stime = Metric_instance_time(PCP_PROC_STIME, pid, offset);
   pp->cutime = Metric_instance_time(PCP_PROC_CUTIME, pid, offset);
   pp->cstime = Metric_instance_time(PCP_PROC_CSTIME, pid, offset);
   process->priority = Metric_instance_u32(PCP_PROC_PRIORITY, pid, offset, 0);
   process->nice = Metric_instance_s32(PCP_PROC_NICE, pid, offset, 0);
   process->nlwp = Metric_instance_u32(PCP_PROC_THREADS, pid, offset, 0);
   process->starttime_ctime = Metric_instance_time(PCP_PROC_STARTTIME, pid, offset);
   process->processor = Metric_instance_u32(PCP_PROC_PROCESSOR, pid, offset, 0);

   process->time = pp->utime + pp->stime;
}

static void PCPProcessList_updateIO(PCPProcess* pp, int pid, int offset, unsigned long long now) {
   pmAtomValue value;

   pp->io_rchar = Metric_instance_ONE_K(PCP_PROC_IO_RCHAR, pid, offset);
   pp->io_wchar = Metric_instance_ONE_K(PCP_PROC_IO_WCHAR, pid, offset);
   pp->io_syscr = Metric_instance_u64(PCP_PROC_IO_SYSCR, pid, offset, ULLONG_MAX);
   pp->io_syscw = Metric_instance_u64(PCP_PROC_IO_SYSCW, pid, offset, ULLONG_MAX);
   pp->io_cancelled_write_bytes = Metric_instance_ONE_K(PCP_PROC_IO_CANCELLED, pid, offset);

   if (PCPMetric_instance(PCP_PROC_IO_READB, pid, offset, &value, PM_TYPE_U64)) {
      unsigned long long last_read = pp->io_read_bytes;
      pp->io_read_bytes = value.ull / ONE_K;
      pp->io_rate_read_bps = ONE_K * (pp->io_read_bytes - last_read) /
                                     (now - pp->io_last_scan_time);
   } else {
      pp->io_read_bytes = ULLONG_MAX;
      pp->io_rate_read_bps = NAN;
   }

   if (PCPMetric_instance(PCP_PROC_IO_WRITEB, pid, offset, &value, PM_TYPE_U64)) {
      unsigned long long last_write = pp->io_write_bytes;
      pp->io_write_bytes = value.ull;
      pp->io_rate_write_bps = ONE_K * (pp->io_write_bytes - last_write) /
                                      (now - pp->io_last_scan_time);
   } else {
      pp->io_write_bytes = ULLONG_MAX;
      pp->io_rate_write_bps = NAN;
   }

   pp->io_last_scan_time = now;
}

static void PCPProcessList_updateMemory(PCPProcess* pp, int pid, int offset) {
   pp->super.m_virt = Metric_instance_u32(PCP_PROC_MEM_SIZE, pid, offset, 0);
   pp->super.m_resident = Metric_instance_u32(PCP_PROC_MEM_RSS, pid, offset, 0);
   pp->m_share = Metric_instance_u32(PCP_PROC_MEM_SHARE, pid, offset, 0);
   pp->m_trs = Metric_instance_u32(PCP_PROC_MEM_TEXTRS, pid, offset, 0);
   pp->m_lrs = Metric_instance_u32(PCP_PROC_MEM_LIBRS, pid, offset, 0);
   pp->m_drs = Metric_instance_u32(PCP_PROC_MEM_DATRS, pid, offset, 0);
   pp->m_dt = Metric_instance_u32(PCP_PROC_MEM_DIRTY, pid, offset, 0);
}

static void PCPProcessList_updateSmaps(PCPProcess* pp, pid_t pid, int offset) {
   pp->m_pss = Metric_instance_u64(PCP_PROC_SMAPS_PSS, pid, offset, 0);
   pp->m_swap = Metric_instance_u64(PCP_PROC_SMAPS_SWAP, pid, offset, 0);
   pp->m_psswp = Metric_instance_u64(PCP_PROC_SMAPS_SWAPPSS, pid, offset, 0);
}

static void PCPProcessList_readOomData(PCPProcess* pp, int pid, int offset) {
   pp->oom = Metric_instance_u32(PCP_PROC_OOMSCORE, pid, offset, 0);
}

static void PCPProcessList_readAutogroup(PCPProcess* pp, int pid, int offset) {
   pp->autogroup_id = Metric_instance_s64(PCP_PROC_AUTOGROUP_ID, pid, offset, -1);
   pp->autogroup_nice = Metric_instance_s32(PCP_PROC_AUTOGROUP_NICE, pid, offset, 0);
}

static void PCPProcessList_readCtxtData(PCPProcess* pp, int pid, int offset) {
   pmAtomValue value;
   unsigned long ctxt = 0;

   if (PCPMetric_instance(PCP_PROC_VCTXSW, pid, offset, &value, PM_TYPE_U32))
      ctxt += value.ul;
   if (PCPMetric_instance(PCP_PROC_NVCTXSW, pid, offset, &value, PM_TYPE_U32))
      ctxt += value.ul;

   pp->ctxt_diff = ctxt > pp->ctxt_total ? ctxt - pp->ctxt_total : 0;
   pp->ctxt_total = ctxt;
}

static char* setString(PCPMetric metric, int pid, int offset, char* string) {
   if (string)
      free(string);
   pmAtomValue value;
   if (PCPMetric_instance(metric, pid, offset, &value, PM_TYPE_STRING))
      string = value.cp;
   else
      string = NULL;
   return string;
}

static void PCPProcessList_updateTTY(Process* process, int pid, int offset) {
   process->tty_name = setString(PCP_PROC_TTYNAME, pid, offset, process->tty_name);
}

static void PCPProcessList_readCGroups(PCPProcess* pp, int pid, int offset) {
   pp->cgroup = setString(PCP_PROC_CGROUPS, pid, offset, pp->cgroup);
}

static void PCPProcessList_readSecattrData(PCPProcess* pp, int pid, int offset) {
   pp->secattr = setString(PCP_PROC_LABELS, pid, offset, pp->secattr);
}

static void PCPProcessList_readCwd(PCPProcess* pp, int pid, int offset) {
   pp->super.procCwd = setString(PCP_PROC_CWD, pid, offset, pp->super.procCwd);
}

static void PCPProcessList_updateUsername(Process* process, int pid, int offset, UsersTable* users) {
   process->st_uid = Metric_instance_u32(PCP_PROC_ID_UID, pid, offset, 0);
   process->user = setUser(users, process->st_uid, pid, offset);
}

static void PCPProcessList_updateCmdline(Process* process, int pid, int offset, const char* comm) {
   pmAtomValue value;
   if (!PCPMetric_instance(PCP_PROC_PSARGS, pid, offset, &value, PM_TYPE_STRING)) {
      if (process->state != ZOMBIE)
         process->isKernelThread = true;
      Process_updateCmdline(process, NULL, 0, 0);
      return;
   }

   char* command = value.cp;
   int length = strlen(command);
   if (command[0] != '(') {
      process->isKernelThread = false;
   } else {
      ++command;
      --length;
      if (command[length - 1] == ')')
         command[--length] = '\0';
      process->isKernelThread = true;
   }

   int tokenStart = 0;
   for (int i = 0; i < length; i++) {
      /* htop considers the next character after the last / that is before
       * basenameOffset, as the start of the basename in cmdline - see
       * Process_writeCommand */
      if (command[i] == '/')
         tokenStart = i + 1;
   }
   int tokenEnd = length;

   Process_updateCmdline(process, command, tokenStart, tokenEnd);
   free(value.cp);

   Process_updateComm(process, comm);

   if (PCPMetric_instance(PCP_PROC_EXE, pid, offset, &value, PM_TYPE_STRING)) {
      Process_updateExe(process, value.cp[0] ? value.cp : NULL);
      free(value.cp);
   }
}

static bool PCPProcessList_updateProcesses(PCPProcessList* this) {
   ProcessList* pl = (ProcessList*) this;
   Machine* host = pl->host;
   PCPMachine* phost = (PCPMachine*) host;

   const Settings* settings = host->settings;
   bool hideKernelThreads = settings->hideKernelThreads;
   bool hideUserlandThreads = settings->hideUserlandThreads;
   uint32_t flags = settings->ss->flags;

   unsigned long long now = (unsigned long long)(phost->timestamp * 1000);
   int pid = -1, offset = -1;

   /* for every process ... */
   while (PCPMetric_iterate(PCP_PROC_PID, &pid, &offset)) {

      bool preExisting;
      Process* proc = ProcessList_getProcess(pl, pid, &preExisting, PCPProcess_new);
      PCPProcess* pp = (PCPProcess*) proc;
      PCPProcessList_updateID(proc, pid, offset);
      proc->isUserlandThread = proc->pid != proc->tgid;
      pp->offset = offset >= 0 ? offset : 0;

      /*
       * These conditions will not trigger on first occurrence, cause we need to
       * add the process to the ProcessList and do all one time scans
       * (e.g. parsing the cmdline to detect a kernel thread)
       * But it will short-circuit subsequent scans.
       */
      if (preExisting && hideKernelThreads && Process_isKernelThread(proc)) {
         proc->updated = true;
         proc->show = false;
         if (proc->state == RUNNING)
            pl->runningTasks++;
         pl->kernelThreads++;
         pl->totalTasks++;
         continue;
      }
      if (preExisting && hideUserlandThreads && Process_isUserlandThread(proc)) {
         proc->updated = true;
         proc->show = false;
         if (proc->state == RUNNING)
            pl->runningTasks++;
         pl->userlandThreads++;
         pl->totalTasks++;
         continue;
      }

      if (flags & PROCESS_FLAG_IO)
         PCPProcessList_updateIO(pp, pid, offset, now);

      PCPProcessList_updateMemory(pp, pid, offset);

      if ((flags & PROCESS_FLAG_LINUX_SMAPS) &&
          (Process_isKernelThread(proc) == false)) {
         if (PCPMetric_enabled(PCP_PROC_SMAPS_PSS))
            PCPProcessList_updateSmaps(pp, pid, offset);
      }

      char command[MAX_NAME + 1];
      unsigned int tty_nr = proc->tty_nr;
      unsigned long long int lasttimes = pp->utime + pp->stime;

      PCPProcessList_updateInfo(proc, pid, offset, command, sizeof(command));
      proc->starttime_ctime += Platform_getBootTime();
      if (tty_nr != proc->tty_nr)
         PCPProcessList_updateTTY(proc, pid, offset);

      float percent_cpu = (pp->utime + pp->stime - lasttimes) / phost->period * 100.0;
      proc->percent_cpu = isnan(percent_cpu) ?
                          0.0 : CLAMP(percent_cpu, 0.0, host->activeCPUs * 100.0);
      proc->percent_mem = proc->m_resident / (double) host->totalMem * 100.0;
      Process_updateCPUFieldWidths(proc->percent_cpu);

      PCPProcessList_updateUsername(proc, pid, offset, host->usersTable);

      if (!preExisting) {
         PCPProcessList_updateCmdline(proc, pid, offset, command);
         Process_fillStarttimeBuffer(proc);
         ProcessList_add(pl, proc);
      } else if (settings->updateProcessNames && proc->state != ZOMBIE) {
         PCPProcessList_updateCmdline(proc, pid, offset, command);
      }

      if (flags & PROCESS_FLAG_LINUX_CGROUP)
         PCPProcessList_readCGroups(pp, pid, offset);

      if (flags & PROCESS_FLAG_LINUX_OOM)
         PCPProcessList_readOomData(pp, pid, offset);

      if (flags & PROCESS_FLAG_LINUX_CTXT)
         PCPProcessList_readCtxtData(pp, pid, offset);

      if (flags & PROCESS_FLAG_LINUX_SECATTR)
         PCPProcessList_readSecattrData(pp, pid, offset);

      if (flags & PROCESS_FLAG_CWD)
         PCPProcessList_readCwd(pp, pid, offset);

      if (flags & PROCESS_FLAG_LINUX_AUTOGROUP)
         PCPProcessList_readAutogroup(pp, pid, offset);

      if (proc->state == ZOMBIE && !proc->cmdline && command[0]) {
         Process_updateCmdline(proc, command, 0, strlen(command));
      } else if (Process_isThread(proc)) {
         if ((settings->showThreadNames || Process_isKernelThread(proc)) && command[0]) {
            Process_updateCmdline(proc, command, 0, strlen(command));
         }

         if (Process_isKernelThread(proc)) {
            pl->kernelThreads++;
         } else {
            pl->userlandThreads++;
         }
      }

      /* Set at the end when we know if a new entry is a thread */
      proc->show = ! ((hideKernelThreads && Process_isKernelThread(proc)) ||
                      (hideUserlandThreads && Process_isUserlandThread(proc)));

      pl->totalTasks++;
      if (proc->state == RUNNING)
         pl->runningTasks++;
      proc->updated = true;
   }
   return true;
}

void ProcessList_goThroughEntries(ProcessList* super) {
   PCPProcessList* this = (PCPProcessList*) super;
   PCPProcessList_updateProcesses(this);
}
