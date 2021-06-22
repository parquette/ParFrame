/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#ifndef GRAPHLAB_LAMBDA_LAMBDA_MASTER_HPP
#define GRAPHLAB_LAMBDA_LAMBDA_MASTER_HPP

#include <map>
#include <globals/globals.hpp>
#include <lambda/lambda_interface.hpp>
#include <lambda/worker_pool.hpp>

namespace graphlab {

namespace shmipc {
  class client;
}

namespace lambda {

  /**
   * \ingroup lambda
   *
   * The lambda_master provides functions to evaluate a lambda
   * function on different input types (single value, list, dictionary)
   * in parallel.
   *
   * Internally, it manages a worker pool of lambda_workers.
   *
   * Each evaluation call is allocated to a worker, and block until the evaluation
   * returns or throws an exception.
   *
   * The evaluation functions can be called in parallel. When this happens,
   * the master evenly allocates the jobs to workers who has the shortest job queue.
   *
   * \code
   *
   * std::vector<flexible_type> args{0,1,2,3,4};
   *
   * // creates a master with 10 workers;
   * lambda_master master(10);
   *
   * // Evaluate a single argument.
   * // plus_one_lambda is equivalent to lambda x: x + 1
   * auto lambda_hash = master.make_lambda(plus_one_lambda);
   *
   * std::vector<flexible_type> out;
   * master.bulk_eval(lambda_hash, {0}, out);
   * ASSERT_EQ(out[0], 1);
   * master.bulk_eval(lambda_hash, {1}, out);
   * ASSERT_EQ(out[0], 2);
   *
   *
   * // Evaluate in parallel, still using plus_one_lambda.
   * std::vector< std::vector<flexible_type> > out_vec;
   * parallel_for(0, args.size(), [&](size_t i) {
   *    master.bulk_eval(lambda_hash, {args[i]}, out_vec[i]);
   * });
   *
   * for (auto val : args) {
   *   ASSERT_EQ(out_vec[i][0], (val + 1));
   * }
   * master.release_lambda(plus_one_lambda);
   *
   * \endcode
   */
  class lambda_master {
   public:

    static lambda_master& get_instance();

    /**
     * Register the lambda_str for all workers, and returns the id for the lambda.
     * Throws the exception  
     */
    size_t make_lambda(const std::string& lambda_str);

    /**
     * Unregister the lambda_str.
     */
    void release_lambda(size_t lambda_hash) noexcept;

    /**
     * Evaluate lambda on batch of inputs.
     */
    void bulk_eval(size_t lambda_hash, const std::vector<flexible_type>& args,
                   std::vector<flexible_type>& out,
                   bool skip_undefined, int seed);

    /**
     * \overload
     */
    void bulk_eval(size_t lambda_hash,
                   const sframe_rows& args,
                   std::vector<flexible_type>& out,
                   bool skip_undefined, int seed);

    /**
     * \overload
     * Lambda takes dictionary argument.
     */
    void bulk_eval(size_t lambda_hash,
                   const std::vector<std::string>& keys,
                   const std::vector<std::vector<flexible_type>>& args, 
                   std::vector<flexible_type>& out,
                   bool skip_undefined, int seed);

    /**
     * \overload
     */
    void bulk_eval(size_t lambda_hash,
        const std::vector<std::string>& keys,
        const sframe_rows& args,
        std::vector<flexible_type>& out,
        bool skip_undefined, int seed);

    inline size_t num_workers() { return m_worker_pool->num_workers(); }

    static void set_lambda_worker_binary(const std::vector<std::string>& path) { 
      lambda_worker_binary_and_args = path;
      std::ostringstream ss;

      for(size_t i = 0; i < path.size(); ++i) {
        if(i != 0) ss << ' '; 
        ss << path[i];
      }
      
      logstream(LOG_INFO) << "Pylambda worker binary: " << ss.str() << std::endl;
    };

    static void set_lambda_worker_binary(const std::string& path) { 
      lambda_worker_binary_and_args = {path}; 
      logstream(LOG_INFO) << "Pylambda worker binary: " << path << std::endl;
    };

    static const std::vector<std::string>& get_lambda_worker_binary() {
      return lambda_worker_binary_and_args;
    };
    
   private:

    lambda_master(size_t nworkers);

    lambda_master(lambda_master const&) = delete;

    lambda_master& operator=(lambda_master const&) = delete;

   private:
    std::shared_ptr<worker_pool<lambda_evaluator_proxy>> m_worker_pool;
    std::map<void*, std::shared_ptr<shmipc::client>> m_shared_memory_worker_connections;

    std::unordered_map<size_t, size_t> m_lambda_object_counter;
    graphlab::mutex m_mtx;

    /** The binary for executing the lambda_workers.
     */
    static std::vector<std::string> lambda_worker_binary_and_args;    

  };

} // end lambda
} // end graphlab

#endif
