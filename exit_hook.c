#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/syscall.h>
#include <signal.h>
#include <fcntl.h>
#include <stdarg.h>

static int s_hook_init = 0;
static int s_dumping = 0;  // re-entrancy guard

typedef void (*real_exit_t)(int);
typedef void (*real__exit_t)(int);
typedef void (*sig_handler_t)(int);
typedef void (*sig_sigaction_t)(int, siginfo_t*, void*);
typedef int (*real_sigaction_t)(int, struct sigaction const*, struct sigaction*);

static real_exit_t       real_exit_fn;
static real__exit_t      real__exit_fn;
static real_sigaction_t  real_sigaction_fn;
static sig_sigaction_t   real_sigsegv;   // app's handler (sa_sigaction or sa_handler)
static sig_sigaction_t   real_sigabrt;
static int               real_sigsegv_is_sigaction;  // 1 if app used SA_SIGINFO

// Signal-safe log write. No malloc, no stdio, no locks.
static void raw_logf(char const* fmt, ...)
{
    int fd = open("/azerothcore/env/dist/logs/exit_hook.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    va_list ap;
    va_start(ap, fmt);
    vdprintf(fd, fmt, ap);
    va_end(ap);
    close(fd);
}

static void dump_backtrace(void)
{
    if (s_dumping) return;  // re-entrancy guard
    s_dumping = 1;

    void* buffer[128];
    int nptrs = backtrace(buffer, 128);

    time_t t;
    time(&t);

    raw_logf("\n=== EXIT_HOOK %s ===\n", ctime(&t));  // ctime includes \n
    raw_logf("Backtrace (%d frames):\n", nptrs);

    // backtrace_symbols_fd writes to a raw fd, no malloc.
    int fd = open("/azerothcore/env/dist/logs/exit_hook.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0)
    {
        backtrace_symbols_fd(buffer, nptrs, fd);
        write(fd, "\n", 1);
        close(fd);
    }

    raw_logf("--- dladdr resolves ---\n");
    for (int i = 0; i < nptrs; i++)
    {
        Dl_info info;
        if (dladdr(buffer[i], &info))
            raw_logf("  #%d: %p %s (%s)\n", i, buffer[i],
                     info.dli_sname ? info.dli_sname : "?",
                     info.dli_fname ? info.dli_fname : "?");
    }
    raw_logf("==========================\n\n");
    s_dumping = 0;
}

// Old-style handler (signal() / sigaction without SA_SIGINFO)
static void signal_handler(int sig)
{
    raw_logf("CAUGHT SIGNAL %d (%s)\n", sig,
             sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : "other");
    dump_backtrace();

    sig_sigaction_t real = (sig == SIGSEGV) ? real_sigsegv : real_sigabrt;
    if (real && real != SIG_IGN && real != SIG_DFL)
    {
        // restore default so a second crash generates a proper core
        signal(sig, SIG_DFL);
        real(sig, NULL, NULL);  // call the app's handler
    }
    else
    {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

// SA_SIGINFO-style handler (sigaction with SA_SIGINFO)
static void signal_siginfo_handler(int sig, siginfo_t* info, void* ctx)
{
    raw_logf("CAUGHT SIGNAL %d (%s) si_code=%d si_addr=%p\n",
             sig,
             sig == SIGSEGV ? "SIGSEGV" : sig == SIGABRT ? "SIGABRT" : "other",
             info ? info->si_code : -1,
             info ? info->si_addr : 0);
    dump_backtrace();

    sig_sigaction_t real = (sig == SIGSEGV) ? real_sigsegv : real_sigabrt;
    if (real && real != SIG_IGN && real != SIG_DFL)
    {
        signal(sig, SIG_DFL);
        real(sig, info, ctx);
    }
    else
    {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void init_hook(void)
{
    if (s_hook_init) return;
    s_hook_init = 1;

    real_exit_fn      = (real_exit_t)dlsym(RTLD_NEXT, "exit");
    real__exit_fn     = (real__exit_t)dlsym(RTLD_NEXT, "_exit");
    real_sigaction_fn = (real_sigaction_t)dlsym(RTLD_NEXT, "sigaction");

    // Seed our handlers so a crash before the app installs its own is caught.
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_sigaction = signal_siginfo_handler;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    real_sigaction_fn(SIGSEGV, &act, NULL);
    real_sigaction_fn(SIGABRT, &act, NULL);
}

int sigaction(int signum, struct sigaction const* act, struct sigaction* oldact)
{
    init_hook();
    if (!real_sigaction_fn) return -1;

    if (signum == SIGSEGV || signum == SIGABRT)
    {
        // Store what the app wants
        if (act)
        {
            if (signum == SIGSEGV)
            {
                real_sigsegv = act->sa_flags & SA_SIGINFO
                    ? (sig_sigaction_t)act->sa_sigaction
                    : (sig_sigaction_t)act->sa_handler;
                real_sigsegv_is_sigaction = !!(act->sa_flags & SA_SIGINFO);
            }
            else
                real_sigabrt = act->sa_flags & SA_SIGINFO
                    ? (sig_sigaction_t)act->sa_sigaction
                    : (sig_sigaction_t)act->sa_handler;
        }
        else
        {
            if (signum == SIGSEGV) { real_sigsegv = NULL; real_sigsegv_is_sigaction = 0; }
            else real_sigabrt = NULL;
        }

        // Install OUR handler with SA_SIGINFO always on so we get full details
        struct sigaction our_act;
        memset(&our_act, 0, sizeof(our_act));
        our_act.sa_sigaction = signal_siginfo_handler;
        our_act.sa_flags = SA_SIGINFO;
        sigemptyset(&our_act.sa_mask);

        struct sigaction tmp_old;
        int ret = real_sigaction_fn(signum, &our_act, &tmp_old);
        if (oldact) *oldact = tmp_old;
        return ret;
    }

    return real_sigaction_fn(signum, act, oldact);
}

sighandler_t signal(int signum, sighandler_t handler)
{
    init_hook();

    if (signum == SIGSEGV || signum == SIGABRT)
    {
        sighandler_t prev = signal_handler;  // will be overwritten if sigaction reveals old
        struct sigaction act, old;
        memset(&act, 0, sizeof(act));
        act.sa_handler = handler;
        sigemptyset(&act.sa_mask);
        if (sigaction(signum, &act, &old) == 0)
            prev = old.sa_handler;
        return prev;
    }

    typedef sighandler_t (*signal_fn_t)(int, sighandler_t);
    signal_fn_t real_signal_fn = (signal_fn_t)dlsym(RTLD_NEXT, "signal");
    return real_signal_fn ? real_signal_fn(signum, handler) : SIG_ERR;
}

static void raw_exit(int status)
{
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}

void _exit(int status)
{
    init_hook();
    dump_backtrace();
    if (real__exit_fn) real__exit_fn(status);
    else raw_exit(status);
}

void exit(int status)
{
    init_hook();
    dump_backtrace();
    if (real_exit_fn) real_exit_fn(status);
    else raw_exit(status);
}

__attribute__((constructor))
static void init(void)
{
    atexit(dump_backtrace);
    init_hook();
}
