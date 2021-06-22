/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#include <boost/bind.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <cppipc/client/comm_client.hpp>
#include <cppipc/common/object_factory_proxy.hpp>
#include <minipsutil/minipsutil.h>
#include <export.hpp>
#ifdef FAKE_ZOOKEEPER
#include <fault/fake_key_value.hpp>
#else
#include <zookeeper_util/key_value.hpp>
#endif

namespace cppipc {

EXPORT std::atomic<size_t>& get_running_command() {
  // A bit of a cleaner way to create a global variable
  static std::atomic<size_t> running_command;
  return running_command;
}

EXPORT std::atomic<size_t>& get_cancelled_command() {
  static std::atomic<size_t> cancelled_command;
  return cancelled_command;
}

comm_client::comm_client(std::vector<std::string> zkhosts, 
                         std::string name,
                         size_t num_tolerable_ping_failures,
                         std::string alternate_control_address,
                         std::string alternate_publish_address,
                         const std::string public_key,
                         const std::string secret_key,
                         const std::string server_public_key, 
                         bool ops_interruptible):
    zmq_ctx(zmq_ctx_new()), 
    keyval(zkhosts.empty() ? 
              NULL :   // make a keyval only if zkhosts is not empty
              new graphlab::zookeeper_util::key_value(zkhosts, "cppipc", name)), 
    object_socket(zmq_ctx, keyval, 
                  zkhosts.empty() ?  // use the name as the address if zookeeper 
                    name :       // is not used.
                    "call", 
                  std::vector<std::string>(),
                  public_key, secret_key, server_public_key
                  ),
    subscribesock(zmq_ctx, keyval, 
               boost::bind(&comm_client::subscribe_callback, this, _1)),
    num_tolerable_ping_failures(num_tolerable_ping_failures),
    alternate_control_address(alternate_control_address),
    alternate_publish_address(alternate_publish_address),
    endpoint_name(name) {
      init(ops_interruptible);
    }

comm_client::comm_client(std::string name, void* zmq_ctx) :
    zmq_ctx(zmq_ctx), 
    owns_zmq_ctx(false),
    keyval(NULL), 
    object_socket(zmq_ctx, keyval, name, std::vector<std::string>(),
                  "", "", ""),
    subscribesock(zmq_ctx, keyval, 
               boost::bind(&comm_client::subscribe_callback, this, _1)),
    endpoint_name(name) {
      ASSERT_MSG(boost::starts_with(name, "inproc://"), "This constructor only supports inproc address");
      init();
}

void comm_client::init(bool ops_interruptible) {
  get_running_command().store(0);
  get_cancelled_command().store(0);

  // connect the subscribesock either to the key "status" (if zookeeper is used),
  // or to the alternate address if zookeeper is not used.
  object_socket.add_to_pollset(&pollset);
  subscribesock.add_to_pollset(&pollset);
  pollset.start_poll_thread();

  if(ops_interruptible) {
    cancel_handling_enabled = true;
  }
}

void comm_client::set_server_alive_watch_pid(int32_t pid) {
  server_alive_watch_pid = pid;
}

void comm_client::poll_server_pid_is_running() {
  if (server_alive_watch_pid != 0 && pid_is_running(server_alive_watch_pid) == false) {
    server_alive = false;
  }
}

reply_status comm_client::start() {

  // create the root object proxy
  object_factory = new object_factory_proxy(*this);

  // now we flag that we are started (so that the ping thread can send pings)
  // and begin the ping thread.

  started = true;

  ping_thread = new boost::thread([this] {
    boost::unique_lock<boost::mutex> lock(this->ping_mutex);
    while(!this->ping_thread_done) {
      boost::system_time timeout =
          boost::get_system_time() + boost::posix_time::milliseconds(1000);
      this->ping_cond.timed_wait(lock, timeout);
      lock.unlock();
      if (this->ping_thread_done) return;

      std::string ping_body = std::string("");
      if(console_cancel_handler::get_instance().get_cancel_flag()) {
        console_cancel_handler::get_instance().set_cancel_flag(false);

        // Send "ctrlc<distinct_command_id>" in the ping body
        ping_body += "ctrlc";
        ping_body += std::to_string(get_cancelled_command().load());
      }

      // manually construct a call message to wait on the future
      call_message msg;
      prepare_call_message_structure(0, &object_factory_base::ping, msg);
      graphlab::oarchive oarc;
      cppipc::issue(oarc, &object_factory_base::ping, ping_body);
      msg.body = oarc.buf;
      msg.bodylen = oarc.off;

      auto future = this->internal_call_future(msg, true);
      // now, we wait on the future for 5 seconds
      // do it in 1 second increments
      // this speeds up client termination somewhat since it doesn't have
      // to wait for the full 5 seconds to cancel.
      for (size_t i = 0;i < 5; ++i) {
        auto future_timeout =
            boost::chrono::system_clock::now() + boost::chrono::milliseconds(1000);
        future.wait_until(future_timeout);
        if (future.has_value()) break;
        if (this->ping_thread_done) return;
      }
      lock.lock();
      if (future.has_value()) {
        // we ignore the message as long as we get a reply
        future.get()->msgvec.clear();
        delete future.get();
        // everything is good!
        server_alive = true;
        ping_failure_count = 0;
      } else {
        ++ping_failure_count;
        if (ping_failure_count >= this->num_tolerable_ping_failures) {
          server_alive = false;
        }
      }
    }
  });
  start_status_callback_thread();
  std::string cntladdress;
  // Bring the control_socket up
  if (!keyval) {
    if (alternate_control_address.length() > 0) {
      cntladdress = alternate_control_address;
    } else {
      try {
        cntladdress = object_factory->get_control_address();
      } catch (ipcexception& except) {
        // FAIL!! We cannot start
        return except.get_reply_status();
      }
    }
  }

  cntladdress = convert_generic_address_to_specific(cntladdress);

  control_socket = new libfault::async_request_socket(zmq_ctx, keyval,
      (keyval == NULL) ? cntladdress : "control",
      std::vector<std::string>());

  control_socket->add_to_pollset(&pollset);

  // connect the subscriber to the status address
  if (keyval) {
    subscribesock.connect("status");
  } else if (alternate_publish_address.length() > 0) {
    subscribesock.connect(alternate_publish_address);
  } else {
      std::string pubaddress;
      try {
        pubaddress = object_factory->get_status_publish_address();
      } catch (ipcexception& except) {
        // cannot get the publish address!
        // FAIL!!! We are no longer started!
        started = false;
        stop_ping_thread();
        return except.get_reply_status();
      }
      pubaddress = convert_generic_address_to_specific(pubaddress);
      subscribesock.connect(pubaddress);
  }

  return reply_status::OK;
}

std::string comm_client::convert_generic_address_to_specific(std::string aux_addr) {
  std::string ret_str;
  // Has the server given us a "accept any TCP addresses" address?
  // Then we must convert to the address we are connected to
  // the server on
  logstream(LOG_INFO) << "Possibly converting " << aux_addr << std::endl;
  if(boost::starts_with(aux_addr, "tcp://0.0.0.0") ||
      boost::starts_with(aux_addr, "tcp://*")) {
    // Find port number in this address
    size_t port_delimiter = aux_addr.find_last_of(':');
    std::string port_num = aux_addr.substr(port_delimiter+1,
        aux_addr.length()-(port_delimiter+1));
    ret_str += endpoint_name;

    // If there is a port number on this, take it off
    // NOTE: This won't work on IPv6 addresses
    port_delimiter = ret_str.find_last_of(':');
    if(std::isdigit(ret_str[port_delimiter+1])) {
      ret_str = ret_str.substr(0, port_delimiter);
    }

    ret_str += ':';
    ret_str += port_num;
  } else {
    return aux_addr;
  }

  logstream(LOG_INFO) << "Converted " << aux_addr << " to " << ret_str << std::endl;
  return ret_str;
}

comm_client::~comm_client() {
  if (!socket_closed) stop();

  if(object_factory != NULL) {
    delete object_factory;
    object_factory = NULL;
  }
}

void comm_client::stop() {
  if (!started) return;

  stop_ping_thread();

  stop_status_callback_thread();

  // clear all status callbacks
  clear_status_watch();

  // stop all pollset callbacks
  pollset.stop_poll_thread();

  // close all sockets
  object_socket.close();
  if(control_socket != NULL) {
    control_socket->close();
  }
  subscribesock.close();
  delete control_socket;

  // destroy zookeeper 
  if (keyval) delete keyval;
  keyval = NULL;

  // close zeromq context
  if (owns_zmq_ctx) {
    zmq_ctx_destroy(zmq_ctx);
  }
  socket_closed = true;
  started = false;
}

void comm_client::stop_ping_thread() {
  ping_mutex.lock();
  if (!ping_thread) {
    ping_mutex.unlock();
    return;
  } else {
    // stop the ping thread
    ping_thread_done = true;
    ping_cond.notify_one();
    ping_mutex.unlock();
    ping_thread->join();
    delete ping_thread;
    ping_thread = NULL;
    server_alive = false;
  }
}

void comm_client::apply_auth(call_message& call) {
  for(auto& auth : auth_stack) {
    auth->apply_auth(call);
  }
}


bool comm_client::validate_auth(reply_message& reply) {
  for(auto& auth : boost::adaptors::reverse(auth_stack)) {
    if (auth->validate_auth(reply) == false) return false;
  }
  return true;
}

void comm_client::subscribe_callback(libfault::zmq_msg_vector& recv) {
  // check that it is the right format. It should just be one message
  if (recv.size() != 1) return;
  // decode the message, convert zmq_msg_t to string
  recv.reset_read_index();
  zmq_msg_t* zmsg = recv.read_next();
  std::string msg((char*)zmq_msg_data(zmsg), zmq_msg_size(zmsg));
  
  boost::lock_guard<boost::mutex> guard(status_buffer_mutex);
  status_buffer.push_back(msg);
  status_buffer_cond.notify_one();
}

void comm_client::status_callback_thread_function() {
  std::vector<std::string> localbuf;
  while(!status_callback_thread_done) {
    localbuf.clear();
    // loop on a condition wait for the buffer contents
    {
      boost::unique_lock<boost::mutex> buffer_lock(status_buffer_mutex);
      while(status_buffer.empty() && !status_callback_thread_done) {
        status_buffer_cond.wait(buffer_lock);
      } 
      // swap out and get my own copy of the messages
      std::swap(localbuf, status_buffer);
    }
    // take a local copy of the prefix_to_status_callback
    // so we don't need to hold the lock to prefix_to_status_callback
    // when issuing the callbacks. (that is at a risk of causing deadlocks)
    decltype(prefix_to_status_callback) local_prefix_to_status_callback;
    {
      boost::lock_guard<boost::mutex> guard(this->status_callback_lock);
      local_prefix_to_status_callback = prefix_to_status_callback;
    }
    // issue all the messages
    for(auto& msg: localbuf) {
      // fast exit if we are meant to stop.
      if (status_callback_thread_done) break;
      for(auto& cb: local_prefix_to_status_callback) {
        if (boost::starts_with(msg, cb.first)) {
          cb.second(msg);
        }
      }
    }
  }
}


void comm_client::start_status_callback_thread() {
  if (status_callback_thread == NULL) {
    // starts the callback thread for the status publishing
    status_callback_thread = new boost::thread([this] {
      this->status_callback_thread_function();
    });
  }
}

void comm_client::stop_status_callback_thread() {
  // wake up and shut down the status callback thread
  {
    boost::unique_lock<boost::mutex> buffer_lock(status_buffer_mutex);
    status_callback_thread_done = true; 
    status_buffer_cond.notify_one();
  }
  status_callback_thread->join();
  delete status_callback_thread;
  status_callback_thread = NULL;
}

void comm_client::add_status_watch(std::string prefix, 
                         std::function<void(std::string)> callback) {
  boost::lock_guard<boost::mutex> guard(this->status_callback_lock);
  for(auto& cb: prefix_to_status_callback) {
    if (cb.first == prefix) {
      cb.second = callback;
      return;
    }
  }
  prefix_to_status_callback.emplace_back(prefix, callback);
  subscribesock.subscribe(prefix);
}

void comm_client::remove_status_watch(std::string prefix) {
  boost::lock_guard<boost::mutex> guard(this->status_callback_lock);
  auto iter = prefix_to_status_callback.begin();
  while (iter != prefix_to_status_callback.end()) {
    if (iter->first == prefix) {
      prefix_to_status_callback.erase(iter);
      subscribesock.unsubscribe(prefix);
      break;
    }
    ++iter;
  }
}
void comm_client::clear_status_watch() {
  boost::lock_guard<boost::mutex> guard(this->status_callback_lock);
  prefix_to_status_callback.clear();
}

boost::shared_future<libfault::message_reply*>
comm_client::internal_call_future(call_message& call, bool control) {
  // If the socket is already dead, return with an unreachable
  if (socket_closed) {
    libfault::message_reply* reply = new libfault::message_reply;
    reply->status = EHOSTUNREACH;
    return boost::make_future(reply);
  }
  apply_auth(call);
  libfault::zmq_msg_vector callmsg;
  call.emit(callmsg);
  // Control messages use a separate socket
  if(control && control_socket != NULL) {
    return control_socket->request_master(callmsg);
  }
  return object_socket.request_master(callmsg);
}

int comm_client::internal_call(call_message& call, reply_message& reply, bool control) {
  if (!started) {
    return ENOTCONN;
  }

  auto future = internal_call_future(call, control);
  while(server_alive && !future.has_value()) {
    poll_server_pid_is_running();
    auto future_timeout =
          boost::chrono::system_clock::now() + boost::chrono::milliseconds(5000);
    future.wait_until(future_timeout);
  }

  // if server is dead, we quit
  if (server_alive == false) {
    call.clear();
    return EHOSTUNREACH;
  }

  int status = future.get()->status;
  if (status != 0) {
    delete future.get();
    return status;
  }
  // otherwise construct the reply
  reply.construct(future.get()->msgvec);
  future.get()->msgvec.clear();
  delete future.get();

  if (!validate_auth(reply)) {
    // construct an auth failure reply
    reply.clear();
    reply.status = reply_status::AUTH_FAILURE;
  }
  return status;
}


size_t comm_client::make_object(std::string object_type_name) {
  if (!started) {
    throw ipcexception(reply_status::COMM_FAILURE, 0, "Client not started");
  }
  return object_factory->make_object(object_type_name);
}


std::string comm_client::ping(std::string pingval) {
  if (!started) {
    throw ipcexception(reply_status::COMM_FAILURE, 0, "Client not started");
  }
  return object_factory->ping(pingval);
}


void comm_client::delete_object(size_t object_id) {
  if (!started) {
    throw ipcexception(reply_status::COMM_FAILURE, 0, "Client not started");
  }
  size_t ref_cnt = 0;
  try {
    object_factory->delete_object(object_id);
    ref_cnt = decr_ref_count(object_id);
  } catch(...) {
    // do nothing if we fail to delete. thats ok
  }
  if(ref_cnt == size_t(-1)) {
    throw ipcexception(reply_status::EXCEPTION, 0, "Attempted to delete untracked object!");
  }
}

// Returns new reference count of object
size_t comm_client::incr_ref_count(size_t object_id) {
  boost::lock_guard<boost::mutex> guard(ref_count_lock);
  auto ret = object_ref_count.insert(std::make_pair(object_id, 1));
  if(!ret.second) {
    ret.first->second++;
  }

  return ret.first->second;
}

// Returns new reference count of object, and size_t(-1) if object not found
size_t comm_client::decr_ref_count(size_t object_id) {
  size_t ref_cnt;
  {
    boost::lock_guard<boost::mutex> guard(ref_count_lock);
    auto ret = object_ref_count.find(object_id);

    if(ret != object_ref_count.end()) {
      if(ret->second > 1) {
        ref_cnt = --ret->second;
      } else if(ret->second == 1) {
        object_ref_count.erase(ret);
        ref_cnt = 0;
      } else {
        object_ref_count.erase(ret);
        ref_cnt = ret->second;
      }
    } else {
      ref_cnt = size_t(-1);
    }
  }
  if (ref_cnt == 0) {
    send_deletion_list({object_id});
  }
  return ref_cnt;
}

size_t comm_client::get_ref_count(size_t object_id) {
  boost::lock_guard<boost::mutex> guard(ref_count_lock);
  auto ret = object_ref_count.find(object_id);

  if(ret != object_ref_count.end()) {
    return ret->second;
  }

  return size_t(-1);
}

// Returns 0 if we sent the tracked objects, 1 if the sync point was not
// reached yet, and -1 if an error occurred while sending.
int comm_client::send_deletion_list(const std::vector<size_t>& object_ids) {
  // Send tracked objects
  call_message msg;
  prepare_call_message_structure(0, &object_factory_base::sync_objects, msg);

  graphlab::oarchive oarc;
  cppipc::issue(oarc, &object_factory_base::sync_objects, 
                object_ids, false /* inactive list */);

  msg.body = oarc.buf;
  msg.bodylen = oarc.off;

  // Receive reply. Not used for anything currently except an indicator of
  // success.
  reply_message reply;
  int r = internal_call(msg, reply);

  if(r == 0) {
    return 0;
  } else {
    return -1;
  }
}


} // cppipc
