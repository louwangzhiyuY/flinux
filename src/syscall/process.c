/*
 * This file is part of Foreign Linux.
 *
 * Copyright (C) 2014, 2015 Xiangyan Sun <wishstudio@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <common/errno.h>
#include <common/futex.h>
#include <common/resource.h>
#include <common/sysinfo.h>
#include <common/wait.h>
#include <fs/virtual.h>
#include <syscall/mm.h>
#include <syscall/process.h>
#include <syscall/sig.h>
#include <syscall/vfs.h>
#include <syscall/syscall.h>
#include <datetime.h>
#include <log.h>
#include <ntdll.h>
#include <str.h>

#include <stdbool.h>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#define MAX_PROCESS_COUNT		4096

#define PROCESS_NOTEXIST		0 /* The process does not exist */
#define PROCESS_RUNNING			1 /* The process is running normally */
struct process
{
	/* Status for current slot */
	int status;
	/* Windows process identifier */
	pid_t win_pid;
	/* Process group id */
	pid_t pgid;
	/* Parent process id */
	pid_t ppid;
	/* Session id */
	pid_t sid;
	/* Handle to sigwrite pipe in the process */
	HANDLE sigwrite;
};

struct process_shared_data
{
	pid_t last_allocated_process;
	struct process processes[MAX_PROCESS_COUNT]; /* The zero slot is never used */
};

static volatile struct process_shared_data *process_shared;

#define MAX_CHILD_COUNT 1024
struct process_data
{
	void *stack_base;
	pid_t pid;
	int child_count;
	struct slist child_list, child_freelist;
	struct child_process child[MAX_CHILD_COUNT];
	/* Mutex for process_shared */
	/* You have to lock this mutex on the following scenarios:
	 * 1. When writing to shared area
	 * 2. When reading process slots other than the current process
	 *
	 * TODO: It's better to have a lightweight interprocess RW lock.
	 * Windows only provides an intraprocess one.
	 */
	HANDLE shared_mutex;
} _process;

static struct process_data *const process = &_process;

static void process_init_private()
{
	process->child_count = 0;
	slist_init(&process->child_list);
	slist_init(&process->child_freelist);
	for (int i = 0; i < MAX_CHILD_COUNT; i++)
		slist_add(&process->child_freelist, &process->child[i].list);
	process_shared = (volatile struct process_shared_data *)mm_global_shared_alloc(sizeof(struct process_shared_data));
	SECURITY_ATTRIBUTES attr;
	attr.nLength = sizeof(SECURITY_ATTRIBUTES);
	attr.bInheritHandle = TRUE;
	attr.lpSecurityDescriptor = NULL;
	process->shared_mutex = CreateMutexW(&attr, FALSE, L"flinux_process_shared_writer");
}

static void process_lock_shared()
{
	WaitForSingleObject(process->shared_mutex, INFINITE);
}

static void process_unlock_shared()
{
	ReleaseMutex(process->shared_mutex);
}

/* Allocate a new process in the global process table, return slot id */
static pid_t process_alloc()
{
	/* Note that pid starts from 1, but initial value of last_allocated_process is zero */
	for (int i = 1; i < MAX_PROCESS_COUNT; i++)
	{
		pid_t cur = process_shared->last_allocated_process + i;
		if (cur >= MAX_PROCESS_COUNT)
			cur -= MAX_PROCESS_COUNT - 1;
		if (process_shared->processes[cur].status == PROCESS_NOTEXIST)
		{
			process_shared->last_allocated_process = cur;
			return cur;
		}
	}
	log_error("Process table full.\n");
	__debugbreak();
	return 0;
}

void process_init()
{
	process_init_private();
	process->stack_base = VirtualAlloc(NULL, STACK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	/* Allocate global process table slot */
	process_lock_shared();
	pid_t pid = process_alloc();
	if (pid == 1)
	{
		/* INIT process does not exist, create it now */
		process_shared->processes[1].status = PROCESS_RUNNING;
		process_shared->processes[1].win_pid = 0;
		process_shared->processes[1].ppid = 0;
		process_shared->processes[1].pgid = 1;
		process_shared->processes[1].sid = 1;
		process_shared->processes[1].sigwrite = NULL;
		/* Done, allocate a new pid for current process */
		pid = process_alloc();
	}
	process_shared->processes[pid].status = PROCESS_RUNNING;
	process_shared->processes[pid].win_pid = GetCurrentProcessId();
	process_shared->processes[pid].ppid = 1;
	process_shared->processes[pid].pgid = pid;
	process_shared->processes[pid].sid = pid;
	process_shared->processes[pid].sigwrite = signal_get_process_sigwrite();
	process_unlock_shared();
	process->pid = pid;
	log_info("PID: %d\n", pid);
}

void process_afterfork(void *stack_base, pid_t pid)
{
	process_init_private();
	process->stack_base = stack_base;
	/* The parent have global process table slot set for us
	 * We just use the pid they give us
	 */
	process->pid = pid;
	process_shared->processes[pid].sigwrite = signal_get_process_sigwrite();
	log_info("PID: %d\n", pid);
}

void *process_get_stack_base()
{
	return process->stack_base;
}

pid_t process_add_child(DWORD win_pid, HANDLE handle)
{
	if (slist_empty(&process->child_freelist))
	{
		log_error("process: Maximum number of process exceeded.\n");
		__debugbreak();
	}
	/* Allocate a new process table entry */
	process_lock_shared();
	pid_t pid = process_alloc();
	process_shared->processes[pid].status = PROCESS_RUNNING;
	process_shared->processes[pid].win_pid = win_pid;
	process_shared->processes[pid].pgid = process_shared->processes[process->pid].pgid;
	process_shared->processes[pid].ppid = process->pid;
	process_shared->processes[pid].sid = process_shared->processes[process->pid].sid;
	process_shared->processes[pid].sigwrite = NULL;
	process_unlock_shared();

	struct child_process *proc = slist_entry(slist_next(&process->child_freelist), struct child_process, list);
	slist_remove(&process->child_freelist, &proc->list);
	slist_add(&process->child_list, &proc->list);
	proc->pid = pid;
	proc->hProcess = handle;
	proc->terminated = false;
	process->child_count++;
	signal_add_process(proc);

	return pid;
}

static pid_t process_wait(pid_t pid, int *status, int options, struct rusage *rusage)
{
	if (options & WUNTRACED)
		log_error("Unhandled option WUNTRACED\n");
	if (options & WCONTINUED)
		log_error("Unhandled option WCONTINUED\n");
	if (rusage)
		log_error("rusage not supported.\n");
	struct child_process *proc = NULL;
	if (pid > 0)
	{
		slist_iterate_safe(&process->child_list, prev, cur)
		{
			struct child_process *p = slist_entry(cur, struct child_process, list);
			if (p->pid == pid)
			{
				proc = p;
				if (options & WNOHANG)
				{
					if (!proc->terminated)
						return -ECHILD;
				}
				else
				{
					DWORD result = signal_wait(1, &proc->hProcess, INFINITE);
					if (result == WAIT_INTERRUPTED)
						return -EINTR;
				}
				/* Decrement semaphore */
				WaitForSingleObject(signal_get_process_wait_semaphore(), INFINITE);
				/* Remove from child list */
				slist_remove(prev, cur);
				slist_add(&process->child_freelist, cur);
				process->child_count--;
				break;
			}
		}
		if (proc == NULL)
		{
			log_warning("pid %d is not a child.\n", pid);
			return -ECHILD;
		}
	}
	else if (pid == -1)
	{
		if (process->child_count == 0)
		{
			log_warning("No childs.\n");
			return -ECHILD;
		}
		if (!(options & WNOHANG))
		{
			HANDLE sem = signal_get_process_wait_semaphore();
			DWORD result = signal_wait(1, &sem, INFINITE);
			if (result == WAIT_INTERRUPTED)
				return -EINTR;
		}
		/* Find the terminated child */
		slist_iterate_safe(&process->child_list, prev, cur)
		{
			struct child_process *p = slist_entry(cur, struct child_process, list);
			if (p->terminated)
			{
				if (options & WNOHANG)
				{
					/* Decrement semaphore */
					WaitForSingleObject(signal_get_process_wait_semaphore(), INFINITE);
				}
				proc = p;
				/* Remove from child list */
				slist_remove(prev, cur);
				slist_add(&process->child_freelist, cur);
				process->child_count--;
				break;
			}
		}
		if (proc == NULL) /* WNOHANG and no unwaited child */
			return -ECHILD;
	}
	else
	{
		log_error("pid unhandled.\n");
		return -EINVAL;
	}
	DWORD exitCode;
	GetExitCodeProcess(proc->hProcess, &exitCode);
	CloseHandle(proc->hProcess);
	pid = proc->pid;
	log_info("pid: %d exit code: %d\n", pid, exitCode);
	if (status)
		*status = W_EXITCODE(exitCode, 0);
	return pid;
}

DEFINE_SYSCALL(waitpid, pid_t, pid, int *, status, int, options)
{
	log_info("sys_waitpid(%d, %p, %d)\n", pid, status, options);
	return process_wait(pid, status, options, NULL);
}

DEFINE_SYSCALL(wait4, pid_t, pid, int *, status, int, options, struct rusage *, rusage)
{
	log_info("sys_wait4(%d, %p, %d, %p)\n", pid, status, options, rusage);
	if (rusage)
		log_error("rusage != NULL\n");
	return process_wait(pid, status, options, rusage);
}

bool process_pid_exist(pid_t pid)
{
	if (pid < 0 || pid >= MAX_PROCESS_COUNT)
		return false;
	return process_shared->processes[pid].status != PROCESS_NOTEXIST;
}

pid_t process_get_pid()
{
	return process->pid;
}

DEFINE_SYSCALL(getpid)
{
	log_info("getpid(): %d\n", process->pid);
	return process->pid;
}

pid_t process_get_ppid(pid_t pid)
{
	return process_shared->processes[process->pid].ppid;
}

DEFINE_SYSCALL(getppid)
{
	pid_t ppid = process_shared->processes[process->pid].ppid;
	log_info("getppid(): %d\n", ppid);
	return ppid;
}

DEFINE_SYSCALL(setpgid, pid_t, pid, pid_t, pgid)
{
	log_info("setpgid(%d, %d)\n", pid, pgid);
	return 0;
}

pid_t process_get_pgid(pid_t pid)
{
	if (pid == 0)
		pid = process->pid;
	pid_t pgid;
	if (pid != process->pid)
		process_lock_shared();
	if (process_shared->processes[pid].status == PROCESS_NOTEXIST)
		pgid = -ESRCH;
	else
		pgid = process_shared->processes[pid].pgid;
	if (pid != process->pid)
		process_unlock_shared();
	return pgid;
}

DEFINE_SYSCALL(getpgid, pid_t, pid)
{
	pid_t pgid = process_get_pgid(pid);
	log_info("getpgid(%d): %d\n", pid, pgid);
	return pgid;
}

DEFINE_SYSCALL(getpgrp)
{
	log_info("getpgrp()\n");
	return sys_getpgid(process->pid);
}

DEFINE_SYSCALL(gettid)
{
	log_info("gettid(): %d\n", process->pid);
	return process->pid;
}

pid_t process_get_sid()
{
	return process_shared->processes[process->pid].sid;
}

DEFINE_SYSCALL(getsid)
{
	pid_t sid = process_shared->processes[process->pid].sid;
	log_info("getsid(): %d\n", sid);
	return sid;
}

void procfs_pid_begin_iter(int dir_tag)
{
	process_lock_shared();
}

void procfs_pid_end_iter(int dir_tag)
{
	process_unlock_shared();
}

int procfs_pid_iter(int dir_tag, int iter_tag, int *type, char *name, int namelen)
{
	while (iter_tag < MAX_PROCESS_COUNT && process_shared->processes[iter_tag].status == PROCESS_NOTEXIST)
		iter_tag++;
	if (iter_tag == MAX_PROCESS_COUNT)
		return VIRTUALFS_ITER_END;
	*type = DT_DIR;
	ksprintf(name, "%d", iter_tag);
	return iter_tag + 1;
}

DEFINE_SYSCALL(setsid)
{
	log_info("setsid().\n");
	log_error("setsid() not implemented.\n");
	return 0;
}

DEFINE_SYSCALL(getuid)
{
	log_info("getuid(): %d\n", 0);
	return 0;
}

DEFINE_SYSCALL(setgid, gid_t, gid)
{
	log_info("setgid(%d)\n", gid);
	return 0;
}

DEFINE_SYSCALL(getgid)
{
	log_info("getgid(): %d\n", 0);
	return 0;
}

DEFINE_SYSCALL(geteuid)
{
	log_info("geteuid(): %d\n", 0);
	return 0;
}

DEFINE_SYSCALL(getegid)
{
	log_info("getegid(): %d\n", 0);
	return 0;
}

DEFINE_SYSCALL(setuid, uid_t, uid)
{
	log_info("setuid(%d)\n", uid);
	return 0;
}

DEFINE_SYSCALL(setresuid, uid_t, ruid, uid_t, euid, uid_t, suid)
{
	log_info("setresuid(%d, %d, %d)\n", ruid, euid, suid);
	return 0;
}
DEFINE_SYSCALL(getresuid, uid_t *, ruid, uid_t *, euid, uid_t *, suid)
{
	log_info("getresuid(%d, %d, %d)\n", ruid, euid, suid);
	return 0;
}

DEFINE_SYSCALL(setresgid, gid_t, rgid, gid_t, egid, gid_t, sgid)
{
	log_info("setresgid(%d, %d, %d)\n", rgid, egid, sgid);
	return 0;
}
DEFINE_SYSCALL(getresgid, uid_t *, rgid, gid_t *, egid, gid_t *, sgid)
{
	log_info("getresgid(%d, %d, %d)\n", rgid, egid, sgid);
	return 0;
}
DEFINE_SYSCALL(getgroups, int, size, gid_t *, list)
{
	log_info("getgroups()\n");
	return 0;
}

DEFINE_SYSCALL(exit, int, status)
{
	log_info("exit(%d)\n", status);
	/* TODO: Gracefully shutdown mm, vfs, etc. */
	log_shutdown();
	ExitProcess(status);
}

DEFINE_SYSCALL(exit_group, int, status)
{
	log_info("exit_group(%d)\n", status);
	/* TODO: Gracefully shutdown mm, vfs, etc. */
	log_shutdown();
	ExitProcess(status);
}

DEFINE_SYSCALL(uname, struct utsname *, buf)
{
	log_info("sys_uname(%p)\n", buf);
	if (!mm_check_write(buf, sizeof(struct utsname)))
		return -EFAULT;
	/* Just mimic a reasonable Linux uname */
	strcpy(buf->sysname, "Linux");
	strcpy(buf->nodename, "ForeignLinux");
	strcpy(buf->release, "3.15.0");
	strcpy(buf->version, "3.15.0");
#ifdef _WIN64
	strcpy(buf->machine, "x86_64");
#else
	strcpy(buf->machine, "i686");
#endif
	strcpy(buf->domainname, "GNU/Linux");
	return 0;
}

DEFINE_SYSCALL(olduname, struct old_utsname *, buf)
{
	if (!mm_check_write(buf, sizeof(struct old_utsname)))
		return -EFAULT;
	struct utsname newbuf;
	sys_uname(&newbuf);
	strcpy(buf->sysname, newbuf.sysname);
	strcpy(buf->nodename, newbuf.nodename);
	strcpy(buf->release, newbuf.release);
	strcpy(buf->version, newbuf.version);
	strcpy(buf->machine, newbuf.machine);
	return 0;
}

DEFINE_SYSCALL(oldolduname, struct oldold_utsname *, buf)
{
	if (!mm_check_write(buf, sizeof(struct oldold_utsname)))
		return -EFAULT;
	struct utsname newbuf;
	sys_uname(&newbuf);
	strncpy(buf->sysname, newbuf.sysname, __OLD_UTS_LEN + 1);
	strncpy(buf->nodename, newbuf.nodename, __OLD_UTS_LEN + 1);
	strncpy(buf->release, newbuf.release, __OLD_UTS_LEN + 1);
	strncpy(buf->version, newbuf.version, __OLD_UTS_LEN + 1);
	strncpy(buf->machine, newbuf.machine, __OLD_UTS_LEN + 1);
	return 0;
}

DEFINE_SYSCALL(sysinfo, struct sysinfo *, info)
{
	log_info("sysinfo(%p)\n", info);
	if (!mm_check_write(info, sizeof(*info)))
		return -EFAULT;
	MEMORYSTATUSEX memory;
	memory.dwLength = sizeof(memory);
	GlobalMemoryStatusEx(&memory);

	info->uptime = (intptr_t)(GetTickCount64() / 1000ULL);
	info->loads[0] = info->loads[1] = info->loads[2] = 0; /* TODO */
	info->totalram = memory.ullTotalPhys / PAGE_SIZE;
	info->freeram = memory.ullAvailPhys / PAGE_SIZE;
	info->sharedram = 0;
	info->bufferram = 0;
	info->totalswap = memory.ullTotalPageFile / PAGE_SIZE;
	info->freeswap = memory.ullAvailPageFile / PAGE_SIZE;
	info->procs = 100; /* TODO */
	info->totalhigh = 0;
	info->freehigh = 0;
	info->mem_unit = PAGE_SIZE;
	RtlSecureZeroMemory(info->_f, sizeof(info->_f));
	return 0;
}

DEFINE_SYSCALL(getrlimit, int, resource, struct rlimit *, rlim)
{
	log_info("getrlimit(%d, %p)\n", resource, rlim);
	if (!mm_check_write(rlim, sizeof(struct rlimit)))
		return -EFAULT;
	switch (resource)
	{
	case RLIMIT_STACK:
		rlim->rlim_cur = STACK_SIZE;
		rlim->rlim_max = STACK_SIZE;
		break;

	case RLIMIT_NPROC:
		log_info("RLIMIT_NPROC: return fake result.\n");
		rlim->rlim_cur = 65536;
		rlim->rlim_max = 65536;
		break;

	case RLIMIT_NOFILE:
		rlim->rlim_cur = MAX_FD_COUNT;
		rlim->rlim_max = MAX_FD_COUNT;
		break;

	default:
		log_error("Unsupported resource: %d\n", resource);
		return -EINVAL;
	}
	return 0;
}

DEFINE_SYSCALL(setrlimit, int, resource, const struct rlimit *, rlim)
{
	log_info("setrlimit(%d, %p)\n", resource, rlim);
	if (!mm_check_read(rlim, sizeof(struct rlimit)))
		return -EFAULT;
	switch (resource)
	{
	default:
		log_error("Unsupported resource: %d\n", resource);
		return -EINVAL;
	}
}

DEFINE_SYSCALL(getrusage, int, who, struct rusage *, usage)
{
	log_info("getrusage(%d, %p)\n", who, usage);
	if (!mm_check_write(usage, sizeof(struct rusage)))
		return -EFAULT;
	ZeroMemory(usage, sizeof(struct rusage));
	switch (who)
	{
	default:
		log_error("Unhandled who: %d.\n", who);
		return -EINVAL;
	}
}

DEFINE_SYSCALL(getpriority, int, which, int, who)
{
	log_info("getpriority(which=%d, who=%d)\n", which, who);
	log_error("getpriority() not implemented. Fake returning 0.\n");
	return 0;
}

DEFINE_SYSCALL(setpriority, int, which, int, who, int, prio)
{
	log_info("setpriority(which=%d, who=%d, prio=%d)\n", which, who, prio);
	log_error("setpriority() not implemented. Fake returning 0.\n");
	return 0;
}

DEFINE_SYSCALL(prctl, int, option, uintptr_t, arg2, uintptr_t, arg3, uintptr_t, arg4, uintptr_t, arg5)
{
	log_info("prctl(%d)\n", option);
	log_error("prctl() not implemented.\n");
	return 0;
}

DEFINE_SYSCALL(capget, void *, header, void *, data)
{
	log_info("capget(%p, %p)\n", header, data);
	log_error("capget() not implemented.\n");
	return 0;
}

DEFINE_SYSCALL(capset, void *, header, const void *, data)
{
	log_info("capset(%p, %p)\n", header, data);
	log_error("capset() not implemented.\n");
	return 0;
}

DEFINE_SYSCALL(prlimit64, pid_t, pid, int, resource, const struct rlimit64 *, new_limit, struct rlimit64 *, old_limit)
{
	log_info("prlimit64(pid=%d, resource=%d, new_limit=%p, old_limit=%p)\n", pid, resource, new_limit, old_limit);
	log_error("prlimit64() not implemented.\n");
	return 0;
}

DEFINE_SYSCALL(getcpu, unsigned int *, cpu, unsigned int *, node, void *, tcache)
{
	log_info("getcpu(%p, %p, %p)\n", cpu, node, tcache);
	if (cpu)
		*cpu = 0;
	if (node)
		*node = 0;
	return 0;
}

DEFINE_SYSCALL(sched_getaffinity, pid_t, pid, size_t, cpusetsize, uint8_t *, mask)
{
	log_info("sched_getaffinity(%d, %d, %p)\n", pid, cpusetsize, mask);
	if (pid != 0)
	{
		log_error("pid != 0.\n");
		return -ESRCH;
	}
	int bytes = (cpusetsize + 7) & ~7;
	if (!mm_check_write(mask, bytes))
		return -EFAULT;
	for (int i = 0; i < bytes; i++)
		mask[i] = 0;
	/* TODO: Applications (i.e. ffmpeg) use this to detect the number of cpus and enable multithreading
	 * on cpu with multiple cores.
	 * Since we does not support multithreading at the time, we just report back one bit to let them
	 * think we only have one core and give up multithreading.
	 */
	mask[0] = 1;
#if 0
	GROUP_AFFINITY affinity;
	GetThreadGroupAffinity(GetCurrentThread(), &affinity);
	int size = min(sizeof(uintptr_t), cpusetsize) * 8;
	for (int i = 0; i < size; i++)
		if (affinity.Mask & (1 << i))
			mask[i / 8] |= 1 << i;
#endif
	return sizeof(uintptr_t);
}

DEFINE_SYSCALL(set_tid_address, int *, tidptr)
{
	log_info("set_tid_address(tidptr=%p)\n", tidptr);
	log_error("clear_child_tid not supported.\n");
	return GetCurrentThreadId();
}

DEFINE_SYSCALL(futex, int *, uaddr, int, op, int, val, const struct timespec *, timeout, int *, uaddr2, int, val3)
{
	log_info("futex(%p, %d, %d, %p, %p, %d)\n", uaddr, op, val, timeout, uaddr2, val3);
	log_error("Unsupported futex operation, returning -ENOSYS\n");
	return -ENOSYS;
}

DEFINE_SYSCALL(set_robust_list, struct robust_list_head *, head, int, len)
{
	log_info("set_robust_list(head=%p, len=%d)\n", head, len);
	if (len != sizeof(struct robust_list_head))
		log_error("len (%d) != sizeof(struct robust_list_head)\n", len);
	log_error("set_robust_list() not supported.\n");
	return 0;
}
