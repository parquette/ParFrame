/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#ifndef GRAPHLAB_SFRAME_QUERY_ENGINE_INFER_OPERATOR_FIELD_H_
#define GRAPHLAB_SFRAME_QUERY_ENGINE_INFER_OPERATOR_FIELD_H_

#include <logger/assertions.hpp>
#include <memory>
#include <vector>
#include <string>
#include <flexible_type/flexible_type.hpp> 

namespace graphlab { namespace query_eval {

struct planner_node;
class query_operator;
struct query_operator_attributes;

/**
 * An enumeration of all operator types.
 */
enum class planner_node_type : int {
    CONSTANT_NODE, 
    APPEND_NODE,
    BINARY_TRANSFORM_NODE,
    LOGICAL_FILTER_NODE,
    PROJECT_NODE,
    RANGE_NODE,
    SARRAY_SOURCE_NODE,
    SFRAME_SOURCE_NODE,
    TRANSFORM_NODE,
    LAMBDA_TRANSFORM_NODE,
    GENERALIZED_TRANSFORM_NODE,
    UNION_NODE,
    GENERALIZED_UNION_PROJECT_NODE,
    REDUCE_NODE,

      // These are used as logical-node-only types.  Do not actually become an operator.
      IDENTITY_NODE,

      // used to denote an invalid node type.  Must always be last.
      INVALID
};


/**
 *  Infers the type schema of a planner node by backtracking its
 *  dependencies.
 */
std::vector<flex_type_enum> infer_planner_node_type(std::shared_ptr<planner_node> pnode);

/**
 *  Infers the length of the output of a planner node by backtracking its 
 *  dependencies.
 *
 *  Returns -1 if the length cannot be computed without an actual execution.
 */
int64_t infer_planner_node_length(std::shared_ptr<planner_node> pnode);

/**
 *  Infers the number of columns present in the output.
 */
size_t infer_planner_node_num_output_columns(std::shared_ptr<planner_node> pnode);

/** Returns the number of nodes in this planning graph, including pnode. 
 */
size_t infer_planner_node_num_dependency_nodes(std::shared_ptr<planner_node> pnode);

/**
 *  Transforms a planner node into the operator. 
 */
std::shared_ptr<query_operator> planner_node_to_operator(std::shared_ptr<planner_node> pnode);

/**  Get the name of the node from the type.
 */
std::string planner_node_type_to_name(planner_node_type type);

/**  Get the type of the node from the name. 
 */
planner_node_type planner_node_name_to_type(const std::string& name);

/**  Get the attribute struct from the type. 
 */
query_operator_attributes planner_node_type_to_attributes(planner_node_type type);

////////////////////////////////////////////////////////////////////////////////

/**  This operator consumes all inputs at the same rate, and there
 *  is exactly one row for every input row.
 */
bool consumes_inputs_at_same_rates(const query_operator_attributes& attr);
bool consumes_inputs_at_same_rates(const std::shared_ptr<planner_node>& n);

////////////////////////////////////////////////////////////////////////////////

/**  A collection of flags used in actually doing the query
 *   optimization.
 */
bool is_linear_transform(const query_operator_attributes& attr);
bool is_linear_transform(const std::shared_ptr<planner_node>& n);

////////////////////////////////////////////////////////////////////////////////

/**  This operator consumes all inputs at the same rate, but reduces
 *   the rows in the output.
 */
bool is_sublinear_transform(const query_operator_attributes& attr);
bool is_sublinear_transform(const std::shared_ptr<planner_node>& n);

////////////////////////////////////////////////////////////////////////////////

/**
 * This operator is a source node.
 */
bool is_source_node(const query_operator_attributes& attr);
bool is_source_node(const std::shared_ptr<planner_node>& n);

/** Returns true if the output of this node can be parallel sliceable
 *  by the sources on this block, and false otherwise. 
 */
bool is_parallel_slicable(const std::shared_ptr<planner_node>& n);

/** Returns a set of integers giving the different parallel slicable
 *  units for the inputs of a particular node. If 
 */
std::vector<size_t> get_parallel_slicable_codes(const std::shared_ptr<planner_node>& n);

typedef std::function<std::string(std::shared_ptr<planner_node>)> pnode_tagger;

/** Representation of the node as a string.
 */
std::string planner_node_repr(const std::shared_ptr<planner_node>& node);


std::ostream& operator<<(std::ostream&,
                      const std::shared_ptr<planner_node>& node);
}}

#endif /* _INFER_OPERATOR_FIELD_H_ */
