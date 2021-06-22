/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#include <cstddef>
#include <unistd.h>
#include <logger/logger.hpp>
#include <timer/timer.hpp>
#include <logger/log_rotate.hpp>
#include <parallel/pthread_tools.hpp>
#include <boost/filesystem.hpp>

namespace fs = boost::filesystem;

#ifdef _WIN32
#define unlink _unlink
#endif

namespace graphlab {

static std::string log_base_name;
static std::string symlink_name;
static size_t log_counter = 0;
static size_t log_interval = 24 * 60 * 60;
static size_t truncate_limit = 2;
static thread log_rotate_thread;
static mutex lock;
static conditional cond;
static bool thread_running = false;

std::string make_file_name(std::string base_name, size_t ctr) {
  return base_name + "." + std::to_string(ctr);
}

void log_rotation_background_thread() {
  while(thread_running) {
    // set up the current logger
    std::string current_log_file = make_file_name(log_base_name, log_counter);
    global_logger().set_log_file(current_log_file);
#ifndef _WIN32
    unlink(symlink_name.c_str());
    fs::create_symlink(current_log_file.c_str(), symlink_name.c_str());
#endif
    
    // if our counter exceeds the truncate limit, delete earlier files
    if (truncate_limit > 0 && log_counter >= truncate_limit) {
      // delete oldest files
      std::string oldest_log_file = make_file_name(log_base_name, 
                                                   log_counter - truncate_limit); 
      unlink(oldest_log_file.c_str());
    }

    // sleep for the log interval period.
    // We maintain our own additional timer to prevent spurious wakeups
    timer ti; ti.start();
    lock.lock();
    while (thread_running && ti.current_time() < log_interval) {
      cond.timedwait(lock, log_interval);
    }
    lock.unlock();

    ++log_counter;
  }
}

void begin_log_rotation(std::string _log_file_name, 
                        size_t _log_interval,
                        size_t _truncate_limit) {
  if (_truncate_limit == 0) throw "Truncate limit must be >= 1";
  stop_log_rotation();
  // set up global variables
  log_base_name = _log_file_name;
  log_interval = _log_interval;
  truncate_limit = _truncate_limit;
  log_counter = 0;
  symlink_name = log_base_name;

  thread_running = true;
  std::cout << "Launching log rotate thread" << std::endl;
  log_rotate_thread.launch(log_rotation_background_thread);
}

void stop_log_rotation() {
  // if no log rotation active, quit.
  if (!thread_running) return;
  // join the log rotation thread.
  lock.lock();
  thread_running = false;
  cond.signal();
  lock.unlock();
  log_rotate_thread.join();
  // we will continue logging to the same location, but we will 
  // delete the symlink
#ifndef _WIN32
  unlink(symlink_name.c_str());
#endif
}

} // namespace graphlab
