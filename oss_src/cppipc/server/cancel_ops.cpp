/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#include "cppipc/server/cancel_ops.hpp"
#include "export.hpp"

namespace cppipc {

/** A way of making srv_running_command global
  * The values of srv_running_command are meant to have a range of an unsigned
  * 64-bit integer.  Two values have special meaning:
  *   - A value of 0 means that there is no command currently running.
  *   - A value of uint64_t(-1) means that the currently running command should
  *   be cancelled.
  *
  * NOTE: This design relies on the current fact that the cppipc server will only
  * run one command at a time.  This must be revisited if more than 1 command could
  * possibly be running.
  */
std::atomic<unsigned long long>& get_srv_running_command() {
  static std::atomic<unsigned long long> srv_running_command;
  return srv_running_command;
}

std::atomic<bool>& get_cancel_bit_checked() {
  static std::atomic<bool> cancel_bit_checked;
  return cancel_bit_checked;
}

EXPORT bool must_cancel() {
  unsigned long long max_64bit = (unsigned long long)uint64_t(-1);
  std::atomic<unsigned long long> &command = get_srv_running_command();
  get_cancel_bit_checked().store(true);

  // In theory, this is not an atomic operation. However, this command variable
  // is only written at times strictly before and after must_cancel can be
  // called.  Command gets reset to 0 once the command has exited.
  return !(command.load() != max_64bit);
}

} // cppipc
