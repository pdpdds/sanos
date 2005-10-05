//
// os.c
//
// Operating system API
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
#include <inifile.h>
#include <moddb.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <verinfo.h>

#include <os/seg.h>
#include <os/tss.h>
#include <os/syspage.h>
#include <os/pe.h>
#include <os/syscall.h>

#include "heap.h"
#include "resolv.h"

struct critsect heap_lock;
struct critsect mod_lock;
struct section *osconfig;
struct moddb usermods;
struct peb *peb;

struct term console = {TERM_CONSOLE, 80, 25};

int sprintf(char *buf, const char *fmt, ...);

void init_sntpd();
void init_threads(hmodule_t hmod, struct term *initterm);
void init_userdb();

void start_syslog();
void stop_syslog();

void globalhandler(int signum, struct siginfo *info);

void panic(const char *msg)
{
  syslog(LOG_CRIT, "panic: %s", msg);
  exit(3);
}

int *_errno()
{
  return &(gettib()->errnum);
}

int *_fmode()
{
  return &peb->fmodeval;
}

static int check_access(struct stat64 *st, int mode)
{
  int uid = getuid();
  
  if (uid == 0) return 0;

  if (uid == st->st_uid)
    mode <<= 6;
  else if (getgid() == st->st_gid)
    mode <<= 3;

  if ((mode && st->st_mode) == 0)
  {
    errno = EACCES;
    return -1;
  }

  return 0;
}

handle_t creat(const char *name, int mode)
{
  return open(name, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

handle_t sopen(const char *name, int flags, int shflags, ...)
{
  va_list args;
  int mode = 0;

  if (flags & O_CREAT)
  {
    va_start(args, shflags);
    mode = va_arg(args, int);
    va_end(args);
  }

  return open(name, FILE_FLAGS(flags, shflags), mode);
}

int eof(handle_t f)
{
  return tell64(f) == fstat64(f, NULL);
}

int umask(int mask)
{
  int oldmask;

  mask &= S_IRWXUGO;
  oldmask = peb->umaskval;
  peb->umaskval = mask;
  return oldmask;
}

void *malloc(size_t size)
{
  void *p;

  //syslog(LOG_MODULE | LOG_DEBUG, "malloc %d bytes", size);

  enter(&heap_lock);
  p = heap_alloc(size);
  leave(&heap_lock);

  if (size && !p) panic("malloc: out of memory");
  //if (size && !p) errno = ENOMEM;
  //syslog(LOG_MODULE | LOG_DEBUG, "malloced %d bytes at %p", size, p);

  return p;
}

void *realloc(void *mem, size_t size)
{
  void *p;

  enter(&heap_lock);
  p = heap_realloc(mem, size);
  leave(&heap_lock);

  if (size && !p) panic("realloc: out of memory");
  //if (size && !p) errno = ENOMEM;

  return p;
}

void *calloc(size_t num, size_t size)
{
  void *p;

  enter(&heap_lock);
  p = heap_calloc(num, size);
  leave(&heap_lock);

  if (size * num != 0 && !p) panic("calloc: out of memory");
  //if (size * num != 0 && !p) errno = ENOMEM;

  return p;
}

void free(void *p)
{
  enter(&heap_lock);
  heap_free(p);
  leave(&heap_lock);
}

struct mallinfo mallinfo()
{
  struct mallinfo m;

  enter(&heap_lock);
  m = heap_mallinfo();
  leave(&heap_lock);

  return m;
}

int canonicalize(const char *filename, char *buffer, int size)
{
  char *p;
  char *end;
  int len;

  // Check for maximum filename length
  if (!filename) 
  {
    errno = EINVAL;
    return -1;
  }

  // Remove drive letter from filename (e.g. c:)
  if (filename[0] != 0 && filename[1] == ':') filename += 2;

  // Initialize buffer
  p = buffer;
  end = buffer + size;

  // Add current directory to filename if relative path
  if (*filename != PS1 && *filename != PS2)
  {
    // Do not add current directory if it is root directory
    len = strlen(peb->curdir);
    if (len > 1)
    {
      memcpy(p, peb->curdir, len);
      p += len;
    }
  }

  while (*filename)
  {
    // Parse path separator
    if (*filename == PS1 || *filename == PS2) filename++;
    if (p == end) 
    {
      errno = ENAMETOOLONG;
      return -1;
    }
    *p++ = peb->pathsep;

    // Parse next name part in path
    len = 0;
    while (*filename && *filename != PS1 && *filename != PS2)
    {
      // We do not allow control characters in filenames
      if (*filename > 0 && *filename < ' ') 
      {
	errno = EINVAL;
	return -1;
      }
      if (p == end) 
      {
	errno = ENAMETOOLONG;
	return -1;
      }
      *p++ = *filename++;
      len++;
    }

    // Handle empty name parts and '.' and '..'
    if (len == 0)
      p--;
    if (len == 1 && filename[-1] == '.')
      p -= 2;
    else if (len == 2 && filename[-1] == '.' && filename[-2] == '.')
    {
      p -= 4;
      if (p < buffer) 
      {
	errno = EINVAL;
	return -1;
      }
      while (*p != PS1) p--;
    }
  }

  // Convert empty filename to /
  if (p == buffer) *p++ = peb->pathsep;

  // Terminate string
  if (p == end) 
  {
    errno = ENAMETOOLONG;
    return -1;
  }

  *p = 0;

  return p - buffer;
}

char *getcwd(char *buf, size_t size)
{
  size_t len;

  len = strlen(peb->curdir);

  if (buf)
  {
    if (len >= size)
    {
      errno = ERANGE;
      return NULL;
    }
  }
  else
  {
    if (size == 0)
      size = len + 1;
    else if (len >= size)
    {
      errno = ERANGE;
      return NULL;
    }

    buf = malloc(size);
    if (!buf) 
    {
      errno = ENOMEM;
      return NULL;
    }
  }

  memcpy(buf, peb->curdir, len + 1);
  return buf;
}

static void *load_image(char *filename)
{
  handle_t f;
  char *buffer;
  char *imgbase;
  struct dos_header *doshdr;
  struct image_header *imghdr;
  int i;
  unsigned int bytes;

  // Allocate header buffer
  buffer = malloc(PAGESIZE);
  if (!buffer) return NULL;
  memset(buffer, 0, PAGESIZE);

  // Open file
  f = open(filename, O_RDONLY | O_BINARY);
  if (f < 0) 
  {
    free(buffer);
    return NULL;
  }

  // Read headers
  if ((bytes = read(f, buffer, PAGESIZE)) < 0)
  {
    close(f);
    free(buffer);
    return NULL;
  }
  
  doshdr = (struct dos_header *) buffer;
  imghdr = (struct image_header *) (buffer + doshdr->e_lfanew);

  // Check PE file signature
  if (doshdr->e_lfanew > bytes || imghdr->signature != IMAGE_PE_SIGNATURE) 
  {
    close(f);
    free(buffer);
    return NULL;
  }

  // Check alignment
  //if (imghdr->optional.file_alignment != PAGESIZE || imghdr->optional.section_alignment != PAGESIZE) panic("image not page aligned");

  // Allocate memory for module
  imgbase = (char *) mmap(NULL, imghdr->optional.size_of_image, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE, 'UMOD');
  if (imgbase == NULL)
  {
    close(f);
    free(buffer);
    return NULL;
  }

  // copy header to image
  memcpy(imgbase, buffer, PAGESIZE);

  // Read sections
  for (i = 0; i < imghdr->header.number_of_sections; i++)
  {
    if (imghdr->sections[i].pointer_to_raw_data != 0)
    {
      if (lseek(f, imghdr->sections[i].pointer_to_raw_data, SEEK_SET) != imghdr->sections[i].pointer_to_raw_data)
      {
	munmap(imgbase, imghdr->optional.size_of_image, MEM_RELEASE);
	close(f);
        free(buffer);
	return NULL;
      }

      if (read(f, RVA(imgbase, imghdr->sections[i].virtual_address), imghdr->sections[i].size_of_raw_data) != (int) imghdr->sections[i].size_of_raw_data)
      {
	munmap(imgbase, imghdr->optional.size_of_image, MEM_RELEASE);
	close(f);
        free(buffer);
	return NULL;
      }
    }
  }

  //syslog(LOG_MODULE | LOG_DEBUG, "image %s loaded at %p (%d KB)", filename, imgbase, imghdr->optional.size_of_image / 1024);

  // Close file
  close(f);
  free(buffer);

  return imgbase;
}

static int unload_image(hmodule_t hmod, size_t size)
{
  return munmap(hmod, size, MEM_RELEASE);
}

static int protect_region(void *mem, size_t size, int protect)
{
  return mprotect(mem, size, protect);
}

static void logldr(char *msg)
{
  syslog(LOG_MODULE | LOG_DEBUG, "mod: %s", msg);
}

void *dlsym(hmodule_t hmod, const char *procname)
{
  void *addr;

  enter(&mod_lock);
  addr = get_proc_address(hmod, (char *) procname);
  leave(&mod_lock);
  return addr;
}

hmodule_t getmodule(const char *name)
{
  hmodule_t hmod;

  if (name == NULL) return gettib()->job->hmod;

  enter(&mod_lock);
  hmod = get_module_handle(&usermods, (char *) name);
  leave(&mod_lock);
  return hmod;
}

int getmodpath(hmodule_t hmod, char *buffer, int size)
{
  int rc;

  if (hmod == NULL) hmod = gettib()->job->hmod;

  enter(&mod_lock);
  rc = get_module_filename(&usermods, hmod, buffer, size);
  leave(&mod_lock);
  
  if (rc < 0)
  {
    errno = -rc;
    return -1;
  }

  return 0;
}

hmodule_t dlopen(const char *name, int mode)
{
  hmodule_t hmod;

  enter(&mod_lock);
  hmod = load_module(&usermods, (char *) name, 0);
  leave(&mod_lock);

  return hmod;
}

int dlclose(hmodule_t hmod)
{
  int rc;

  enter(&mod_lock);
  rc = unload_module(&usermods, hmod);
  leave(&mod_lock);
  return rc;
}

char *dlerror()
{
  return strerror(errno);
}

int exec(hmodule_t hmod, const char *args)
{
  int rc;

  if (get_image_header(hmod)->header.characteristics & IMAGE_FILE_DLL) return -ENOEXEC;

  rc = ((int (*)(hmodule_t, char *, int)) get_entrypoint(hmod))(hmod, (char *) args, 0);

  return rc;
}

void *getresdata(hmodule_t hmod, int type, char *name, int lang, int *len)
{
  void *data;
  int rc;

  if (hmod == NULL) hmod = gettib()->job->hmod;

  rc = get_resource_data(hmod, INTRES(type), name, INTRES(lang), &data);
  if (rc < 0)
  {
    errno = -rc;
    return NULL;
  }

  if (len) *len = rc;
  return data;
}

int getreslen(hmodule_t hmod, int type, char *name, int lang)
{
  void *data;
  int rc;

  if (hmod == NULL) hmod = gettib()->job->hmod;

  rc = get_resource_data(hmod, INTRES(type), name, INTRES(lang), &data);
  if (rc < 0)
  {
    errno = -rc;
    return rc;
  }

  return rc;
}

struct verinfo *getverinfo(hmodule_t hmod)
{
  struct verinfo *ver;

  if (hmod == NULL) hmod = gettib()->job->hmod;
  ver = get_version_info(hmod);
  if (ver == NULL) errno = ENOENT;
  return ver;
}

int getvervalue(hmodule_t hmod, char *name, char *buf, int size)
{
  int rc;

  if (hmod == NULL) hmod = gettib()->job->hmod;
  rc = get_version_value(hmod, name, buf, size);
  if (rc < 0)
  {
    errno = -rc;
    return -1;
  }

  return rc;
}

int uname(struct utsname *buf)
{
  struct cpuinfo cpu;
  char machine[8];
  struct verinfo *ver;
  int osflags;
  char *build;
  struct tm tm;

  if (!buf)
  {
    errno = EINVAL;
    return -1;
  }

  if (sysinfo(SYSINFO_CPU, &cpu, sizeof(struct cpuinfo)) < 0) return -1;
  machine[0] = 'i';
  machine[1] = '0' + cpu.cpu_family;
  machine[2] = '8';
  machine[3] = '6';
  machine[4] = 0;

  osflags = peb->osversion.file_flags;
  if (osflags & VER_FLAG_PRERELEASE) 
    build = "prerelease ";
  else if (osflags & VER_FLAG_PATCHED) 
    build = "patch ";
  else if (osflags & VER_FLAG_PRIVATEBUILD) 
    build = "private ";
  else if (osflags & VER_FLAG_DEBUG) 
    build = "debug ";
  else
    build = "";

  gmtime_r(&peb->ostimestamp, &tm);
  ver = &peb->osversion;

  memset(buf, 0, sizeof(struct utsname));
  strncpy(buf->sysname, peb->osname, UTSNAMELEN);
  gethostname(buf->nodename, UTSNAMELEN);
  sprintf(buf->release, "%d.%d.%d.%d", ver->file_major_version, ver->file_minor_version, ver->file_release_number, ver->file_build_number);
  sprintf(buf->version, "%s%04d-%02d-%02d %02d:%02d:%02d", build, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  strncpy(buf->machine, machine, UTSNAMELEN);

  return 0;
}

void dbgbreak()
{
  __asm { int 3 };
}

unsigned sleep(unsigned seconds)
{
  msleep(seconds * 1000);
  return 0;
}

char *crypt(const char *key, const char *salt)
{
  return crypt_r(key, salt, gettib()->cryptbuf);
}

void init_net()
{
  struct section *sect;
  struct property *prop;
  struct ifcfg ifcfg;
  struct sockaddr_in *sin;
  char str[256];
  int first;
  int rc;
  int sock;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return;

  sect = find_section(osconfig, "netif");
  if (!sect) return;

  first = 1;
  prop = sect->properties;
  while (prop)
  {
    memset(&ifcfg, 0, sizeof(ifcfg));

    strcpy(ifcfg.name, prop->name);

    if (get_option(prop->value, "ip", str, sizeof str, NULL))
    {
      sin = (struct sockaddr_in *) &ifcfg.addr;
      sin->sin_family = AF_INET;
      sin->sin_addr.s_addr = inet_addr(str);
    }
    else
      ifcfg.flags |= IFCFG_DHCP;

    if (get_option(prop->value, "gw", str, sizeof str, NULL))
    {
      sin = (struct sockaddr_in *) &ifcfg.gw;
      sin->sin_family = AF_INET;
      sin->sin_addr.s_addr = inet_addr(str);
    }

    if (get_option(prop->value, "mask", str, sizeof str, NULL))
    {
      sin = (struct sockaddr_in *) &ifcfg.netmask;
      sin->sin_family = AF_INET;
      sin->sin_addr.s_addr = inet_addr(str);
    }

    if (get_option(prop->value, "broadcast", str, sizeof str, NULL))
    {
      sin = (struct sockaddr_in *) &ifcfg.broadcast;
      sin->sin_family = AF_INET;
      sin->sin_addr.s_addr = inet_addr(str);
    }

    ifcfg.flags |= IFCFG_UP;
    if (first) ifcfg.flags |= IFCFG_DEFAULT;

    rc = ioctl(sock, SIOIFCFG, &ifcfg, sizeof(struct ifcfg));
    if (rc < 0)
      syslog(LOG_ERR, "%s: unable to configure net interface, %s", ifcfg.name, strerror(errno));
    else
    {
      unsigned long addr = ((struct sockaddr_in *) &ifcfg.addr)->sin_addr.s_addr;
      unsigned long gw = ((struct sockaddr_in *) &ifcfg.gw)->sin_addr.s_addr;
      unsigned long mask = ((struct sockaddr_in *) &ifcfg.netmask)->sin_addr.s_addr;
      unsigned long bcast = ((struct sockaddr_in *) &ifcfg.broadcast)->sin_addr.s_addr;

      //syslog(LOG_INFO, "%s: addr %a mask %a gw %a bcast %a", ifcfg.name, &addr, &mask, &gw, &bcast);

      if (first) peb->ipaddr.s_addr = addr;
    }

    prop = prop->next;
    first = 0;
  }

  close(sock);
}

void init_hostname()
{
  struct hostent *hp;
  char *dot;
  int len;

  // Get hostname and domain from os config
  if (!*peb->hostname)
  {
    char *host = get_property(osconfig, "os", "hostname", NULL);
    if (host) strcpy(peb->hostname, host);
  }

  if (!*peb->default_domain)
  {
    char *domain = get_property(osconfig, "dns", "domain", NULL);
    if (domain) strcpy(peb->default_domain, domain);
  }

  // Check for hostname already set by configuration or DHCP
  if (*peb->hostname) return;

  // Check for any IP address configured
  if (peb->ipaddr.s_addr == INADDR_ANY) return;

  // Try to lookup hostname from the ip address using DNS
  hp = gethostbyaddr((char *) &peb->ipaddr, sizeof(struct in_addr), AF_INET);
  if (!hp || !hp->h_name || !*hp->h_name) return;

  // Check that domain name matches
  dot = strchr(hp->h_name, '.');
  if (!dot) return;
  if (strcmp(dot + 1, peb->default_domain) != 0) return;

  // Copy hostname from DNS to PEB
  len = dot - hp->h_name;
  if (len >= sizeof(peb->hostname)) return;
  memcpy(peb->hostname, hp->h_name, len);
  peb->hostname[len] = 0;
}

void init_mount()
{
  struct section *sect;
  struct property *prop;
  char devname[256];
  char *type;
  char *opts;
  int rc;

  sect = find_section(osconfig, "mount");
  if (!sect) return;

  prop = sect->properties;
  while (prop)
  {
    strcpy(devname, prop->value ? prop->value : "");
    type = strchr(devname, ',');
    if (type)
    {
      *type++ = 0;
      while (*type == ' ') type++;
      opts = strchr(type, ',');
      if (opts)
      {
	*opts++ = 0;
	while (*opts == ' ') opts++;
      }
      else
	opts = NULL;
    }
    else
    {
      type = "dfs";
      opts = NULL;
    }

    //syslog(LOG_DEBUG, "mount %s on %s type %s opts %s", devname, prop->name, type, opts);

    rc = mount(type, prop->name, devname, opts);
    if (rc < 0) syslog(LOG_ERR, "%s: error %d mounting %s %s", prop->name, errno, type, devname);

    prop = prop->next;
  }
}

int seed_random_device(char *rndfn)
{
  int rndfile;
  int rnddev;
  char buf[512];
  int n;

  rnddev = open("/dev/random", O_WRONLY | O_BINARY);
  if (rnddev < 0) return rnddev;

  rndfile = open(rndfn, O_BINARY);
  if (rndfile < 0)
  {
    close(rnddev);
    return -1;
  }

  n = read(rndfile, buf, sizeof buf);
  if (n > 0) write(rnddev, buf, n);

  close(rndfile);
  close(rnddev);

  return 0;
}

int save_random_device(char *rndfn)
{
  int rndfile;
  int rnddev;
  char buf[512];
  int n;

  rnddev = open("/dev/urandom", O_BINARY);
  if (rnddev < 0) return -1;

  rndfile = open(rndfn, O_CREAT | O_BINARY);
  if (rndfile < 0)
  {
    close(rnddev);
    return -1;
  }

  n = read(rnddev, buf, sizeof buf);
  if (n > 0) write(rndfile, buf, n);

  close(rndfile);
  close(rnddev);

  return 0;
}

int __stdcall start(hmodule_t hmod, void *reserved, void *reserved2)
{
  char *init;
  int rc;
  char *rndfn;

  // Set usermode segment selectors
  __asm
  {
    mov	ax, SEL_UDATA + SEL_RPL3
    mov	ds, ax
    mov	es, ax
  }

  // Setup pointer to process environment block (PEB)
  peb = (struct peb *) PEB_ADDRESS;
  peb->globalhandler = globalhandler;
  peb->fmodeval = O_BINARY; //O_TEXT;
#if DEBUG
  peb->debug = 1;
#endif

  // Initialize heap and module locks
  mkcs(&heap_lock);
  mkcs(&mod_lock);

  // Initialize initial job
  init_threads(hmod, &console);

  // Load configuration file
  osconfig = read_properties("/etc/os.ini");
  if (!osconfig) panic("error reading /etc/os.ini");
  peb->debug = get_numeric_property(osconfig, "os", "debug", peb->debug);

  // Initialize network interfaces
  init_net();

  // Initialize resolver
  res_init();

  // Determine hostname
  init_hostname();

  // Initialize NTP daemon
  init_sntpd();

  // Initialize user module database
  peb->usermods = &usermods;
  usermods.load_image = load_image;
  usermods.unload_image = unload_image;
  usermods.protect_region = protect_region;
  usermods.log = logldr;

  init_module_database(&usermods, "os.dll", hmod, get_property(osconfig, "os", "libpath", "/bin"), find_section(osconfig, "modaliases"), 0);

  // Load user database
  init_userdb();
  initgroups("root", 0);

  // Mount devices
  init_mount();

  // Initialize log
  start_syslog();

  // Initialize random device with seed
  rndfn = get_property(osconfig, "os", "rndfile", NULL);
  if (rndfn) seed_random_device(rndfn);

  // Load and execute init program
  init = get_property(osconfig, "os", "init", "/bin/sh");
  while (1)
  {
    rc = spawn(P_WAIT, NULL, init, NULL);
    if (rc != 0) 
    {
      syslog(LOG_DEBUG, "Init returned exit code %d: %s", rc, init);
      sleep(1);
    }
  }

  //if (rndfn) save_random_device(rndfn);
  //syslog(LOG_INFO | LOG_SYSLOG, "shutdown");
  //stop_syslog();
  //exitos(EXITOS_POWEROFF);
}
