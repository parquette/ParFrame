/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#ifndef GRAPHLAB_LAMBDA_PYLAMBDA_EVALUATOR_HPP
#define GRAPHLAB_LAMBDA_PYLAMBDA_EVALUATOR_HPP
#include <lambda/lambda_interface.hpp>
#include <flexible_type/flexible_type.hpp>
#include <parallel/pthread_tools.hpp>
#include <string>

// Forward delcaration
namespace boost {
  namespace python {
    namespace api {
      class object;
    }
  }
}

namespace graphlab {
namespace shmipc {
  class server;
}
class sframe_rows;

namespace lambda {
/**
 * \ingroup lambda
 *
 * A functor class wrapping a pickled python lambda string.
 *
 * The lambda type is assumed to be either: S -> T or or List -> T.
 * where all types should be compatible  with flexible_type.
 *
 * \note: currently only support basic flexible_types: flex_string, flex_int, flex_float
 *
 * \internal
 * All public member functions including the constructors are guarded by the
 * global mutex, preventing simultanious access to the python's GIL.
 *
 * Internally, the class stores a a python lambda object which is created from the
 * pickled lambda string upon construction. The lambda object is equivalent
 * to a python lambda object (with proper reference counting), and therefore, the class is copiable.
 */
class pylambda_evaluator : public lambda_evaluator_interface {

 public:
  /**
   * Construct an empty evaluator.
   */
  inline pylambda_evaluator(graphlab::shmipc::server* shared_memory_server = nullptr) { 
    m_shared_memory_server = shared_memory_server;
    m_current_lambda_hash = (size_t)(-1); 
  };

  ~pylambda_evaluator();

  /**
   * Sets the internal lambda from a pickled lambda string.
   *
   * Throws an exception if the construction failed.
   */
  size_t make_lambda(const std::string& pylambda_str);

  /**
   * Release the cached lambda object
   */
  void release_lambda(size_t lambda_hash);

  /**
   * Evaluate the lambda function on each argument separately in the args list.
   */
  std::vector<flexible_type> bulk_eval(size_t lambda_hash, const std::vector<flexible_type>& args, bool skip_undefined, int seed);

  /**
   * \overload
   *
   * We have to use different function name because
   * the cppipc interface doesn't support true overload
   */
  std::vector<flexible_type> bulk_eval_rows(size_t lambda_hash,
      const sframe_rows& values, bool skip_undefined, int seed); 

  /**
   * Evaluate the lambda function on each element separately in the values.
   * The value element is combined with the keys to form a dictionary argument. 
   */
  std::vector<flexible_type> bulk_eval_dict(size_t lambda_hash,
      const std::vector<std::string>& keys,
      const std::vector<std::vector<flexible_type>>& values,
      bool skip_undefined, int seed);

  /**
   * \overload
   *
   * We have to use different function name because
   * the cppipc interface doesn't support true overload
   */
  std::vector<flexible_type> bulk_eval_dict_rows(size_t lambda_hash,
      const std::vector<std::string>& keys,
      const sframe_rows& values,
      bool skip_undefined, int seed);

  /**
   * Initializes shared memory communication via SHMIPC.
   * Returns the shared memory address to connect to.
   */
  std::string initialize_shared_memory_comm();

 private:

  // Set the lambda object for the next evaluation.
  void set_lambda(size_t lambda_hash);

  /**
   * Apply as a function: flexible_type -> flexible_type,
   *
   * \note: this function does not perform type check and exception could be thrown
   * when applying of the function. As a subroutine, this function does not
   * try to acquire GIL and assumes it's already been acquired.
   */
  flexible_type eval(size_t lambda_hash, const flexible_type& arg);

  /**
   * Redirects to either bulk_eval_rows or bulk_eval_dict_rows.
   * First byte in the string is a bulk_eval_serialized_tag byte to denote
   * whether this call is going to bulk_eval_rows or bulk_eval_dict_rows.
   *
   * Deserializes the remaining parameters from the string 
   * and calls the function accordingly.
   */
  std::vector<flexible_type> bulk_eval_rows_serialized(const char* ptr, size_t len);

  /**
   * The unpickled python lambda object.
   */
  boost::python::api::object* m_current_lambda = NULL;
  std::map<size_t, boost::python::api::object*> m_lambda_hash;
  size_t m_current_lambda_hash;
  graphlab::shmipc::server* m_shared_memory_server;
  graphlab::thread m_shared_memory_listener;
  volatile bool m_shared_memory_thread_terminating = false;
};
  } // end of lambda namespace
} // end of graphlab namespace
#endif
