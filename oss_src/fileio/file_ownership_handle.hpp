/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#ifndef GRAPHLAB_FILEIO_FILE_OWNERSHIP_HANDLE_HPP
#define GRAPHLAB_FILEIO_FILE_OWNERSHIP_HANDLE_HPP
#include <vector>
#include <string>
#include <logger/logger.hpp>
#include <fileio/fs_utils.hpp>
namespace graphlab {
namespace fileio {
/**
 * A simple RAII class which manages the lifespan of one file.
 * On destruction, this file is deleted if marked for deletion.
 */
struct file_ownership_handle {
  file_ownership_handle() = default;
  // deleted copy constructor
  file_ownership_handle(const file_ownership_handle& ) = delete;
  // deleted assignment operator
  file_ownership_handle& operator=(const file_ownership_handle&) = delete;

  /// move constructor
  file_ownership_handle(file_ownership_handle&& other) {
    m_file = other.m_file;
    other.m_file = std::string();
    m_delete_on_destruction = other.m_delete_on_destruction;
    m_recursive_deletion = other.m_recursive_deletion;
  }

  /// move assignment
  file_ownership_handle& operator=(file_ownership_handle&& other) {
    m_file = other.m_file;
    other.m_file = std::string();
    m_delete_on_destruction = other.m_delete_on_destruction;
    m_recursive_deletion = other.m_recursive_deletion;
    return (*this);
  }

  /// construct from one file
  inline file_ownership_handle(const std::string& file, 
                               bool delete_on_destruction = true,
                               bool recursive_deletion = false) {
    m_file = file;
    this->m_delete_on_destruction = delete_on_destruction;
    this->m_recursive_deletion = recursive_deletion;
  }
  
  void delete_on_destruction() {
    m_delete_on_destruction =  true;
  }

  void do_not_delete_on_destruction() {
    m_delete_on_destruction =  false;
  }

  /// Destructor deletes the owned file if delete_on_destruction is true
  inline ~file_ownership_handle() {
    if (m_delete_on_destruction) {
      try {
        if (!m_file.empty()) {
          if (m_recursive_deletion) {
            logstream(LOG_DEBUG) << "deleting directory " << m_file << std::endl;
            fileio::delete_path_recursive(m_file);
          } else {
            logstream(LOG_DEBUG) << "deleting file " << m_file << std::endl;
            fileio::delete_path_impl(m_file);
          }
        }
      } catch (...) {
        logstream(LOG_ERROR) << "Exception on attempted deletion of " << m_file << std::endl;
      }
    }
  }

  std::string m_file;
  bool m_delete_on_destruction = false;
  bool m_recursive_deletion = false;
}; // file_ownership_handle
} // namespace fileio
} // namespace graphlab
#endif
