#pragma once

#include <stdlib.h>
#include <unordered_set>
#include <string>
#include <atomic>
#include <mutex>

//result of a single test run: 1 model with 1 test dataset
enum class EXECUTE_RESULT {
  SUCCESS = 0,
  UNKNOWN_ERROR = -1,
  WITH_EXCEPTION = -2,
  RESULT_DIFFERS = -3,
  SHAPE_MISMATCH = -4,
  TYPE_MISMATCH = -5,
  NOT_SUPPORT = -6,
  LOAD_MODEL_FAILED = -7,
  INVALID_GRAPH = -8
};

class TestResultStat {
 public:
  size_t total_test_case_count = 0;
  std::atomic_int succeeded;
  std::atomic_int not_implemented;
  std::atomic_int load_model_failed;
  std::atomic_int invalid_graph;
  std::atomic_int throwed_exception;
  std::atomic_int result_differs;
  std::atomic_int skipped;

  TestResultStat() : succeeded(0), not_implemented(0), load_model_failed(0), throwed_exception(0), result_differs(0), skipped(0), invalid_graph(0) {}

  void AddNotImplementedKernels(const std::string& s) {
    std::lock_guard<std::mutex> l(m_);
    not_implemented_kernels.insert(s);
  }

  void AddFailedKernels(const std::string& s) {
    std::lock_guard<std::mutex> l(m_);
    failed_kernels.insert(s);
  }

  void AddFailedTest(const std::string& s) {
    std::lock_guard<std::mutex> l(m_);
    failed_test_cases.insert(s);
  }

  std::string ToString();

 private:
  std::mutex m_;
  std::unordered_set<std::string> not_implemented_kernels;
  std::unordered_set<std::string> failed_kernels;
  std::unordered_set<std::string> failed_test_cases;
};
