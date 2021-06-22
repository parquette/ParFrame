/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#include <sframe/sframe_rows.hpp>
#include <sframe_query_engine/execution/query_context.hpp>
#include <sframe_query_engine/execution/execution_node.hpp>

namespace graphlab {
namespace query_eval {

query_context::query_context() {
  m_buffers = std::make_shared<sframe_rows>();
}
query_context::query_context(std::function<std::shared_ptr<sframe_rows>(size_t, bool)> callback_on_get_input,
                             std::function<emit_state(const std::shared_ptr<sframe_rows>&)> callback_on_emit,
                            size_t max_buffer_size,
                            emit_state initial_state) 
    : m_max_buffer_size(max_buffer_size),
    m_callback_on_get_input(callback_on_get_input),
    m_callback_on_emit(callback_on_emit),
    m_initial_state(initial_state){ 
  m_buffers = std::make_shared<sframe_rows>();
}

std::shared_ptr<sframe_rows> query_context::get_output_buffer() {
  return m_buffers;
}

emit_state query_context::initial_state() const {
  return m_initial_state;
}

emit_state query_context::emit(const std::shared_ptr<sframe_rows>& rows) {
  return m_callback_on_emit(m_buffers);
}
std::shared_ptr<const sframe_rows> query_context::get_next(size_t input_number) {
  return std::const_pointer_cast<const sframe_rows>(m_callback_on_get_input(input_number, false));
}

void query_context::skip_next(size_t input_number) {
  m_callback_on_get_input(input_number, true);
}


query_context::~query_context() { }

} // query_eval
} // graphlab
