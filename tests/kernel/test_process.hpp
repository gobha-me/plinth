#pragma once

#include <functional>

namespace plinth::test_process {

// Register an explicit process-level test-fixture shutdown action. The custom
// Catch2 main runs actions in reverse registration order after Session::run()
// and before any static-storage destructor executes.
auto register_shutdown(std::function<void()> action) -> void;

} // namespace plinth::test_process
