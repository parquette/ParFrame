/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#ifndef GRAPHLAB_SFRAME_QUERY_ENGINE_PLANNER_HPP_
#define GRAPHLAB_SFRAME_QUERY_ENGINE_PLANNER_HPP_

#include <vector>
#include <string>
#include <memory>

#include <sframe/sframe.hpp>
#include <sframe_query_engine/planning/materialize_options.hpp>
#include <sframe_query_engine/planning/planner_node.hpp>

namespace graphlab { 
namespace query_eval { 

class query_planner;
/**  The main query plan call.
 *
 */
class planner {
 public:
  typedef std::function<bool(size_t, const std::shared_ptr<sframe_rows>&)> 
    write_callback_type;

  planner() {}

  /**  
   * Materialize the output from a node on a graph as an SFrame.
   *
   * Note that exec_params allows some control over the execution of the
   * materialization.
   *
   *
   * This function is the tip of the materialization pipeline,
   * everything materialization operation should come through here, and the
   * objective here is to correctly handle all query plans.
   *
   * Internally, the materialization hierarchy is:
   *  - \ref planner::materialize Handles the most general materializations
   *  - \ref planner::partial_materialize Handles the most general materializations 
   *                                      but performs all materializations except 
   *                                      for the last stage. A private function.
   *  - \ref planner::execute_node Replicates a plan for parallelization. 
   *                               A private function.
   *  - \ref subplan_executor Executes a restricted plan.
   */
  sframe materialize(std::shared_ptr<planner_node> tip, 
                     materialize_options exec_params = materialize_options());

  /**
   * \overload
   * Convenience overload for a very common use case which is to just
   * materialize to a callback function.
   *
   * See the \ref materialize_options for details on what the arguments achieve.
   *
   * But most notably, if partial_materialize is false, the materialization
   * may fail. See \ref materialize_options for details.
   */
  void materialize(std::shared_ptr<planner_node> tip,
                   write_callback_type callback,
                   size_t num_segments,
                   bool partial_materialize = true);

  
  /** If this returns true, it is recommended to go ahead and
   *  materialize the sframe operations on the fly to prevent memory
   *  issues.
   */
  bool online_materialization_recommended(std::shared_ptr<planner_node> tip);

  /**  
   * Materialize the output, returning the result as a planner node.
   */
  std::shared_ptr<planner_node>  materialize_as_planner_node(
      std::shared_ptr<planner_node> tip, 
      materialize_options exec_params = materialize_options());
  
  
};




} // namespace query_eval
} // namespace graphlab

#endif /* GRAPHLAB_SFRAME_QUERY_ENGINE_PLANNER_HPP_ */
