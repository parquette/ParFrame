/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#ifndef GRAPHLAB_SFRAME_QUERY_ENGINE_OPERATORS_EXECUTION_NODE_HPP
#define GRAPHLAB_SFRAME_QUERY_ENGINE_OPERATORS_EXECUTION_NODE_HPP

#include <memory>
#include <vector>
#include <queue>
#include <boost/coroutine/coroutine.hpp>
#include <flexible_type/flexible_type.hpp>
#include <sframe_query_engine/operators/operator.hpp>

namespace graphlab { 
class sframe_rows;

namespace query_eval {
class query_context;
/**
 * The execution node provides a wrapper around an operator. It
 *  - manages the coroutine context for the operator
 *  - manages the connections between the operator and its inputs and outputs.
 *  - Manages the buffering and transfer of information between the operator,
 *
 * \subsection execution_node_coroutines Coroutines
 *
 * Essentially, calling a coroutine, causes a context switch to occur
 * starting the coroutine. Then within the coroutine, a "sink()" function can be 
 * called which context switches and resumes execution *where the coroutine was
 * initially triggered*. 
 *
 * The classical example is a producer-consumer queue
 * \code
 * void producer() {
 *   while(1) {
 *     a = new work
 *     consumer(a); // or sink(a) in the above syntax
 *   }
 * }
 *
 * void consumer() {
 *   while(1) {
 *     a = producer();
 *     // do work on a
 *   }
 * }
 *
 * \endcode
 *
 * Here, we are using coroutines to attach and communicate between query 
 * operators, so for instance, here is a simple transform on a source.
 * \code
 * void data_source() {
 *   while(data_source_has_rows) {
 *     rows = read_rows
 *     sink(rows);
 *   }
 * }
 *
 * void transform() {
 *   while(1) {
 *     data = source()
 *     if (data == nullptr) {
 *       break;
 *     } else {
 *       transformed_data = apply_transform(data)
 *       sink(transformed_data);
 *     }
 *   }
 * }
 * \endcode
 *
 * while the context switch is relatively cheap (boost coroutines promise this
 * at < 100 cycles or so), we still want to avoid performing the context 
 * switch for every row, so our unit of communication across coroutines
 * is an \ref sframe_rows object which represents a collection of rows, but
 * represented columnar-wise. Every communicated block must be of a constant
 * number of rows (i.e. SFRAME_READ_BATCH_SIZE. ex: 256), except for the last
 * block which may be smaller. Operators which perform filtering for instance,
 * must hence make sure to buffer accordingly.
 *
 * \subsection execution_node_rate_control Rate Control
 * One key issue with any of the pipeline models (whether pull-based: like here,
 * or push-based) is about rate-control. For instance, the operator graph
 * corresponding to the following expression has issues:
 * \code
 * logical_filter(source_A, selector_source) + logical_filter(source_B, selector_source)
 * \endcode
 *  - To compute the "+", the left logical filter operator is invoked
 *  - To compute the left logical filter, source_A and selector_source is read
 *    and they continue to be read until say... 256 rows are generated.
 *    This is then sent to the "+" operator which resumes execution.
 *  - The "+" operator then reads the right logical filter operator.
 *  - The right logical filter operator now needs to read source_B and 
 *  selector_source.
 *  - However, selector_source has already advanced because it was partially 
 *  consumed for the left logical filter.
 * 
 * A solution to this requires the selector_source to buffer its reads
 * while feeding the left logical_filter. However, that is not tractable since
 * there is no upper limit as to how much has to be buffered.
 *
 * (There are arbitrarily complicated alternative solutions though)
 *
 * Therefore, it is required that during execution, all connected operators
 * operate at exactly the same rate. This is a tricky thing to test though.
 * 
 * \subsection execution_node_uniform_rate_control Uniform Rate Assumption
 *
 * This uniform execution rate control assumption allows for one clear
 * additional benefit. i.e. only one output buffer is required for each operator
 * and this output buffer and be arbitrarily reused. Since, the next time
 * any operator is called, it is guaranteed that any previous data it generated
 * has already been consumed.
 *
 * \subsection execution_node_usage execution_node Usage
 *
 * The execution_node is not generally used directly (see the hierarchy of
 * materialize functions). However, usage is not very complicated.
 *
 * Given an execution_node graph, with a tip you will like to consume data from:
 * \code
 *  (tip is a shared_ptr<execution_node>)
 *  // register a new consumer (aka myself)
 *  size_t consumer_id = tip->register_consumer();
 *
 *  while(1) {
 *    auto rows = node->get_next(consumer_id);
 *    // do stuff. rows == nullptr on completion
 *  }
 * \endcode
 */
class execution_node  : public std::enable_shared_from_this<execution_node> {
 public:
  execution_node(){}

  /** 
   * Initializes the execution node with an operator and inputs.
   * Also resets the operator.
   */
  explicit execution_node(const std::shared_ptr<query_operator>& op,
                 const std::vector<std::shared_ptr<execution_node> >& inputs 
                   = std::vector<std::shared_ptr<execution_node>>());

  execution_node(execution_node&&) = default;
  execution_node& operator=( execution_node&&) = default;

  execution_node(const execution_node&) = delete;
  execution_node& operator=(const execution_node&) = delete;
  
  /** 
   * Initializes the execution node with an operator and inputs.
   * Also resets the operator.
   */
  void init(const std::shared_ptr<query_operator>& op,
            const std::vector<std::shared_ptr<execution_node> >& inputs
            = std::vector<std::shared_ptr<execution_node>>());


  /**
   * Adds an execution consumer. This function call then
   * returns an ID which the caller should use with get_next().
   */
  size_t register_consumer();


  /** Returns nullptr if there is no more data.
   */
  std::shared_ptr<sframe_rows> get_next(size_t consumer_id, bool skip=false);

  /**
   * Returns the number of inputs of the execution node
   */
  inline size_t num_inputs() const {
    return m_inputs.size();
  }

  inline std::shared_ptr<execution_node> get_input_node(size_t i) const {
    return m_inputs[i].m_node;
  }

  /**
   * resets the state of this execution node. Note that this does NOT 
   * recursively reset all parents (since in a general graph this could cause
   * multiple resets of the same vertex). The caller must ensure that all 
   * connected execution nodes are reset.
   */
  void reset();

  /**
   * Returns true if an exception occured while executing this node
   */
  bool exception_occurred() const {
    return m_exception_occured;
  }

  /**
   * If an exception occured while excecuting this node, this returns the 
   * last exception exception. Otherwise returns an exception_ptr which 
   * compares equal to the null pointer.
   */
  std::exception_ptr get_exception() const {
    return m_exception;
  }
 private:
  /**
   * Internal function used to add to the operator output
   */
  void add_operator_output(const std::shared_ptr<sframe_rows>& rows);

  /**
   * Internal utility function what pulls the next batch of rows from a input
   * to this node.
   */
  std::shared_ptr<sframe_rows> get_next_from_input(size_t input_id, bool skip);

  /**
   * Starts the coroutines
   */
  void start_coroutines();

  /// The operator implementation
  std::shared_ptr<query_operator> m_operator;

  /// The coroutine running the actual function
  typename boost::coroutines::coroutine<void>::pull_type m_source;

  /**
   * The inputs to this execution node:
   *   what execution node they come from, and what is the consumer ID
   *   when trying to pull data from the execution node.
   */
  struct input_node {
    std::shared_ptr<execution_node> m_node;
    size_t m_consumer_id = 0;
  };
  std::vector<input_node> m_inputs;

  /**
   * every block of output is assigned an ID. The ID of the current
   * head is the m_head. The queue has a maximum length of 2 since
   * all consumers must consume in lock step, the gap between the min position
   * and max position of consumers is at most 1.
   */
  std::queue<std::shared_ptr<sframe_rows> > m_output_queue; 
  size_t m_head = 0; 
  bool m_coroutines_started = false;
  bool m_skip_next_block = false;

  /// m_consumer_pos[i] is the ID which consumer i is consuming next.
  std::vector<size_t> m_consumer_pos;

  /// exception handling
  bool m_exception_occured = false;
  std::exception_ptr m_exception;

  friend class query_context;
};

}}

#endif /* GRAPHLAB_SFRAME_QUERY_ENGINE_OPERATORS_EXECUTION_NODE_HPP */
