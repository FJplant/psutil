/*
 * Copyright (c) 2009, Giampaolo Rodola', Landry Breuil (OpenBSD).
 * All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Platform-specific module methods for FreeBSD and OpenBSD.

 * OpenBSD references:
 * - OpenBSD source code: http://anoncvs.spacehopper.org/openbsd-src/
 *
 * OpenBSD: missing compared to FreeBSD implementation:
 * - psutil.net_connections()
 * - psutil.Process.get/set_cpu_affinity()  (not supported natively)
 * - psutil.Process.memory_maps()
 */


#include <Python.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <paths.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/proc.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <net/route.h>

#include <sys/socketvar.h>    // for struct xsocket
#include <sys/un.h>
#include <sys/unpcb.h>
// for xinpcb struct
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>   // for struct xtcpcb
#include <netinet/tcp_fsm.h>   // for TCP connection states
#include <arpa/inet.h>         // for inet_ntop()

#include <sys/mount.h>

#include <net/if.h>       // net io counters
#include <net/if_dl.h>
#include <net/route.h>

#include <netinet/in.h>   // process open files/connections
#include <sys/un.h>

#include "_psutil_bsd.h"
#include "_psutil_common.h"

#ifdef  __FreeBSD__
    #include "arch/bsd/process_info.h"
#elif __OpenBSD__
    #include "arch/openbsd/openbsd.h"
#endif

#ifdef  __FreeBSD__
    #include <sys/cpuset.h>
    #include <net/if_media.h>
    #include <devstat.h>  // get io counters
    #include <sys/vmmeter.h>  // needed for vmtotal struct
    #include <libutil.h>  // process open files, shared libs (kinfo_getvmmap)

    #if __FreeBSD_version < 900000
        #include <utmp.h>  // system users
    #else
        #include <utmpx.h>
    #endif
#endif

#ifdef  __OpenBSD__
    #include <utmp.h>
    #include <netdb.h>  // for NI_MAXHOST
    #include <sys/vnode.h>  // for VREG
    #define _KERNEL  // for DTYPE_VNODE
    #include <sys/file.h>
    #undef _KERNEL
    #include <sys/disk.h>  // struct diskstats
    #include <sys/sched.h>  // for CPUSTATES & CP_*
    #include <sys/swap.h>
    #include <kvm.h>
#endif


#define TV2DOUBLE(t)    ((t).tv_sec + (t).tv_usec / 1000000.0)

#ifdef __FreeBSD__
    // convert a timeval struct to a double
    // convert a bintime struct to milliseconds
    #define BT2MSEC(bt)     (bt.sec * 1000 + ( ( (uint64_t) 1000000000 * (uint32_t) (bt.frac >> 32) ) >> 32 ) / 1000000)
#endif

#ifdef __OpenBSD__
    #define KPT2DOUBLE(t)   (t ## _sec + t ## _usec / 1000000.0)
#endif


#ifdef __FreeBSD__
/*
 * Utility function which fills a kinfo_proc struct based on process pid
 */
static int
psutil_kinfo_proc(const pid_t pid, struct kinfo_proc *proc) {
    int mib[4];
    size_t size;
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = pid;

    size = sizeof(struct kinfo_proc);

    if (sysctl((int *)mib, 4, proc, &size, NULL, 0) == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    // sysctl stores 0 in the size if we can't find the process information.
    if (size == 0) {
        NoSuchProcess();
        return -1;
    }
    return 0;
}
#endif


/*
 * Return a Python list of all the PIDs running on the system.
 */
static PyObject *
psutil_pids(PyObject *self, PyObject *args) {
    kinfo_proc *proclist = NULL;
    kinfo_proc *orig_address = NULL;
    size_t num_processes;
    size_t idx;
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_pid = NULL;

    if (py_retlist == NULL)
        return NULL;
    if (psutil_get_proc_list(&proclist, &num_processes) != 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "failed to retrieve process list.");
        goto error;
    }

    if (num_processes > 0) {
        orig_address = proclist; // save so we can free it after we're done
        for (idx = 0; idx < num_processes; idx++) {
#ifdef __FreeBSD__
            py_pid = Py_BuildValue("i", proclist->ki_pid);
#elif __OpenBSD__
            py_pid = Py_BuildValue("i", proclist->p_pid);
#endif
            if (!py_pid)
                goto error;
            if (PyList_Append(py_retlist, py_pid))
                goto error;
            Py_DECREF(py_pid);
            proclist++;
        }
        free(orig_address);
    }

    return py_retlist;

error:
    Py_XDECREF(py_pid);
    Py_DECREF(py_retlist);
    if (orig_address != NULL)
        free(orig_address);
    return NULL;
}


/*
 * Return a Python float indicating the system boot time expressed in
 * seconds since the epoch.
 */
static PyObject *
psutil_boot_time(PyObject *self, PyObject *args) {
    // fetch sysctl "kern.boottime"
    static int request[2] = { CTL_KERN, KERN_BOOTTIME };
    struct timeval boottime;
    size_t len = sizeof(boottime);

    if (sysctl(request, 2, &boottime, &len, NULL, 0) == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    return Py_BuildValue("d", (double)boottime.tv_sec);
}


/*
 * Return process name from kinfo_proc as a Python string.
 */
static PyObject *
psutil_proc_name(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
#ifdef __FreeBSD__
    return Py_BuildValue("s", kp.ki_comm);
#elif __OpenBSD__
    return Py_BuildValue("s", kp.p_comm);
#endif
}


#ifdef __FreeBSD__
/*
 * Return process pathname executable.
 * Thanks to Robert N. M. Watson:
 * http://fxr.googlebit.com/source/usr.bin/procstat/procstat_bin.c?v=8-CURRENT
 */
static PyObject *
psutil_proc_exe(PyObject *self, PyObject *args) {
    long pid;
    char pathname[PATH_MAX];
    int error;
    int mib[4];
    int ret;
    size_t size;

    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PATHNAME;
    mib[3] = pid;

    size = sizeof(pathname);
    error = sysctl(mib, 4, pathname, &size, NULL, 0);
    if (error == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    if (size == 0 || strlen(pathname) == 0) {
        ret = psutil_pid_exists(pid);
        if (ret == -1)
            return NULL;
        else if (ret == 0)
            return NoSuchProcess();
        else
            strcpy(pathname, "");
    }
    return Py_BuildValue("s", pathname);
}
#endif


/*
 * Return process cmdline as a Python list of cmdline arguments.
 */
static PyObject *
psutil_proc_cmdline(PyObject *self, PyObject *args) {
    long pid;
    PyObject *py_retlist = NULL;

    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;

    py_retlist = psutil_get_cmdline(pid);
    // psutil_get_cmdline() returns NULL only if psutil_cmd_args
    // failed with ESRCH (no process with that PID)
    if (NULL == py_retlist)
        return PyErr_SetFromErrno(PyExc_OSError);
    return Py_BuildValue("N", py_retlist);
}


/*
 * Return process parent pid from kinfo_proc as a Python integer.
 */
static PyObject *
psutil_proc_ppid(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
#ifdef __FreeBSD__
    return Py_BuildValue("l", (long)kp.ki_ppid);
#elif __OpenBSD__
    return Py_BuildValue("l", (long)kp.p_ppid);
#endif
}


/*
 * Return process status as a Python integer.
 */
static PyObject *
psutil_proc_status(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
#ifdef __FreeBSD__
    return Py_BuildValue("i", (int)kp.ki_stat);
#elif __OpenBSD__
    return Py_BuildValue("i", (int)kp.p_stat);
#endif
}


/*
 * Return process real, effective and saved user ids from kinfo_proc
 * as a Python tuple.
 */
static PyObject *
psutil_proc_uids(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
    return Py_BuildValue("lll",
#ifdef __FreeBSD__
                         (long)kp.ki_ruid,
                         (long)kp.ki_uid,
                         (long)kp.ki_svuid);
#elif __OpenBSD__
                         (long)kp.p_ruid,
                         (long)kp.p_uid,
                         (long)kp.p_svuid);
#endif
}


/*
 * Return process real, effective and saved group ids from kinfo_proc
 * as a Python tuple.
 */
static PyObject *
psutil_proc_gids(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
    return Py_BuildValue("lll",
#ifdef __FreeBSD__
                         (long)kp.ki_rgid,
                         (long)kp.ki_groups[0],
                         (long)kp.ki_svuid);
#elif __OpenBSD__
                         (long)kp.p_rgid,
                         (long)kp.p_groups[0],
                         (long)kp.p_svuid);
#endif
}


/*
 * Return process real, effective and saved group ids from kinfo_proc
 * as a Python tuple.
 */
static PyObject *
psutil_proc_tty_nr(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
#ifdef __FreeBSD__
    return Py_BuildValue("i", kp.ki_tdev);
#elif __OpenBSD__
    return Py_BuildValue("i", kp.p_tdev);
#endif
}


/*
 * Return the number of context switches performed by process as a tuple.
 */
static PyObject *
psutil_proc_num_ctx_switches(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
    return Py_BuildValue("(ll)",
#ifdef __FreeBSD__
                         kp.ki_rusage.ru_nvcsw,
                         kp.ki_rusage.ru_nivcsw);
#elif __OpenBSD__
                         kp.p_uru_nvcsw,
                         kp.p_uru_nivcsw);
#endif
}


#ifdef __FreeBSD__
/*
 * Return number of threads used by process as a Python integer.
 */
static PyObject *
psutil_proc_num_threads(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
    return Py_BuildValue("l", (long)kp.ki_numthreads);
}
#endif


/*
 * Retrieves all threads used by process returning a list of tuples
 * including thread id, user time and system time.
 * Thanks to Robert N. M. Watson (FreeBSD):
 * http://fxr.googlebit.com/source/usr.bin/procstat/
 *     procstat_threads.c?v=8-CURRENT
 * OpenBSD reference:
 * https://github.com/janmojzis/pstree/blob/master/proc_kvm.c
 * Note: OpenBSD requires root access.
 */

#ifdef __FreeBSD__
static PyObject *
psutil_proc_threads(PyObject *self, PyObject *args) {
    long pid;
    int mib[4];
    struct kinfo_proc *kip = NULL;
    struct kinfo_proc *kipp = NULL;
    int error;
    unsigned int i;
    size_t size;
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_tuple = NULL;

    if (py_retlist == NULL)
        return NULL;
    if (! PyArg_ParseTuple(args, "l", &pid))
        goto error;

    // we need to re-query for thread information, so don't use *kipp
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID | KERN_PROC_INC_THREAD;
    mib[3] = pid;

    size = 0;
    error = sysctl(mib, 4, NULL, &size, NULL, 0);
    if (error == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }
    if (size == 0) {
        NoSuchProcess();
        goto error;
    }

    kip = malloc(size);
    if (kip == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    error = sysctl(mib, 4, kip, &size, NULL, 0);
    if (error == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }
    if (size == 0) {
        NoSuchProcess();
        goto error;
    }

    for (i = 0; i < size / sizeof(*kipp); i++) {
        kipp = &kip[i];
        py_tuple = Py_BuildValue("Idd",
                                 kipp->ki_tid,
                                 TV2DOUBLE(kipp->ki_rusage.ru_utime),
                                 TV2DOUBLE(kipp->ki_rusage.ru_stime));
        if (py_tuple == NULL)
            goto error;
        if (PyList_Append(py_retlist, py_tuple))
            goto error;
        Py_DECREF(py_tuple);
    }
    free(kip);
    return py_retlist;

error:
    Py_XDECREF(py_tuple);
    Py_DECREF(py_retlist);
    if (kip != NULL)
        free(kip);
    return NULL;
}
#endif


#ifdef __FreeBSD__
/*
 * Return an XML string from which we'll determine the number of
 * physical CPU cores in the system.
 */
static PyObject *
psutil_cpu_count_phys(PyObject *self, PyObject *args) {
    void *topology = NULL;
    size_t size = 0;
    PyObject *py_str;

    if (sysctlbyname("kern.sched.topology_spec", NULL, &size, NULL, 0))
        goto error;

    topology = malloc(size);
    if (!topology) {
        PyErr_NoMemory();
        return NULL;
    }

    if (sysctlbyname("kern.sched.topology_spec", topology, &size, NULL, 0))
        goto error;

    py_str = Py_BuildValue("s", topology);
    free(topology);
    return py_str;

error:
    if (topology != NULL)
        free(topology);
    Py_RETURN_NONE;
}
#endif


/*
 * Return a Python tuple (user_time, kernel_time)
 */
static PyObject *
psutil_proc_cpu_times(PyObject *self, PyObject *args) {
    long pid;
    double user_t, sys_t;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
    // convert from microseconds to seconds
#ifdef __FreeBSD__
    user_t = TV2DOUBLE(kp.ki_rusage.ru_utime);
    sys_t = TV2DOUBLE(kp.ki_rusage.ru_stime);
#elif __OpenBSD__
    user_t = KPT2DOUBLE(kp.p_uutime);
    sys_t = KPT2DOUBLE(kp.p_ustime);
#endif
    return Py_BuildValue("(dd)", user_t, sys_t);
}


/*
 * Return the number of logical CPUs in the system.
 * XXX this could be shared with OSX
 */
static PyObject *
psutil_cpu_count_logical(PyObject *self, PyObject *args) {
    int mib[2];
    int ncpu;
    size_t len;

    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;
    len = sizeof(ncpu);

    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == -1)
        Py_RETURN_NONE;  // mimic os.cpu_count()
    else
        return Py_BuildValue("i", ncpu);
}


/*
 * Return a Python float indicating the process create time expressed in
 * seconds since the epoch.
 */
static PyObject *
psutil_proc_create_time(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
#ifdef __FreeBSD__
    return Py_BuildValue("d", TV2DOUBLE(kp.ki_start));
#elif __OpenBSD__
    return Py_BuildValue("d", KPT2DOUBLE(kp.p_ustart));
#endif
}


/*
 * Return a Python float indicating the process create time expressed in
 * seconds since the epoch.
 */
static PyObject *
psutil_proc_io_counters(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
    // there's apparently no way to determine bytes count, hence return -1.
    return Py_BuildValue("(llll)",
#ifdef __FreeBSD__
                         kp.ki_rusage.ru_inblock,
                         kp.ki_rusage.ru_oublock,
#elif __OpenBSD__
                         kp.p_uru_inblock,
                         kp.p_uru_oublock,
#endif
                         -1,
                         -1);
}


/*
 * Return extended memory info for a process as a Python tuple.
 */
#define ptoa(x)         ((paddr_t)(x) << PAGE_SHIFT)
static PyObject *
psutil_proc_memory_info(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;
    return Py_BuildValue(
        "(lllll)",
#ifdef __FreeBSD__
        ptoa(kp.ki_rssize),  // rss
        (long)kp.ki_size,  // vms
        ptoa(kp.ki_tsize),  // text
        ptoa(kp.ki_dsize),  // data
        ptoa(kp.ki_ssize));  // stack
#elif __OpenBSD__
        ptoa(kp.p_vm_rssize),    // rss
        // vms, this is how ps does it, see:
        // http://anoncvs.spacehopper.org/openbsd-src/tree/bin/ps/print.c#n461
        ptoa(kp.p_vm_dsize + kp.p_vm_ssize + kp.p_vm_tsize),  // vms
        ptoa(kp.p_vm_tsize),  // text
        ptoa(kp.p_vm_dsize),  // data
        ptoa(kp.p_vm_ssize));  // stack
#endif
}


#ifdef __FreeBSD__
/*
 * Return virtual memory usage statistics.
 */
static PyObject *
psutil_virtual_mem(PyObject *self, PyObject *args) {
    unsigned int   total, active, inactive, wired, cached, free;
    size_t         size = sizeof(total);
    struct vmtotal vm;
    int            mib[] = {CTL_VM, VM_METER};
    long           pagesize = getpagesize();
#if __FreeBSD_version > 702101
    long buffers;
#else
    int buffers;
#endif
    size_t buffers_size = sizeof(buffers);

    if (sysctlbyname("vm.stats.vm.v_page_count", &total, &size, NULL, 0))
        goto error;
    if (sysctlbyname("vm.stats.vm.v_active_count", &active, &size, NULL, 0))
        goto error;
    if (sysctlbyname("vm.stats.vm.v_inactive_count",
                     &inactive, &size, NULL, 0))
        goto error;
    if (sysctlbyname("vm.stats.vm.v_wire_count", &wired, &size, NULL, 0))
        goto error;
    if (sysctlbyname("vm.stats.vm.v_cache_count", &cached, &size, NULL, 0))
        goto error;
    if (sysctlbyname("vm.stats.vm.v_free_count", &free, &size, NULL, 0))
        goto error;
    if (sysctlbyname("vfs.bufspace", &buffers, &buffers_size, NULL, 0))
        goto error;

    size = sizeof(vm);
    if (sysctl(mib, 2, &vm, &size, NULL, 0) != 0)
        goto error;

    return Py_BuildValue("KKKKKKKK",
        (unsigned long long) total    * pagesize,
        (unsigned long long) free     * pagesize,
        (unsigned long long) active   * pagesize,
        (unsigned long long) inactive * pagesize,
        (unsigned long long) wired    * pagesize,
        (unsigned long long) cached   * pagesize,
        (unsigned long long) buffers,
        (unsigned long long) (vm.t_vmshr + vm.t_rmshr) * pagesize  // shared
    );

error:
    PyErr_SetFromErrno(PyExc_OSError);
    return NULL;
}
#endif


#ifndef _PATH_DEVNULL
#define _PATH_DEVNULL "/dev/null"
#endif

#ifdef __FreeBSD__
/*
 * Return swap memory stats (see 'swapinfo' cmdline tool)
 */
static PyObject *
psutil_swap_mem(PyObject *self, PyObject *args) {
    kvm_t *kd;
    struct kvm_swap kvmsw[1];
    unsigned int swapin, swapout, nodein, nodeout;
    size_t size = sizeof(unsigned int);

    kd = kvm_open(NULL, _PATH_DEVNULL, NULL, O_RDONLY, "kvm_open failed");
    if (kd == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "kvm_open failed");
        return NULL;
    }

    if (kvm_getswapinfo(kd, kvmsw, 1, 0) < 0) {
        kvm_close(kd);
        PyErr_SetString(PyExc_RuntimeError, "kvm_getswapinfo failed");
        return NULL;
    }

    kvm_close(kd);

    if (sysctlbyname("vm.stats.vm.v_swapin", &swapin, &size, NULL, 0) == -1)
        goto sbn_error;
    if (sysctlbyname("vm.stats.vm.v_swapout", &swapout, &size, NULL, 0) == -1)
        goto sbn_error;
    if (sysctlbyname("vm.stats.vm.v_vnodein", &nodein, &size, NULL, 0) == -1)
        goto sbn_error;
    if (sysctlbyname("vm.stats.vm.v_vnodeout", &nodeout, &size, NULL, 0) == -1)
        goto sbn_error;

    return Py_BuildValue("(iiiII)",
                         kvmsw[0].ksw_total,                     // total
                         kvmsw[0].ksw_used,                      // used
                         kvmsw[0].ksw_total - kvmsw[0].ksw_used, // free
                         swapin + swapout,                       // swap in
                         nodein + nodeout);                      // swap out

sbn_error:
    PyErr_SetFromErrno(PyExc_OSError);
    return NULL;
}
#endif


/*
 * Return a Python tuple representing user, kernel and idle CPU times
 */
static PyObject *
psutil_cpu_times(PyObject *self, PyObject *args) {
    long cpu_time[CPUSTATES];
    size_t size = sizeof(cpu_time);
    int ret;

#ifdef __FreeBSD__
    ret = sysctlbyname("kern.cp_time", &cpu_time, &size, NULL, 0);
#elif __OpenBSD__
    int mib[] = {CTL_KERN, KERN_CPTIME};
    ret = sysctl(mib, 2, &cpu_time, &size, NULL, 0);
#endif
    if (ret == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    return Py_BuildValue("(ddddd)",
                         (double)cpu_time[CP_USER] / CLOCKS_PER_SEC,
                         (double)cpu_time[CP_NICE] / CLOCKS_PER_SEC,
                         (double)cpu_time[CP_SYS] / CLOCKS_PER_SEC,
                         (double)cpu_time[CP_IDLE] / CLOCKS_PER_SEC,
                         (double)cpu_time[CP_INTR] / CLOCKS_PER_SEC
                        );
}


/*
 * Return files opened by process as a list of ("", fd) tuples
 */
#if defined(__FreeBSD_version) && __FreeBSD_version >= 800000
 /*
 * Return files opened by process as a list of (path, fd) tuples.
 * TODO: this is broken as it may report empty paths. 'procstat'
 * utility has the same problem see:
 * https://github.com/giampaolo/psutil/issues/595
 */
static PyObject *
psutil_proc_open_files(PyObject *self, PyObject *args) {
    long pid;
    int i, cnt;
    struct kinfo_file *freep = NULL;
    struct kinfo_file *kif;
    struct kinfo_proc kipp;
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_tuple = NULL;

    if (py_retlist == NULL)
        return NULL;
    if (! PyArg_ParseTuple(args, "l", &pid))
        goto error;
    if (psutil_kinfo_proc(pid, &kipp) == -1)
        goto error;

    freep = kinfo_getfile(pid, &cnt);
    if (freep == NULL) {
        psutil_raise_ad_or_nsp(pid);
        goto error;
    }

    for (i = 0; i < cnt; i++) {
        kif = &freep[i];
        if ((kif->kf_type == KF_TYPE_VNODE) &&
                (kif->kf_vnode_type == KF_VTYPE_VREG))
        {
            py_tuple = Py_BuildValue("(si)", kif->kf_path, kif->kf_fd);
            if (py_tuple == NULL)
                goto error;
            if (PyList_Append(py_retlist, py_tuple))
                goto error;
            Py_DECREF(py_tuple);
        }
    }
    free(freep);
    return py_retlist;

error:
    Py_XDECREF(py_tuple);
    Py_DECREF(py_retlist);
    if (freep != NULL)
        free(freep);
    return NULL;
}


/*
 * Return files opened by process as a list of (path, fd) tuples
 */
static PyObject *
psutil_proc_num_fds(PyObject *self, PyObject *args) {
    long pid;
    int cnt;

    struct kinfo_file *freep;
    struct kinfo_proc kipp;

    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kipp) == -1)
        return NULL;

    freep = kinfo_getfile(pid, &cnt);
    if (freep == NULL) {
        psutil_raise_ad_or_nsp(pid);
        return NULL;
    }
    free(freep);

    return Py_BuildValue("i", cnt);
}


/*
 * Return process current working directory.
 */
static PyObject *
psutil_proc_cwd(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_file *freep = NULL;
    struct kinfo_file *kif;
    struct kinfo_proc kipp;
    PyObject *py_path = NULL;

    int i, cnt;

    if (! PyArg_ParseTuple(args, "l", &pid))
        goto error;
    if (psutil_kinfo_proc(pid, &kipp) == -1)
        goto error;

    freep = kinfo_getfile(pid, &cnt);
    if (freep == NULL) {
        psutil_raise_ad_or_nsp(pid);
        goto error;
    }

    for (i = 0; i < cnt; i++) {
        kif = &freep[i];
        if (kif->kf_fd == KF_FD_TYPE_CWD) {
            py_path = Py_BuildValue("s", kif->kf_path);
            if (!py_path)
                goto error;
            break;
        }
    }
    /*
     * For lower pids it seems we can't retrieve any information
     * (lsof can't do that it either).  Since this happens even
     * as root we return an empty string instead of AccessDenied.
     */
    if (py_path == NULL)
        py_path = Py_BuildValue("s", "");
    free(freep);
    return py_path;

error:
    Py_XDECREF(py_path);
    if (freep != NULL)
        free(freep);
    return NULL;
}
#endif


#ifdef  __OpenBSD__
static PyObject *
psutil_proc_open_files(PyObject *self, PyObject *args) {
    long pid;
    int i, cnt;
    struct kinfo_file *freep = NULL;
    struct kinfo_file *kif;
    struct kinfo_proc kipp;
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_tuple = NULL;

    if (py_retlist == NULL)
        return NULL;
    if (! PyArg_ParseTuple(args, "l", &pid))
        goto error;
    if (psutil_kinfo_proc(pid, &kipp) == -1)
        goto error;

    freep = kinfo_getfile(pid, &cnt);
    if (freep == NULL) {
        psutil_raise_ad_or_nsp(pid);
        goto error;
    }

    for (i = 0; i < cnt; i++) {
        kif = &freep[i];
        if ((kif->f_type == DTYPE_VNODE) &&
                (kif->v_type == VREG))
        {
            py_tuple = Py_BuildValue("(si)", "", kif->fd_fd);
            if (py_tuple == NULL)
                goto error;
            if (PyList_Append(py_retlist, py_tuple))
                goto error;
            Py_DECREF(py_tuple);
        }
    }
    free(freep);
    return py_retlist;

error:
    Py_XDECREF(py_tuple);
    Py_DECREF(py_retlist);
    if (freep != NULL)
        free(freep);
    return NULL;
}
#endif


/*
 * Return files opened by process as a list of (path, fd) tuples
 */
static PyObject *
psutil_proc_num_fds(PyObject *self, PyObject *args) {
    long pid;
    int cnt;

    struct kinfo_file *freep;
    struct kinfo_proc kipp;

    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kipp) == -1)
        return NULL;

    freep = kinfo_getfile(pid, &cnt);
    if (freep == NULL) {
        psutil_raise_ad_or_nsp(pid);
        return NULL;
    }
    free(freep);

    return Py_BuildValue("i", cnt);
}


/*
 * Process current working directory.
 * Reference:
 * http://anoncvs.spacehopper.org/openbsd-src/tree/bin/ps/print.c#n179
 */
static PyObject *
psutil_proc_cwd(PyObject *self, PyObject *args) {
    long pid;
    struct kinfo_proc kp;
    char path[MAXPATHLEN];
    size_t pathlen = sizeof path;

    if (! PyArg_ParseTuple(args, "l", &pid))
        return NULL;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        return NULL;

    int name[] = { CTL_KERN, KERN_PROC_CWD, pid };
    if (sysctl(name, 3, path, &pathlen, NULL, 0) != 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    return Py_BuildValue("s", path);
}


// see sys/kern/kern_sysctl.c lines 1100 and
// usr.bin/fstat/fstat.c print_inet_details()
char *
psutil_convert_ipv4(int family, uint32_t addr[4]) {
    struct in_addr a;
    memcpy(&a, addr, sizeof(a));
    return inet_ntoa(a);
}


const char *
psutil_inet6_addrstr(struct in6_addr *p)
{
    struct sockaddr_in6 sin6;
    static char hbuf[NI_MAXHOST];
    const int niflags = NI_NUMERICHOST;

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_len = sizeof(struct sockaddr_in6);
    sin6.sin6_addr = *p;
    if (IN6_IS_ADDR_LINKLOCAL(p) &&
        *(u_int16_t *)&sin6.sin6_addr.s6_addr[2] != 0) {
        sin6.sin6_scope_id =
            ntohs(*(u_int16_t *)&sin6.sin6_addr.s6_addr[2]);
        sin6.sin6_addr.s6_addr[2] = sin6.sin6_addr.s6_addr[3] = 0;
    }

    if (getnameinfo((struct sockaddr *)&sin6, sin6.sin6_len,
        hbuf, sizeof(hbuf), NULL, 0, niflags))
        return "invalid";

    return hbuf;
}


// a signaler for connections without an actual status
static int PSUTIL_CONN_NONE = 128;

/*
 * Return connections opened by process.
 */
static PyObject *
psutil_proc_connections(PyObject *self, PyObject *args) {
    long pid;
    int i, cnt;

    struct kinfo_file *freep = NULL;
    struct kinfo_file *kif;
    char *tcplist = NULL;
    struct tcpcb *tcp;

    PyObject *py_retlist = PyList_New(0);
    PyObject *py_tuple = NULL;
    PyObject *py_laddr = NULL;
    PyObject *py_raddr = NULL;
    PyObject *py_af_filter = NULL;
    PyObject *py_type_filter = NULL;
    PyObject *py_family = NULL;
    PyObject *_type = NULL;

    if (py_retlist == NULL)
        return NULL;
    if (! PyArg_ParseTuple(args, "lOO", &pid, &py_af_filter, &py_type_filter))
        goto error;
    if (!PySequence_Check(py_af_filter) || !PySequence_Check(py_type_filter)) {
        PyErr_SetString(PyExc_TypeError, "arg 2 or 3 is not a sequence");
        goto error;
    }

    freep = kinfo_getfile(pid, &cnt);
    if (freep == NULL) {
        psutil_raise_ad_or_nsp(pid);
        goto error;
    }

    for (i = 0; i < cnt; i++) {
        int state;
        int lport;
        int rport;
        char path[PATH_MAX];
        char addrbuf[NI_MAXHOST + 2];
        int inseq;
        struct in6_addr laddr6;
        py_tuple = NULL;
        py_laddr = NULL;
        py_raddr = NULL;

        kif = &freep[i];
        if (kif->f_type == DTYPE_SOCKET) {
            // apply filters
            py_family = PyLong_FromLong((long)kif->so_family);
            inseq = PySequence_Contains(py_af_filter, py_family);
            Py_DECREF(py_family);
            if (inseq == 0)
                continue;
            _type = PyLong_FromLong((long)kif->so_type);
            inseq = PySequence_Contains(py_type_filter, _type);
            Py_DECREF(_type);
            if (inseq == 0)
                continue;

            // IPv4 / IPv6 socket
            if ((kif->so_family == AF_INET) || (kif->so_family == AF_INET6)) {
                // fill status
                if (kif->so_type == SOCK_STREAM)
                    state = kif->t_state;
                else
                    state = PSUTIL_CONN_NONE;

                // ports
                lport = ntohs(kif->inp_lport);
                rport = ntohs(kif->inp_fport);

                // local address, IPv4
                if (kif->so_family == AF_INET) {
                    py_laddr = Py_BuildValue(
                        "(si)",
                        psutil_convert_ipv4(kif->so_family, kif->inp_laddru),
                        lport);
                    if (!py_laddr)
                        goto error;
                }
                else {
                    // local address, IPv6
                    memcpy(&laddr6, kif->inp_laddru, sizeof(laddr6));
                    (void *)(uintptr_t)kif->inp_ppcb;
                    snprintf(addrbuf, sizeof(addrbuf), "%s",
                             psutil_inet6_addrstr(&laddr6));
                    py_laddr = Py_BuildValue("(si)", addrbuf, lport);
                    if (!py_laddr)
                        goto error;
                }

                if (rport != 0) {
                    // remote address, IPv4
                    if (kif->so_family == AF_INET) {
                        py_raddr = Py_BuildValue(
                            "(si)",
                            psutil_convert_ipv4(
                                kif->so_family, kif->inp_faddru),
                            rport);
                    }
                    else {
                        // remote address, IPv6
                        memcpy(&laddr6, kif->inp_faddru, sizeof(laddr6));
                        (void *)(uintptr_t)kif->inp_ppcb;
                        snprintf(addrbuf, sizeof(addrbuf), "%s",
                                 psutil_inet6_addrstr(&laddr6));
                        py_raddr = Py_BuildValue("(si)", addrbuf, rport);
                        if (!py_raddr)
                            goto error;
                    }
                }
                else {
                    py_raddr = Py_BuildValue("()");
                }

                if (!py_raddr)
                    goto error;
                py_tuple = Py_BuildValue(
                    "(iiiNNi)",
                    kif->fd_fd,
                    kif->so_family,
                    kif->so_type,
                    py_laddr,
                    py_raddr,
                    state);
                if (!py_tuple)
                    goto error;
                if (PyList_Append(py_retlist, py_tuple))
                    goto error;
                Py_DECREF(py_tuple);
            }
            // UNIX socket
            else if (kif->so_family == AF_UNIX) {
                py_tuple = Py_BuildValue(
                    "(iiisOi)",
                    kif->fd_fd,
                    kif->so_family,
                    kif->so_type,
                    kif->unp_path,
                    Py_None,
                    PSUTIL_CONN_NONE);
                if (!py_tuple)
                    goto error;
                if (PyList_Append(py_retlist, py_tuple))
                    goto error;
                Py_DECREF(py_tuple);
                Py_INCREF(Py_None);
            }
        }
    }
    free(freep);
    free(tcplist);
    return py_retlist;

error:
    Py_XDECREF(py_tuple);
    Py_XDECREF(py_laddr);
    Py_XDECREF(py_raddr);
    Py_DECREF(py_retlist);
    if (freep != NULL)
        free(freep);
    if (tcplist != NULL)
        free(tcplist);
    return NULL;
}


#ifdef __OpenBSD__
/*
 * Return a Python list of tuple representing per-cpu times
 */
static PyObject *
psutil_per_cpu_times(PyObject *self, PyObject *args) {
    static int maxcpus;
    int mib[3];
    int ncpu;
    size_t len;
    size_t size;
    int i;
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_cputime = NULL;

    if (py_retlist == NULL)
        return NULL;


    // retrieve the number of cpus
    mib[0] = CTL_HW;
    mib[1] = HW_NCPU;
    len = sizeof(ncpu);
    if (sysctl(mib, 2, &ncpu, &len, NULL, 0) == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }
    uint64_t cpu_time[CPUSTATES];

    for (i = 0; i < ncpu; i++) {
        // per-cpu info
        mib[0] = CTL_KERN;
        mib[1] = KERN_CPTIME2;
        mib[2] = i;
        size = sizeof(cpu_time);
        if (sysctl(mib, 3, &cpu_time, &size, NULL, 0) == -1) {
            warn("failed to get kern.cptime2");
            PyErr_SetFromErrno(PyExc_OSError);
            return NULL;
        }

        py_cputime = Py_BuildValue(
            "(ddddd)",
            (double)cpu_time[CP_USER] / CLOCKS_PER_SEC,
            (double)cpu_time[CP_NICE] / CLOCKS_PER_SEC,
            (double)cpu_time[CP_SYS] / CLOCKS_PER_SEC,
            (double)cpu_time[CP_IDLE] / CLOCKS_PER_SEC,
            (double)cpu_time[CP_INTR] / CLOCKS_PER_SEC);
        if (!py_cputime)
            goto error;
        if (PyList_Append(py_retlist, py_cputime))
            goto error;
        Py_DECREF(py_cputime);
    }

    return py_retlist;

error:
    Py_XDECREF(py_cputime);
    Py_DECREF(py_retlist);
    return NULL;
}
#endif


#if 0
// remove spaces from string
void remove_spaces(char *str) {
    char *p1 = str;
    char *p2 = str;
    do
        while (*p2 == ' ')
            p2++;
    while (*p1++ = *p2++);
}


/*
 * Return a list of tuples for every process memory maps.
 * 'procstat' cmdline utility has been used as an example.
 */
static PyObject *
psutil_proc_memory_maps(PyObject *self, PyObject *args) {
    long pid;
    int ptrwidth;
    int i, cnt;
    char addr[30];
    char perms[4];
    const char *path;
    struct kinfo_proc kp;
    struct kinfo_vmentry *freep = NULL;
    struct kinfo_vmentry *kve;
    ptrwidth = 2 * sizeof(void *);
    PyObject *py_tuple = NULL;
    PyObject *retlist = PyList_New(0);

    if (retlist == NULL) {
        return NULL;
    }
    if (! PyArg_ParseTuple(args, "l", &pid))
        goto error;
    if (psutil_kinfo_proc(pid, &kp) == -1)
        goto error;

    freep = kinfo_getvmmap(pid, &cnt);
    if (freep == NULL) {
        psutil_raise_ad_or_nsp(pid);
        goto error;
    }
    for (i = 0; i < cnt; i++) {
        py_tuple = NULL;
        kve = &freep[i];
        addr[0] = '\0';
        perms[0] = '\0';
        sprintf(addr, "%#*jx-%#*jx", ptrwidth, (uintmax_t)kve->kve_start,
                ptrwidth, (uintmax_t)kve->kve_end);
        remove_spaces(addr);
        strlcat(perms, kve->kve_protection & KVME_PROT_READ ? "r" : "-",
                sizeof(perms));
        strlcat(perms, kve->kve_protection & KVME_PROT_WRITE ? "w" : "-",
                sizeof(perms));
        strlcat(perms, kve->kve_protection & KVME_PROT_EXEC ? "x" : "-",
                sizeof(perms));

        if (strlen(kve->kve_path) == 0) {
            switch (kve->kve_type) {
            case KVME_TYPE_NONE:
                path = "[none]";
                break;
            case KVME_TYPE_DEFAULT:
                path = "[default]";
                break;
            case KVME_TYPE_VNODE:
                path = "[vnode]";
                break;
            case KVME_TYPE_SWAP:
                path = "[swap]";
                break;
            case KVME_TYPE_DEVICE:
                path = "[device]";
                break;
            case KVME_TYPE_PHYS:
                path = "[phys]";
                break;
            case KVME_TYPE_DEAD:
                path = "[dead]";
                break;
            case KVME_TYPE_SG:
                path = "[sg]";
                break;
            case KVME_TYPE_UNKNOWN:
                path = "[unknown]";
                break;
            default:
                path = "[?]";
                break;
            }
        }
        else {
            path = kve->kve_path;
        }

        py_tuple = Py_BuildValue("sssiiii",
            addr,                       // "start-end" address
            perms,                      // "rwx" permissions
            path,                       // path
            kve->kve_resident,          // rss
            kve->kve_private_resident,  // private
            kve->kve_ref_count,         // ref count
            kve->kve_shadow_count);     // shadow count
        if (!py_tuple)
            goto error;
        if (PyList_Append(retlist, py_tuple))
            goto error;
        Py_DECREF(py_tuple);
    }
    free(freep);
    return retlist;

error:
    Py_XDECREF(py_tuple);
    Py_DECREF(retlist);
    if (freep != NULL)
        free(freep);
    return NULL;
}
#endif


/*
 * Return a list of tuples including device, mount point and fs type
 * for all partitions mounted on the system.
 */
static PyObject *
psutil_disk_partitions(PyObject *self, PyObject *args) {
    int num;
    int i;
    long len;
    uint64_t flags;
    char opts[200];
    struct statfs *fs = NULL;
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_tuple = NULL;

    if (py_retlist == NULL)
        return NULL;

    // get the number of mount points
    Py_BEGIN_ALLOW_THREADS
    num = getfsstat(NULL, 0, MNT_NOWAIT);
    Py_END_ALLOW_THREADS
    if (num == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }

    len = sizeof(*fs) * num;
    fs = malloc(len);
    if (fs == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    Py_BEGIN_ALLOW_THREADS
    num = getfsstat(fs, len, MNT_NOWAIT);
    Py_END_ALLOW_THREADS
    if (num == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }

    for (i = 0; i < num; i++) {
        py_tuple = NULL;
        opts[0] = 0;
        flags = fs[i].f_flags;

        // see sys/mount.h
        if (flags & MNT_RDONLY)
            strlcat(opts, "ro", sizeof(opts));
        else
            strlcat(opts, "rw", sizeof(opts));
        if (flags & MNT_SYNCHRONOUS)
            strlcat(opts, ",sync", sizeof(opts));
        if (flags & MNT_NOEXEC)
            strlcat(opts, ",noexec", sizeof(opts));
        if (flags & MNT_NOSUID)
            strlcat(opts, ",nosuid", sizeof(opts));
        if (flags & MNT_ASYNC)
            strlcat(opts, ",async", sizeof(opts));
        if (flags & MNT_SOFTDEP)
            strlcat(opts, ",softdep", sizeof(opts));
        if (flags & MNT_NOATIME)
            strlcat(opts, ",noatime", sizeof(opts));

        py_tuple = Py_BuildValue("(ssss)",
                                 fs[i].f_mntfromname,  // device
                                 fs[i].f_mntonname,    // mount point
                                 fs[i].f_fstypename,   // fs type
                                 opts);                // options
        if (!py_tuple)
            goto error;
        if (PyList_Append(py_retlist, py_tuple))
            goto error;
        Py_DECREF(py_tuple);
    }

    free(fs);
    return py_retlist;

error:
    Py_XDECREF(py_tuple);
    Py_DECREF(py_retlist);
    if (fs != NULL)
        free(fs);
    return NULL;
}


/*
 * Return a Python list of named tuples with overall network I/O information
 */
static PyObject *
psutil_net_io_counters(PyObject *self, PyObject *args) {
    char *buf = NULL, *lim, *next;
    struct if_msghdr *ifm;
    int mib[6];
    size_t len;
    PyObject *py_retdict = PyDict_New();
    PyObject *py_ifc_info = NULL;
    if (py_retdict == NULL)
        return NULL;

    mib[0] = CTL_NET;          // networking subsystem
    mib[1] = PF_ROUTE;         // type of information
    mib[2] = 0;                // protocol (IPPROTO_xxx)
    mib[3] = 0;                // address family
    mib[4] = NET_RT_IFLIST;   // operation
    mib[5] = 0;

    if (sysctl(mib, 6, NULL, &len, NULL, 0) < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }

    buf = malloc(len);
    if (buf == NULL) {
        PyErr_NoMemory();
        goto error;
    }

    if (sysctl(mib, 6, buf, &len, NULL, 0) < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }

    lim = buf + len;

    for (next = buf; next < lim; ) {
        py_ifc_info = NULL;
        ifm = (struct if_msghdr *)next;
        next += ifm->ifm_msglen;

        if (ifm->ifm_type == RTM_IFINFO) {
            struct if_msghdr *if2m = (struct if_msghdr *)ifm;
            struct sockaddr_dl *sdl = (struct sockaddr_dl *)(if2m + 1);
            char ifc_name[32];

            strncpy(ifc_name, sdl->sdl_data, sdl->sdl_nlen);
            ifc_name[sdl->sdl_nlen] = 0;
            // XXX: ignore usbus interfaces:
            // http://lists.freebsd.org/pipermail/freebsd-current/
            //     2011-October/028752.html
            // 'ifconfig -a' doesn't show them, nor do we.
            if (strncmp(ifc_name, "usbus", 5) == 0) {
                continue;
            }

            py_ifc_info = Py_BuildValue("(kkkkkkki)",
                                        if2m->ifm_data.ifi_obytes,
                                        if2m->ifm_data.ifi_ibytes,
                                        if2m->ifm_data.ifi_opackets,
                                        if2m->ifm_data.ifi_ipackets,
                                        if2m->ifm_data.ifi_ierrors,
                                        if2m->ifm_data.ifi_oerrors,
                                        if2m->ifm_data.ifi_iqdrops,
                                        0);  // dropout not supported
            if (!py_ifc_info)
                goto error;
            if (PyDict_SetItemString(py_retdict, ifc_name, py_ifc_info))
                goto error;
            Py_DECREF(py_ifc_info);
        }
        else {
            continue;
        }
    }

    free(buf);
    return py_retdict;

error:
    Py_XDECREF(py_ifc_info);
    Py_DECREF(py_retdict);
    if (buf != NULL)
        free(buf);
    return NULL;
}


/*
 * Return a Python dict of tuples for disk I/O information
 */
static PyObject *
psutil_disk_io_counters(PyObject *self, PyObject *args) {
    int i, dk_ndrive, mib[3];
    size_t len;
    struct diskstats *stats;

    PyObject *py_retdict = PyDict_New();
    PyObject *py_disk_info = NULL;
    if (py_retdict == NULL)
        return NULL;

    mib[0] = CTL_HW;
    mib[1] = HW_DISKSTATS;
    len = 0;
    if (sysctl(mib, 2, NULL, &len, NULL, 0) < 0) {
        warn("can't get hw.diskstats size");
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }
    dk_ndrive = (int)(len / sizeof(struct diskstats));

    stats = malloc(len);
    if (stats == NULL) {
        warn("can't malloc");
        PyErr_NoMemory();
        goto error;
    }
    if (sysctl(mib, 2, stats, &len, NULL, 0) < 0 ) {
        warn("could not read hw.diskstats");
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }

    for (i = 0; i < dk_ndrive; i++) {
        py_disk_info = Py_BuildValue(
            "(KKKKLL)",
            stats[i].ds_rxfer,
            stats[i].ds_wxfer,
            stats[i].ds_rbytes,
            stats[i].ds_wbytes,
            (long long) TV2DOUBLE(stats[i].ds_time) / 2, /* assume half read - half writes.. */
            (long long) TV2DOUBLE(stats[i].ds_time) / 2);
        if (!py_disk_info)
            goto error;
        if (PyDict_SetItemString(py_retdict, stats[i].ds_name, py_disk_info))
            goto error;
        Py_DECREF(py_disk_info);
    }

    free(stats);
    return py_retdict;

error:
    Py_XDECREF(py_disk_info);
    Py_DECREF(py_retdict);
    if (stats != NULL)
        free(stats);
    return NULL;
}

/*
 * Return currently connected users as a list of tuples.
 */
static PyObject *
psutil_users(PyObject *self, PyObject *args) {
    PyObject *py_retlist = PyList_New(0);
    PyObject *py_tuple = NULL;

    if (py_retlist == NULL)
        return NULL;

    struct utmp ut;
    FILE *fp;

    fp = fopen(_PATH_UTMP, "r");
    if (fp == NULL) {
        PyErr_SetFromErrno(PyExc_OSError);
        goto error;
    }

    while (fread(&ut, sizeof(ut), 1, fp) == 1) {
        if (*ut.ut_name == '\0')
            continue;
        py_tuple = Py_BuildValue(
            "(sssf)",
            ut.ut_name,         // username
            ut.ut_line,         // tty
            ut.ut_host,         // hostname
           (float)ut.ut_time);  // start time
        if (!py_tuple) {
            fclose(fp);
            goto error;
        }
        if (PyList_Append(py_retlist, py_tuple)) {
            fclose(fp);
            goto error;
        }
        Py_DECREF(py_tuple);
    }

    fclose(fp);

    return py_retlist;

error:
    Py_XDECREF(py_tuple);
    Py_DECREF(py_retlist);
    return NULL;
}



/*
 * System-wide open connections.
 */
#if 0
#define HASHSIZE 1009
static struct xfile *psutil_xfiles;
static int psutil_nxfiles;

int
psutil_populate_xfiles()
{
    size_t len;

    if ((psutil_xfiles = malloc(len = sizeof *psutil_xfiles)) == NULL) {
        PyErr_NoMemory();
        return 0;
    }
    while (sysctlbyname("kern.file", psutil_xfiles, &len, 0, 0) == -1) {
        if (errno != ENOMEM) {
            PyErr_SetFromErrno(0);
            return 0;
        }
        len *= 2;
        if ((psutil_xfiles = realloc(psutil_xfiles, len)) == NULL) {
            PyErr_NoMemory();
            return 0;
        }
    }
    if (len > 0 && psutil_xfiles->xf_size != sizeof *psutil_xfiles) {
        PyErr_Format(PyExc_RuntimeError, "struct xfile size mismatch");
        return 0;
    }
    psutil_nxfiles = len / sizeof *psutil_xfiles;
    return 1;
}

int
psutil_get_pid_from_sock(int sock_hash)
{
    struct xfile *xf;
    int hash, n;
    for (xf = psutil_xfiles, n = 0; n < psutil_nxfiles; ++n, ++xf) {
        if (xf->xf_data == NULL)
            continue;
        hash = (int)((uintptr_t)xf->xf_data % HASHSIZE);
        if (sock_hash == hash) {
            return xf->xf_pid;
        }
    }
    return -1;
}

int psutil_gather_inet(int proto, PyObject *py_retlist)
{
    struct xinpgen *xig, *exig;
    struct xinpcb *xip;
    struct xtcpcb *xtp;
    struct inpcb *inp;
    struct xsocket *so;
    struct sock *sock;
    const char *varname;
    size_t len, bufsize;
    void *buf;
    int hash, retry, vflag, type;

    PyObject *py_tuple = NULL;
    PyObject *py_laddr = NULL;
    PyObject *py_raddr = NULL;

    switch (proto) {
        case IPPROTO_TCP:
            varname = "net.inet.tcp.pcblist";
            type = SOCK_STREAM;
            break;
        case IPPROTO_UDP:
            varname = "net.inet.udp.pcblist";
            type = SOCK_DGRAM;
            break;
    }

    buf = NULL;
    bufsize = 8192;
    retry = 5;
    do {
        for (;;) {
            buf = realloc(buf, bufsize);
            if (buf == NULL) {
                // XXX
                continue;
            }
            len = bufsize;
            if (sysctlbyname(varname, buf, &len, NULL, 0) == 0)
                break;
            if (errno != ENOMEM) {
                PyErr_SetFromErrno(0);
                goto error;
            }
            bufsize *= 2;
        }
        xig = (struct xinpgen *)buf;
        exig = (struct xinpgen *)(void *)((char *)buf + len - sizeof *exig);
        if (xig->xig_len != sizeof *xig || exig->xig_len != sizeof *exig) {
            PyErr_Format(PyExc_RuntimeError, "struct xinpgen size mismatch");
            goto error;
        }
    } while (xig->xig_gen != exig->xig_gen && retry--);


    for (;;) {
        xig = (struct xinpgen *)(void *)((char *)xig + xig->xig_len);
        if (xig >= exig)
            break;

        switch (proto) {
        case IPPROTO_TCP:
            xtp = (struct xtcpcb *)xig;
            if (xtp->xt_len != sizeof *xtp) {
                PyErr_Format(PyExc_RuntimeError, "struct xtcpcb size mismatch");
                goto error;
            }
            break;
        case IPPROTO_UDP:
            xip = (struct xinpcb *)xig;
            if (xip->xi_len != sizeof *xip) {
                PyErr_Format(PyExc_RuntimeError, "struct xinpcb size mismatch");
                goto error;
            }
            inp = &xip->xi_inp;
            so = &xip->xi_socket;
            break;
        }

        inp = &xtp->xt_inp;
        so = &xtp->xt_socket;
        char lip[200], rip[200];
        int family, lport, rport, pid, status;

        hash = (int)((uintptr_t)so->xso_so % HASHSIZE);
        pid = psutil_get_pid_from_sock(hash);
        if (pid < 0)
            continue;
        lport = ntohs(inp->inp_lport);
        rport = ntohs(inp->inp_fport);
        status = xtp->xt_tp.t_state;

        if (inp->inp_vflag & INP_IPV4) {
            family = AF_INET;
            inet_ntop(AF_INET, &inp->inp_laddr.s_addr, lip, sizeof(lip));
            inet_ntop(AF_INET, &inp->inp_faddr.s_addr, rip, sizeof(rip));
        }
        else if (inp->inp_vflag & INP_IPV6) {
            family = AF_INET6;
            inet_ntop(AF_INET6, &inp->in6p_laddr.s6_addr, lip, sizeof(lip));
            inet_ntop(AF_INET6, &inp->in6p_faddr.s6_addr, rip, sizeof(rip));
        }

        // construct python tuple/list
        py_laddr = Py_BuildValue("(si)", lip, lport);
        if (!py_laddr)
            goto error;
        if (rport != 0) {
            py_raddr = Py_BuildValue("(si)", rip, rport);
        }
        else {
            py_raddr = Py_BuildValue("()");
        }
        if (!py_raddr)
            goto error;
        tuple = Py_BuildValue("(iiiNNii)", -1, family, type, py_laddr,
                              py_raddr, status, pid);
        if (!tuple)
            goto error;
        if (PyList_Append(py_retlist, tuple))
            goto error;
        Py_DECREF(tuple);
  }

    free(buf);
    return 1;

error:
    Py_XDECREF(py_tuple);
    Py_XDECREF(py_laddr);
    Py_XDECREF(py_raddr);
    free(buf);
    return 0;
}


int psutil_gather_unix(int proto, PyObject *py_retlist)
{
    struct xunpgen *xug, *exug;
    struct xunpcb *xup;
    struct sock *sock;
    const char *varname, *protoname;
    size_t len, bufsize;
    void *buf;
    int hash, retry;
    int family, lport, rport, pid;
    struct sockaddr_un *sun;
    char path[PATH_MAX];

    PyObject *py_tuple = NULL;
    PyObject *py_laddr = NULL;
    PyObject *py_raddr = NULL;

    switch (proto) {
        case SOCK_STREAM:
            varname = "net.local.stream.pcblist";
            protoname = "stream";
            break;
        case SOCK_DGRAM:
            varname = "net.local.dgram.pcblist";
            protoname = "dgram";
            break;
    }

    buf = NULL;
    bufsize = 8192;
    retry = 5;

    do {
        for (;;) {
            buf = realloc(buf, bufsize);
            if (buf == NULL) {
                PyErr_NoMemory();
                goto error;
            }
            len = bufsize;
            if (sysctlbyname(varname, buf, &len, NULL, 0) == 0)
                break;
            if (errno != ENOMEM) {
                PyErr_SetFromErrno(0);
                goto error;
            }
            bufsize *= 2;
        }
        xug = (struct xunpgen *)buf;
        exug = (struct xunpgen *)(void *)
            ((char *)buf + len - sizeof *exug);
        if (xug->xug_len != sizeof *xug || exug->xug_len != sizeof *exug) {
            PyErr_Format(PyExc_RuntimeError, "struct xinpgen size mismatch");
            goto error;
        }
    } while (xug->xug_gen != exug->xug_gen && retry--);

    for (;;) {
        xug = (struct xunpgen *)(void *)((char *)xug + xug->xug_len);
        if (xug >= exug)
            break;
        xup = (struct xunpcb *)xug;
        if (xup->xu_len != sizeof *xup) {
            warnx("struct xunpgen size mismatch");
            goto error;
        }

        hash = (int)((uintptr_t) xup->xu_socket.xso_so % HASHSIZE);
        pid = psutil_get_pid_from_sock(hash);
        if (pid < 0)
            continue;

        sun = (struct sockaddr_un *)&xup->xu_addr;
        snprintf(path, sizeof(path), "%.*s",
                 (sun->sun_len - (sizeof(*sun) - sizeof(sun->sun_path))),
                 sun->sun_path);

        py_tuple = Py_BuildValue(
            "(iiisOii)", -1, AF_UNIX, proto, path, Py_None,
            PSUTIL_CONN_NONE, pid);
        if (!py_tuple)
            goto error;
        if (PyList_Append(py_retlist, py_tuple))
            goto error;
        Py_DECREF(tuple);
        Py_INCREF(Py_None);
    }

    free(buf);
    return 1;

error:
    Py_XDECREF(py_tuple);
    Py_XDECREF(py_laddr);
    Py_XDECREF(py_raddr);
    free(buf);
    return 0;
}


/*
 * Return system-wide open connections.
 */
static PyObject*
psutil_net_connections(PyObject* self, PyObject* args)
{
    PyObject *py_af_filter = NULL;
    PyObject *py_type_filter = NULL;
    PyObject *py_retlist = PyList_New(0);

    if (py_retlist == NULL)
        return NULL;
    if (psutil_populate_xfiles() != 1)
        goto error;
    if (psutil_gather_inet(IPPROTO_TCP, py_retlist) == 0)
        goto error;
    if (psutil_gather_inet(IPPROTO_UDP, py_retlist) == 0)
        goto error;
    if (psutil_gather_unix(SOCK_STREAM, py_retlist) == 0)
       goto error;
    if (psutil_gather_unix(SOCK_DGRAM, py_retlist) == 0)
        goto error;

//    free(psutil_xfiles);
    return py_retlist;

error:
    Py_DECREF(py_retlist);
//    free(psutil_xfiles);
    return NULL;
}

#endif

/*
 * define the psutil C module methods and initialize the module.
 */
static PyMethodDef
PsutilMethods[] = {
    // --- per-process functions

    {"proc_name", psutil_proc_name, METH_VARARGS,
     "Return process name"},
    {"proc_connections", psutil_proc_connections, METH_VARARGS,
     "Return connections opened by process"},
    {"proc_cmdline", psutil_proc_cmdline, METH_VARARGS,
     "Return process cmdline as a list of cmdline arguments"},
    {"proc_ppid", psutil_proc_ppid, METH_VARARGS,
     "Return process ppid as an integer"},
    {"proc_uids", psutil_proc_uids, METH_VARARGS,
     "Return process real effective and saved user ids as a Python tuple"},
    {"proc_gids", psutil_proc_gids, METH_VARARGS,
     "Return process real effective and saved group ids as a Python tuple"},
    {"proc_cpu_times", psutil_proc_cpu_times, METH_VARARGS,
     "Return tuple of user/kern time for the given PID"},
    {"proc_create_time", psutil_proc_create_time, METH_VARARGS,
     "Return a float indicating the process create time expressed in "
     "seconds since the epoch"},
    {"proc_memory_info", psutil_proc_memory_info, METH_VARARGS,
     "Return extended memory info for a process as a Python tuple."},
    {"proc_num_ctx_switches", psutil_proc_num_ctx_switches, METH_VARARGS,
     "Return the number of context switches performed by process"},
    {"proc_threads", psutil_proc_threads, METH_VARARGS,
     "Return process threads"},
    {"proc_status", psutil_proc_status, METH_VARARGS,
     "Return process status as an integer"},
    {"proc_io_counters", psutil_proc_io_counters, METH_VARARGS,
     "Return process IO counters"},
    {"proc_tty_nr", psutil_proc_tty_nr, METH_VARARGS,
     "Return process tty (terminal) number"},
    {"proc_cwd", psutil_proc_cwd, METH_VARARGS,
     "Return process current working directory."},
    {"proc_open_files", psutil_proc_open_files, METH_VARARGS,
     "Return files opened by process as a list of (path, fd) tuples"},
    {"proc_num_fds", psutil_proc_num_fds, METH_VARARGS,
     "Return the number of file descriptors opened by this process"},
#ifdef __FreeBSD__
    {"proc_exe", psutil_proc_exe, METH_VARARGS,
     "Return process pathname executable"},
    {"proc_num_threads", psutil_proc_num_threads, METH_VARARGS,
     "Return number of threads used by process"},
    {"proc_memory_maps", psutil_proc_memory_maps, METH_VARARGS,
     "Return a list of tuples for every process's memory map"},
    {"proc_cpu_affinity_get", psutil_proc_cpu_affinity_get, METH_VARARGS,
     "Return process CPU affinity."},
    {"proc_cpu_affinity_set", psutil_proc_cpu_affinity_set, METH_VARARGS,
     "Set process CPU affinity."},
    {"cpu_count_phys", psutil_cpu_count_phys, METH_VARARGS,
     "Return an XML string to determine the number physical CPUs."},
#if defined(__FreeBSD_version) && __FreeBSD_version >= 800000
    {"proc_open_files", psutil_proc_open_files, METH_VARARGS,
     "Return files opened by process as a list of (path, fd) tuples"},
#endif
#endif

    // --- system-related functions

    {"pids", psutil_pids, METH_VARARGS,
     "Returns a list of PIDs currently running on the system"},
    {"cpu_count_logical", psutil_cpu_count_logical, METH_VARARGS,
     "Return number of logical CPUs on the system"},
    {"virtual_mem", psutil_virtual_mem, METH_VARARGS,
     "Return system virtual memory usage statistics"},
    {"swap_mem", psutil_swap_mem, METH_VARARGS,
     "Return swap mem stats"},
    {"cpu_times", psutil_cpu_times, METH_VARARGS,
     "Return system cpu times as a tuple (user, system, nice, idle, irc)"},
    {"per_cpu_times", psutil_per_cpu_times, METH_VARARGS,
     "Return system per-cpu times as a list of tuples"},
    {"boot_time", psutil_boot_time, METH_VARARGS,
     "Return the system boot time expressed in seconds since the epoch."},
    {"disk_partitions", psutil_disk_partitions, METH_VARARGS,
     "Return a list of tuples including device, mount point and "
     "fs type for all partitions mounted on the system."},
    {"net_io_counters", psutil_net_io_counters, METH_VARARGS,
     "Return dict of tuples of networks I/O information."},
    {"disk_io_counters", psutil_disk_io_counters, METH_VARARGS,
     "Return a Python dict of tuples for disk I/O information"},
    {"users", psutil_users, METH_VARARGS,
     "Return currently connected users as a list of tuples"},
#if 0
    {"net_connections", psutil_net_connections, METH_VARARGS,
     "Return system-wide open connections."},
#endif
    {NULL, NULL, 0, NULL}
};

struct module_state {
    PyObject *error;
};

#if PY_MAJOR_VERSION >= 3
#define GETSTATE(m) ((struct module_state*)PyModule_GetState(m))
#else
#define GETSTATE(m) (&_state)
#endif

#if PY_MAJOR_VERSION >= 3

static int
psutil_bsd_traverse(PyObject *m, visitproc visit, void *arg) {
    Py_VISIT(GETSTATE(m)->error);
    return 0;
}

static int
psutil_bsd_clear(PyObject *m) {
    Py_CLEAR(GETSTATE(m)->error);
    return 0;
}

static struct PyModuleDef
        moduledef = {
    PyModuleDef_HEAD_INIT,
    "psutil_bsd",
    NULL,
    sizeof(struct module_state),
    PsutilMethods,
    NULL,
    psutil_bsd_traverse,
    psutil_bsd_clear,
    NULL
};

#define INITERROR return NULL

PyMODINIT_FUNC PyInit__psutil_bsd(void)

#else
#define INITERROR return

void init_psutil_bsd(void)
#endif
{
#if PY_MAJOR_VERSION >= 3
    PyObject *module = PyModule_Create(&moduledef);
#else
    PyObject *module = Py_InitModule("_psutil_bsd", PsutilMethods);
#endif
    PyModule_AddIntConstant(module, "version", PSUTIL_VERSION);
    // process status constants

#ifdef __FreeBSD__
    PyModule_AddIntConstant(module, "SIDL", SIDL);
    PyModule_AddIntConstant(module, "SRUN", SRUN);
    PyModule_AddIntConstant(module, "SSLEEP", SSLEEP);
    PyModule_AddIntConstant(module, "SSTOP", SSTOP);
    PyModule_AddIntConstant(module, "SZOMB", SZOMB);
    PyModule_AddIntConstant(module, "SWAIT", SWAIT);
    PyModule_AddIntConstant(module, "SLOCK", SLOCK);
#elif  __OpenBSD__
    PyModule_AddIntConstant(module, "SIDL", SIDL);
    PyModule_AddIntConstant(module, "SRUN", SRUN);
    PyModule_AddIntConstant(module, "SSLEEP", SSLEEP);
    PyModule_AddIntConstant(module, "SSTOP", SSTOP);
    PyModule_AddIntConstant(module, "SZOMB", SZOMB);  // unused
    PyModule_AddIntConstant(module, "SDEAD", SDEAD);
    PyModule_AddIntConstant(module, "SONPROC", SONPROC);
#endif

    // connection status constants
    PyModule_AddIntConstant(module, "TCPS_CLOSED", TCPS_CLOSED);
    PyModule_AddIntConstant(module, "TCPS_CLOSING", TCPS_CLOSING);
    PyModule_AddIntConstant(module, "TCPS_CLOSE_WAIT", TCPS_CLOSE_WAIT);
    PyModule_AddIntConstant(module, "TCPS_LISTEN", TCPS_LISTEN);
    PyModule_AddIntConstant(module, "TCPS_ESTABLISHED", TCPS_ESTABLISHED);
    PyModule_AddIntConstant(module, "TCPS_SYN_SENT", TCPS_SYN_SENT);
    PyModule_AddIntConstant(module, "TCPS_SYN_RECEIVED", TCPS_SYN_RECEIVED);
    PyModule_AddIntConstant(module, "TCPS_FIN_WAIT_1", TCPS_FIN_WAIT_1);
    PyModule_AddIntConstant(module, "TCPS_FIN_WAIT_2", TCPS_FIN_WAIT_2);
    PyModule_AddIntConstant(module, "TCPS_LAST_ACK", TCPS_LAST_ACK);
    PyModule_AddIntConstant(module, "TCPS_TIME_WAIT", TCPS_TIME_WAIT);
    PyModule_AddIntConstant(module, "PSUTIL_CONN_NONE", 128); /*PSUTIL_CONN_NONE */

    if (module == NULL)
        INITERROR;
#if PY_MAJOR_VERSION >= 3
    return module;
#endif
}