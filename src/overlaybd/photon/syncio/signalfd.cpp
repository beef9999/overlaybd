/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include "signal.h"
#include <unistd.h>
#include <assert.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <unistd.h>
#include "fd-events.h"
#include "../thread.h"
#include "../../event-loop.h"
#include "../../utility.h"
#include "../../alog.h"

extern "C"
{
    int __register_atfork(
        void (*prepare) (void),
        void (*parent)  (void),
        void (*child)   (void),
        void *__dso_handle);

    void __unregister_atfork(void *__dso_handle);
}

namespace photon
{
    static constexpr int SIGNAL_MAX = 64;

    static int sgfd = -1;
    static void* sighandlers[SIGNAL_MAX + 1];
    static sigset_t infoset = {0};
    static sigset_t sigset = {-1UL};
    static EventLoop* eloop = nullptr;

    static int set_signal_mask()
    {
        if (sigprocmask(SIG_SETMASK, &sigset, NULL) == -1)
            LOG_ERRNO_RETURN(0, -1, "failed to set sigprocmask()");
        return 0;
    }
    int block_all_signal() {
        sigfillset(&sigset);
        return set_signal_mask();
    }
    static int clear_signal_mask()
    {
        sigemptyset(&sigset);
        return set_signal_mask();
    }
    static int update_signal_mask(int signum, void* oldh, void* newh)
    {
        if ((bool)oldh == (bool)newh)
            return 0;

        if (newh) {
            sigaddset(&sigset, signum);
        } else /*if (oldh)*/ {
            sigdelset(&sigset, signum);
        }
        return set_signal_mask();
    }

    sighandler_t sync_signal(int signum, sighandler_t handler)
    {
        if (signum > SIGNAL_MAX)
            LOG_ERROR_RETURN(EINVAL, nullptr, "signal number ` too big (` maximum)", signum, SIGNAL_MAX);

        auto h = sighandlers[signum];
        sighandlers[signum] = (void*)handler;
        sigdelset(&infoset, signum);
        update_signal_mask(signum, h, (void*)handler);
        return (sighandler_t)h;
    }

    int sync_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
    {
        if (signum > SIGNAL_MAX)
            LOG_ERROR_RETURN(EINVAL, -1, "signal number ` too big (` maximum)", signum, SIGNAL_MAX);

        auto h = sighandlers[signum];
        if (oldact)
        {
            oldact->sa_mask = sigset;
            oldact->sa_flags = SA_RESTART;
            if (sigismember(&infoset, signum))
                oldact->sa_flags |= SA_SIGINFO;
            (void*&)oldact->sa_sigaction = h;
        }

        if (act->sa_flags & SA_SIGINFO) {
            sighandlers[signum] = (void*)act->sa_sigaction;
            sigaddset(&infoset, signum);
        } else {
            sighandlers[signum] = (void*)act->sa_handler;
            sigdelset(&infoset, signum);
        }
        update_signal_mask(signum, h, sighandlers[signum]);
        return 0;
    }

    static int wait_for_signal(void*, EventLoop*)
    {
        // for somehow EventLoop use 0 to present no events
        // and -1 as exit
        // but in fd-event, 0 just means done wait without error
        if (wait_for_fd_readable(sgfd)< 0) {
            ERRNO err;
            if (err.no == ETIMEDOUT) {
                // it might be timedout
                // means no events, continue
                LOG_DEBUG("timeout during wait for signal ", err);
                return 0;
            } else {// means error or terminated and need to be shutdown
                LOG_DEBUG("wait for signalfd failed because `, stop watching", err);
                return -1;
            }
        }
        return 1;
    }

    static int fire_signal(void*, EventLoop*)
    {
        struct signalfd_siginfo fdsi;
        ssize_t ret = read(sgfd, &fdsi, sizeof(fdsi));
        if (ret != sizeof(fdsi))
        {
            if (ret < 0)
                LOG_ERRNO_RETURN(0, 0, "SignalFD read failed");
            if (ret == 0) {
                LOG_ERROR_RETURN(0, 0, "SignalFD readable happend but nothing to read");
            } else {
                LOG_ERROR_RETURN(0, 0, "SignalFD partial read");
            }
        }

        auto no = fdsi.ssi_signo;
        if (no > SIGNAL_MAX)
            LOG_ERROR_RETURN(EINVAL, -1, "signal number ` too big (` maximum)", no, SIGNAL_MAX);

        auto h = sighandlers[no];
        if (h == nullptr)
            return 0;

        if (!sigismember(&infoset, no))
        {
            ((sighandler_t)h)(no);
            return 0;
        }

        siginfo_t siginfo;
        #define ASSIGN(field)   siginfo.si_##field = fdsi.ssi_##field
        ASSIGN(signo);
        ASSIGN(errno);
        ASSIGN(code);
        // ASSIGN(trapno);
        ASSIGN(pid);
        ASSIGN(uid);
        ASSIGN(status);
        ASSIGN(utime);
        ASSIGN(stime);
        // ASSIGN(value);
        ASSIGN(int);
        // ASSIGN(ptr);
        siginfo.si_ptr = (void*)fdsi.ssi_ptr;
        ASSIGN(overrun);
        // ASSIGN(timerid);
        // ASSIGN(addr);
        siginfo.si_addr = (void*)fdsi.ssi_addr;
        ASSIGN(band);
        ASSIGN(fd);
        // ASSIGN(addr_lsb);
        #undef ASSIGN
        reinterpret_cast<decltype(sigaction::sa_sigaction)>(h)(no, &siginfo, nullptr);
        return 0;
    }

    // should be invoked in child process after forked, to clear signal mask
    static void fork_hook_child(void)
    {
        LOG_DEBUG("Fork hook");
        sigset_t sigset0;       // can NOT use photon::clear_signal_mask(),
        sigemptyset(&sigset0);  // as memory may be shared with parent, when vfork()ed
        sigprocmask(SIG_SETMASK, &sigset0, NULL);
    }

    int sync_signal_init()
    {
        if (sgfd != -1)
            LOG_ERROR_RETURN(EALREADY, -1, "already inited");

        sgfd = signalfd(-1, &sigset, SFD_CLOEXEC | SFD_NONBLOCK);
        if (sgfd == -1)
            LOG_ERRNO_RETURN(0, -1, "failed to create signalfd()");

        eloop = new_event_loop(
            {nullptr, &wait_for_signal},
            {nullptr, &fire_signal});
        if (!eloop)
        {
            close(sgfd);
            sgfd = -1;
            LOG_ERROR_RETURN(EFAULT, -1, "failed to thread_create() for signal handling");
        }
        eloop->async_run();
        thread_yield(); // give a chance let eloop to execute do_wait
        __register_atfork(nullptr, nullptr, &fork_hook_child, (void*)&fork_hook_child);
        return clear_signal_mask();
    }

    int sync_signal_fini()
    {
        if (sgfd == -1)
            LOG_ERROR_RETURN(EALREADY, -1, "already finited");

        eloop->stop();
        close(sgfd);
        delete eloop;
        // __unregister_atfork((void*)&fork_hook_child);
        return clear_signal_mask();
    }
}
