#pragma once
#include <ATen/ATen.h>
#include <ATen/core/ivalue.h>
#include <ATen/Parallel.h>

#include <c10/core/thread_pool.h>

#include <algorithm>
#include <cstddef>
#include <exception>

#define INTRA_OP_PARALLEL

namespace at {
namespace internal {
// internal function to get access to intra-op thread pool from
// template parallel primitives (parallel_for, parallel_reduce)
CAFFE2_API TaskThreadPoolBase& _get_intraop_pool();

// internal utility function to mark master thread as in parallel
// region when executing parallel primitives
CAFFE2_API void _set_in_parallel_region(bool);

// Simulate OMP's omp_get_thread_num() by force-setting thread local
// task id as thread number when executing parallel primitives
CAFFE2_API void _set_thread_num(size_t thread_num);
CAFFE2_API void _unset_thread_num();
}

template <class F>
void parallel_for(
    const int64_t begin,
    const int64_t end,
    const int64_t grain_size,
    const F& f) {
  if (begin >= end) {
    return;
  }

  if (grain_size < 0) {
    throw std::runtime_error("Invalid begin, end or grain_size in parallel_for");
  }

  if (((end - begin) >= grain_size) && !in_parallel_region()) {
    // choose number of tasks based on grain size and number of threads;
    size_t chunk_size = divup((end - begin), get_num_threads());
    // make sure each task is at least grain_size size
    chunk_size = std::max((size_t)grain_size, chunk_size);
    size_t num_tasks = divup((end - begin), chunk_size);

    std::atomic_flag err_flag = ATOMIC_FLAG_INIT;
    std::exception_ptr eptr;

    auto task = [&](int64_t task_id, int64_t local_start, int64_t local_end) {
      internal::_set_thread_num(task_id);
      internal::_set_in_parallel_region(true);
      try {
        f(local_start, local_end);
      } catch (...) {
        if (!err_flag.test_and_set()) {
          eptr = std::current_exception();
        }
      }
      internal::_set_in_parallel_region(false);
      internal::_unset_thread_num();
    };

    // using shared_ptr to share ownership of the future with the lambda,
    // to ensure we don't destroy future while lambda is still
    // running in markCompleted
    std::vector<std::shared_ptr<ivalue::Future>> futures(num_tasks);
    for (size_t task_id = 1; task_id < num_tasks; ++task_id) {
      futures[task_id] = std::make_shared<ivalue::Future>();
      auto future_ptr = futures[task_id];
      int64_t local_start = begin + task_id * chunk_size;
      if (local_start < end) {
        int64_t local_end = std::min(end, (int64_t)(chunk_size + local_start));
        internal::_get_intraop_pool().run(
            // copy future_ptr, task_id, local_start, local_end
            [&, future_ptr, task_id, local_start, local_end]() {
          task(task_id, local_start, local_end);
          future_ptr->markCompleted(IValue());
        });
      } else {
        future_ptr->markCompleted(IValue());
      }
    }

    int64_t first_task_end = std::min(end, (int64_t)(chunk_size + begin));
    task(0, begin, first_task_end);

    // wait for all tasks to finish
    for (size_t task_id = 1; task_id < num_tasks; ++task_id) {
      futures[task_id]->wait();
    }

    if (eptr) {
      std::rethrow_exception(eptr);
    }
  } else {
    f(begin, end);
  }
}

template <class scalar_t, class F, class SF>
scalar_t parallel_reduce(
    const int64_t begin,
    const int64_t end,
    const int64_t grain_size,
    const scalar_t ident,
    const F f,
    const SF sf) {
  if (begin >= end) {
    return ident;
  }

  if (grain_size < 0) {
    throw std::runtime_error("Invalid begin, end or grain_size in parallel_reduce");
  }

  if (((end - begin) >= grain_size) && !in_parallel_region()) {
    size_t chunk_size = divup((end - begin), get_num_threads());
    chunk_size = std::max((size_t)grain_size, chunk_size);
    size_t num_tasks = divup((end - begin), chunk_size);
    std::vector<scalar_t> results(num_tasks);
    scalar_t* results_data = results.data();

    std::atomic_flag err_flag = ATOMIC_FLAG_INIT;
    std::exception_ptr eptr;

    auto task = [&](int64_t task_id, int64_t local_start, int64_t local_end) {
      internal::_set_thread_num(task_id);
      internal::_set_in_parallel_region(true);
      try {
        results_data[task_id] = f(local_start, local_end, ident);
      } catch (...) {
        if (!err_flag.test_and_set()) {
          eptr = std::current_exception();
        }
      }
      internal::_set_in_parallel_region(false);
      internal::_unset_thread_num();
    };

    std::vector<std::shared_ptr<ivalue::Future>> futures(num_tasks);
    for (size_t task_id = 1; task_id < num_tasks; ++task_id) {
      futures[task_id] = std::make_shared<ivalue::Future>();
      auto future_ptr = futures[task_id];
      int64_t local_start = begin + task_id * chunk_size;
      if (local_start < end) {
        int64_t local_end = std::min(end, (int64_t)(chunk_size + local_start));
        internal::_get_intraop_pool().run(
            // copy future_ptr, task_id, local_start, local_end
            [&, future_ptr, task_id, local_start, local_end]() {
          task(task_id, local_start, local_end);
          future_ptr->markCompleted(IValue());
        });
      } else {
        future_ptr->markCompleted(IValue());
      }
    }

    int64_t first_task_end = std::min(end, (int64_t)(chunk_size + begin));
    task(0, begin, first_task_end);

    for (size_t task_id = 1; task_id < num_tasks; ++task_id) {
      futures[task_id]->wait();
    }

    if (eptr) {
      std::rethrow_exception(eptr);
    }

    return std::accumulate(
        results_data, results_data + results.size(), ident, sf);
  } else {
    return f(begin, end, ident);
  }
}

} // namespace at
