//
// thread.c
//
// Thread routines
//
// Copyright (C) 2002 Michael Ringgaard. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 
// 1. Redistributions of source code must retain the above copyright 
//    notice, this list of conditions and the following disclaimer.  
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.  
// 3. Neither the name of the project nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission. 
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
// SUCH DAMAGE.
// 

#include <os.h>
#include <string.h>
#include <atomic.h>

#include <os/syscall.h>
#include <os/seg.h>
#include <os/tss.h>
#include <os/syspage.h>

struct critsect job_lock;

void init_threads(hmodule_t hmod)
{
  struct tib *tib = gettib();
  struct job *job;

  mkcs(&job_lock);

  job = malloc(sizeof(struct job));
  if (!job) panic("unable to allocate initial job");
  memset(job, 0, sizeof(struct job));

  job->threadcnt = 1;
  job->terminated = mkevent(1, 0);
  job->in = 0;
  job->out = 1;
  job->err = 2;
  job->termtype = TERM_CONSOLE;
  job->hmod = hmod;
  job->cmdline = NULL;

  tib->job = job;
  peb->firstjob = peb->lastjob = job;
}

handle_t mkthread(void (__stdcall *startaddr)(struct tib *), unsigned long stacksize, struct tib **ptib)
{
  return syscall(SYSCALL_MKTHREAD, &startaddr);
}

void __stdcall threadstart(struct tib *tib)
{
  // Set usermode segment selectors
  __asm
  {
    mov	ax, SEL_UDATA + SEL_RPL3
    mov	ds, ax
    mov	es, ax
  }

  // Call thread routine
  ((void (__stdcall *)(void *)) (tib->startaddr))(tib->startarg);
  endthread(0);
}

handle_t beginthread(void (__stdcall *startaddr)(void *), unsigned stacksize, void *arg, int flags, struct tib **ptib)
{
  struct tib *tib;
  struct job *job;
  handle_t h;

  h = mkthread(threadstart, stacksize, &tib);
  if (h < 0) return h;

  tib->startaddr = (void *) startaddr;
  tib->startarg = arg;

  if (flags & CREATE_NEW_JOB)
  {
    job = malloc(sizeof(struct job));
    if (!job) 
    {
      errno = ENOMEM;
      return -1;
    }
    memset(job, 0, sizeof(struct job));

    if (flags & CREATE_DETACHED)
    {
      job->in = job->out = job->err = -EBADF;
    }
    else
    {
      struct job *parent = gettib()->job;

      job->in = dup(parent->in);
      job->out = dup(parent->out);
      job->err = dup(parent->err);
      job->termtype = parent->termtype;
    }

    job->terminated = mkevent(1, 0);

    enter(&job_lock);
    job->prevjob = peb->lastjob;
    if (!peb->firstjob) peb->firstjob = job;
    if (peb->lastjob) peb->lastjob->nextjob = job;
    peb->lastjob = job;
    leave(&job_lock);
  }
  else
    job = gettib()->job;

  atomic_add(&job->threadcnt, 1);
  tib->job = job;

  if (!(flags & CREATE_SUSPENDED)) resume(h);

  if (ptib) *ptib = tib;
  return h;
}

handle_t self()
{
  //return syscall(SYSCALL_SELF, NULL);
  return gettib()->hndl;
}

int suspend(handle_t thread)
{
  return syscall(SYSCALL_SUSPEND, &thread);
}

int resume(handle_t thread)
{
  return syscall(SYSCALL_RESUME, &thread);
}

void endthread(int retval)
{
  struct job *job;

  job = gettib()->job;
  if (atomic_add(&job->threadcnt, -1) == 0)
  {
    enter(&job_lock);
    if (job->nextjob) job->nextjob->prevjob = job->prevjob;
    if (job->prevjob) job->prevjob->nextjob = job->nextjob;
    if (job == peb->firstjob) peb->firstjob = job->nextjob;
    if (job == peb->lastjob) peb->lastjob = job->prevjob;
    leave(&job_lock);

    if (job->in >= 0) close(job->in);
    if (job->out >= 0) close(job->out);
    if (job->err >= 0) close(job->err);

    if (job->hmod) dlclose(job->hmod);
    if (job->cmdline) free(job->cmdline);

    eset(job->terminated);
    close(job->terminated);

    free(job);
  }

  syscall(SYSCALL_ENDTHREAD, &retval);
}

tid_t gettid()
{
  return gettib()->tid;
}

int getcontext(handle_t thread, void *context)
{
  return syscall(SYSCALL_GETCONTEXT, &thread);
}

int setcontext(handle_t thread, void *context)
{
  return syscall(SYSCALL_SETCONTEXT, &thread);
}

int getprio(handle_t thread)
{
  return syscall(SYSCALL_GETPRIO, &thread);
}

int setprio(handle_t thread, int priority)
{
  return syscall(SYSCALL_SETPRIO, &thread);
}

void sleep(int millisecs)
{
  syscall(SYSCALL_SLEEP, &millisecs);
}

struct tib *gettib()
{
  struct tib *tib;

  __asm
  {
    mov eax, fs:[TIB_SELF_OFFSET]
    mov [tib], eax
  }

  return tib;
}

void exit(int status)
{
  struct job *job = gettib()->job;

  if (job->atexit) job->atexit();
  if (job->exitcodeptr) *job->exitcodeptr = status;
  eset(job->terminated);
  endthread(status);
}

static void __stdcall spawn_program(void *args)
{
  struct job *job = gettib()->job;
  int rc;

  rc = exec(job->hmod, job->cmdline);
  exit(rc);
}

int spawn(int mode, const char *pgm, const char *cmdline, struct tib **tibptr)
{
  hmodule_t hmod;
  handle_t hthread;
  struct tib *tib;
  struct job *job;
  int rc;

  hmod = dlopen(pgm, 0);
  if (!hmod) return -1;

  hthread = beginthread(spawn_program, 0, NULL, CREATE_SUSPENDED | CREATE_NEW_JOB | ((mode & P_DETACH) ? CREATE_DETACHED : 0), &tib);
  if (hthread < 0)
  {
    dlclose(hmod);
    return -1;
  }

  job = tib->job;
  job->hmod = hmod;
  job->cmdline = strdup(cmdline);

  if (mode & P_NOWAIT)
  {
    if (tibptr) *tibptr = tib;
    resume(hthread);
    return hthread;
  }
  else
  {
    int exitcode;
    handle_t terminated;
    
    terminated = dup(job->terminated);
    if (terminated < 0) return -1;

    job->exitcodeptr = &exitcode;
    
    rc = resume(hthread);
    if (rc < 0) return rc;

    rc = wait(terminated, INFINITE);
    if (rc < 0) return -1;

    close(terminated);
    close(hthread);

    return exitcode;
  }
}
