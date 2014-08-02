#ifndef INC_METTLE_FORKED_TEST_RUNNER_HPP
#define INC_METTLE_FORKED_TEST_RUNNER_HPP

#include <chrono>

#include <mettle/compiled_suite.hpp>
#include <mettle/log/core.hpp>

#include "../optional.hpp"

namespace mettle {

class forked_test_runner {
private:
  using timeout_t = METTLE_OPTIONAL_NS::optional<std::chrono::milliseconds>;
public:
  forked_test_runner(timeout_t timeout = timeout_t()) : timeout_(timeout) {}

  template<class Rep, class Period>
  forked_test_runner(std::chrono::duration<Rep, Period> timeout)
    : timeout_(timeout) {}

  test_result
  operator ()(const test_function &test, log::test_output &output) const;
private:
  void fork_watcher(std::chrono::milliseconds timeout) const;

  timeout_t timeout_;
};

} // namespace mettle

#endif
