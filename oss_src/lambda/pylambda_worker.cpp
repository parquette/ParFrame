/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#include <boost/program_options.hpp>
#include <cppipc/server/comm_server.hpp>
#include <lambda/pylambda.hpp>
#include <lambda/python_api.hpp>
#include <shmipc/shmipc.hpp>
#include <lambda/graph_pylambda.hpp>
#include <logger/logger.hpp>
#include <process/process_util.hpp>
#include <boost/python.hpp>
#include <util/try_finally.hpp>

namespace po = boost::program_options;

using namespace graphlab;

/** The main function to be called from the python ctypes library to
 *  create a pylambda worker process.
 *
 *  Different error routes produce different error codes of 101 and
 *  above.
 */
int _pylambda_worker_main(const char* _root_path, const char* _server_address) {

  /** Set up the debug configuration. 
   *  
   *  By default, all LOG_ERROR and LOG_FATAL messages are sent to
   *  stderr, and all LOG_INFO messages are sent to stdout (which is
   *  by default swallowed), and all LOG_DEBUG messages are dropped.
   *
   *  If GRAPHLAB_LAMBDA_WORKER_LOG_FILE is set and is non-empty, then
   *  all log messages are sent to the file instead of the stdout and
   *  stderr.  In this case, the only errors logged to stderr/stdout
   *  concern opening the log file.
   *
   *  If GRAPHLAB_LAMBDA_WORKER_DEBUG_MODE is set, then the default
   *  log level is set to LOG_DEBUG.  If a log file is set, then all
   *  log messages are sent there, otherwise they are sent to stderr.
   */
  const char* debug_mode_str = getenv("GRAPHLAB_LAMBDA_WORKER_DEBUG_MODE");
  const char* debug_mode_file_str = getenv("GRAPHLAB_LAMBDA_WORKER_LOG_FILE");

  std::string log_file_string((debug_mode_file_str == NULL) ? "" : debug_mode_file_str);
  bool log_to_file = (!log_file_string.empty());
  
  bool debug_mode = (debug_mode_str != NULL);

  // Logging using the LOG_DEBUG_WITH_PID macro requires this_pid to be set. 
  size_t this_pid = get_my_pid();
  global_logger().set_pid(this_pid);

  // Set up the logging to file if needed. 
  if(log_to_file) {
    // Set up the logging to the file, with any errors being fully logged.
    global_logger().set_log_to_console(true, true);
    global_logger().set_log_file(log_file_string);
    LOG_DEBUG_WITH_PID("Logging lambda worker logs to " << log_file_string);
    global_logger().set_log_to_console(false);
  }

  // Now, set the log mode for debug   
  if(debug_mode) {
    global_logger().set_log_level(LOG_DEBUG);
    if(!log_to_file) {
      // Set logging to console, with everything logged to stderr.
      global_logger().set_log_to_console(true, true);
    }
  } else { 
    global_logger().set_log_level(LOG_INFO);
    
    if(!log_to_file) {
      // Set logging to console, with only errors going to stderr.
      global_logger().set_log_to_console(true, false);
    }
  }

  // Log the basic information about parameters.
  std::string server_address = _server_address;
  std::string root_path = _root_path;
  size_t parent_pid = get_parent_pid();

  LOG_DEBUG_WITH_PID("root_path = '" << root_path << "'");
  LOG_DEBUG_WITH_PID("server_address = '" << server_address << "'");
  LOG_DEBUG_WITH_PID("parend pid = " << parent_pid);

  try {

    LOG_DEBUG_WITH_PID("Library function entered successfully.");

    // Whenever this is set, it must be restored upon return to python. 
    PyThreadState *python_gil_thread_state = nullptr;
    scoped_finally gil_restorer([&](){
        if(python_gil_thread_state != nullptr) {
          LOG_DEBUG_WITH_PID("Restoring GIL thread state.");
          PyEval_RestoreThread(python_gil_thread_state);
          LOG_DEBUG_WITH_PID("GIL thread state restored.");
          python_gil_thread_state = nullptr;
        }
      });
    
    try {

      LOG_DEBUG_WITH_PID("Attempting to initialize python.");
      graphlab::lambda::init_python(root_path);
      LOG_DEBUG_WITH_PID("Python initialized successfully.");
    
    } catch (const std::string& error) {
      logstream(LOG_ERROR) << this_pid << ": "
                           << "Failed to initialize python (internal exception): " << error << std::endl;
      return 101;
    } catch (const std::exception& e) {
      logstream(LOG_ERROR) << this_pid << ": "
                           << "Failed to initialize python: " << e.what() << std::endl;
      return 102;
    }

    if(server_address == "debug") {
      logstream(LOG_INFO) << "Exiting dry run." << std::endl;
      return 1; 
    }

    // Now, release the gil and continue. 
    python_gil_thread_state = PyEval_SaveThread();

    LOG_DEBUG_WITH_PID("Python GIL released.");
    
    graphlab::shmipc::server shm_comm_server;
    bool has_shm = shm_comm_server.bind();

    LOG_DEBUG_WITH_PID("shm_comm_server bind: has_shm=" << has_shm);

    // construct the server
    cppipc::comm_server server(std::vector<std::string>(), "", server_address);

    server.register_type<graphlab::lambda::lambda_evaluator_interface>([&](){
        if (has_shm) {
          auto n = new graphlab::lambda::pylambda_evaluator(&shm_comm_server);
          LOG_DEBUG_WITH_PID("creation of pylambda_evaluator with SHM complete.");
          return n;
        } else {
          auto n = new graphlab::lambda::pylambda_evaluator();
          LOG_DEBUG_WITH_PID("creation of pylambda_evaluator without SHM complete.");
          return n;
        }
      });
    server.register_type<graphlab::lambda::graph_lambda_evaluator_interface>([&](){
        auto n = new graphlab::lambda::graph_pylambda_evaluator();
        LOG_DEBUG_WITH_PID("creation of graph_pylambda_evaluator complete.");
        return n;
      });

    LOG_DEBUG_WITH_PID("Starting server.");
    server.start();

    wait_for_parent_exit(parent_pid);

    return 0;

    /** Any exceptions happening?
     */
  } catch (const std::string& error) {
    logstream(LOG_ERROR) << "Internal PyLambda Error: " << error << std::endl;
    return 103;
  } catch (const std::exception& error) {
    logstream(LOG_ERROR) << "PyLambda C++ Error: " << error.what() << std::endl;
    return 104;
  } catch (...) {
    logstream(LOG_ERROR) << "Unknown PyLambda Error." << std::endl;
    return 105;
  }
}

// This one has to be accessible from python's ctypes.  
extern "C" {
  int EXPORT pylambda_worker_main(const char* _root_path, const char* _server_address) {
    return _pylambda_worker_main(_root_path, _server_address);
  }
}

