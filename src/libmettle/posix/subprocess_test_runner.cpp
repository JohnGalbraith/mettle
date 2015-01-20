#include "subprocess_test_runner.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

#include <mettle/driver/posix/scoped_pipe.hpp>
#include <mettle/driver/posix/scoped_signal.hpp>
#include <mettle/driver/posix/test_monitor.hpp>
#include "../../utils.hpp"

namespace mettle {

namespace {
  pid_t test_pgid = 0;
  struct sigaction old_sigint, old_sigquit;

  void sig_handler(int signum) {
    assert(test_pgid != 0);
    kill(-test_pgid, signum);

    // Restore the previous signal action and re-raise the signal.
    struct sigaction *old_act = signum == SIGINT ? &old_sigint : &old_sigquit;
    sigaction(signum, old_act, nullptr);
    raise(signum);
  }

  void sig_chld(int) {}

  inline test_result parent_failed() {
    test_pgid = 0;
    return { false, err_string(errno) };
  }

  [[noreturn]] inline void child_failed() {
    _exit(128);
  }
}

test_result subprocess_test_runner::operator ()(
  const test_info &test, log::test_output &output
) const {
  scoped_pipe stdout_pipe, stderr_pipe, log_pipe;
  if(stdout_pipe.open() < 0 ||
     stderr_pipe.open() < 0 ||
     log_pipe.open(O_CLOEXEC) < 0)
    return parent_failed();

  fflush(nullptr);

  scoped_sigprocmask mask;
  if(mask.push(SIG_BLOCK, SIGCHLD) < 0 ||
     mask.push(SIG_BLOCK, {SIGINT, SIGQUIT}) < 0)
    return parent_failed();

  pid_t pid;
  if((pid = fork()) < 0)
    return parent_failed();

  if(pid == 0) {
    if(mask.clear() < 0)
      child_failed();

    // Make a new process group so we can kill the test and all its children
    // as a group.
    setpgid(0, 0);

    if(timeout_)
      fork_monitor(*timeout_);

    if(stdout_pipe.close_read() < 0 ||
       stderr_pipe.close_read() < 0 ||
       log_pipe.close_read() < 0)
      child_failed();

    if(stdout_pipe.move_write(STDOUT_FILENO) < 0 ||
       stderr_pipe.move_write(STDERR_FILENO) < 0)
      child_failed();

    auto result = test.function();
    if(write(log_pipe.write_fd, result.message.c_str(),
             result.message.length()) < 0)
      child_failed();

    fflush(nullptr);

    _exit(!result.passed);
  }
  else {
    scoped_signal sigint, sigquit, sigchld;
    test_pgid = pid;

    if(sigaction(SIGINT, nullptr, &old_sigint) < 0 ||
       sigaction(SIGQUIT, nullptr, &old_sigquit) < 0)
      return parent_failed();

    if(sigint.open(SIGINT, sig_handler) < 0 ||
       sigquit.open(SIGQUIT, sig_handler) < 0 ||
       sigchld.open(SIGCHLD, sig_chld) < 0)
      return parent_failed();

    if(mask.pop() < 0)
      return parent_failed();

    if(stdout_pipe.close_write() < 0 ||
       stderr_pipe.close_write() < 0 ||
       log_pipe.close_write() < 0)
      return parent_failed();

    std::string message;
    std::vector<readfd> dests = {
      {stdout_pipe.read_fd, &output.stdout_log},
      {stderr_pipe.read_fd, &output.stderr_log},
      {log_pipe.read_fd,    &message}
    };

    // Read from the piped stdout, stderr, and log. If we're interrupted
    // (probably by SIGCHLD), do one last non-blocking read to get any data we
    // might have missed.
    sigset_t empty;
    sigemptyset(&empty);
    if(read_into(dests, nullptr, &empty) < 0) {
      if(errno != EINTR)
        return parent_failed();
      timespec timeout = {0, 0};
      if(read_into(dests, &timeout, nullptr) < 0)
        return parent_failed();
    }

    int status;
    if(waitpid(pid, &status, 0) < 0)
      return parent_failed();

    // Make sure everything in the test's process group is dead. Don't worry
    // about reaping.
    kill(-pid, SIGKILL);
    test_pgid = 0;

    if(WIFEXITED(status)) {
      int exit_code = WEXITSTATUS(status);
      if(exit_code == err_timeout) {
        std::ostringstream ss;
        ss << "Timed out after " << timeout_->count() << " ms";
        return { false, ss.str() };
      }
      else {
        return { exit_code == 0, message };
      }
    }
    else { // WIFSIGNALED
      return { false, strsignal(WTERMSIG(status)) };
    }
  }
}

} // namespace mettle