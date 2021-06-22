/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#ifndef GRAPHLAB_SFRAME_QUERY_ENGINE_QUERY_OPTIMIZATION_TRANSFORMS_H_
#define GRAPHLAB_SFRAME_QUERY_ENGINE_QUERY_OPTIMIZATION_TRANSFORMS_H_

#include <sframe_query_engine/planning/optimization_node_info.hpp>
#include <string>

namespace graphlab {
namespace query_eval {

class optimization_transform_registry;
class materialize_options;

/**
 *  Optimization transforms are successively applied until no more
 *  optimizations are possible.  A queue of active nodes is maintained,
 *  starting with all nodes in the planner graph.
 *
 *  The transforms are indexed by type; for each node in the planner
 *  graph, all transformations that apply to that planner node type
 *  are attempted in order of declaration.  If no transformations
 *  apply to a node, then it is discarded from the active queue.
 *
 *  If apply_transform returns true, then the optimization for that
 *  node is stopped.  If apply_transform returns false, then it is
 *  assumed that the transformation did not apply, and the next
 *  transformation is attempted.
 *
 *  Changes to the graph coming as a result of transformations should
 *  call the appropriate method in the optimization_engine.  All
 *  intrinsic graph operations required to maintain the graph
 *  consistently are done internally by these nodes, including
 *  requeueing all affected nodes on the active queue.
 *
 *  All new transformations need to be registered in
 *  populate_transforms() inside optimization_transforms.cpp.
 */
class opt_transform {
 public:

  /** A description string that gets logged when the transformation is
   *  applied.
   */
  virtual std::string description() = 0; 
  
  /** Does the transform apply to a particular node type?
   */
  virtual bool transform_applies(planner_node_type t) = 0;

  /** Return true if the transform was applied.
   */
  virtual bool apply_transform(optimization_engine *opt_manager, cnode_info_ptr n) = 0;
};

////////////////////////////////////////////////////////////////////////////////

/** Deterime which stages are run, given exec_params.
 */
std::vector<size_t> get_stages_to_run(const materialize_options& exec_params);

/** Populate the transform registry with all transforms.
 */
void populate_transforms(optimization_transform_registry *otr);

}}

#endif /* _OPTIMIZATION_TRANSFORMS_H_ */
