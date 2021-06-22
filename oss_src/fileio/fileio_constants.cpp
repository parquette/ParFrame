/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#include <cstdlib>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <fileio/fileio_constants.hpp>
#include <fileio/fs_utils.hpp>
#include <fileio/temp_files.hpp>
#include <fileio/hdfs.hpp>
#include <globals/globals.hpp>
#include <random/random.hpp>
#include <iostream>
#include <export.hpp>

namespace fs = boost::filesystem;

namespace graphlab {
namespace fileio {


/**
 * Finds the system temp directory.
 *
 * Really, we should be using $TMPDIR or /tmp. But Fedora 18 figured that
 * /tmp should be on tmpfs and thus should only hold small files. Thus we
 * should use /var/tmp when available. But that means we are not following
 * best practices and using $TMPDIR. so.... aargh.
 *
 * This will emit one of the following in order of preference. It will return
 * the first directory which exists. exit(1) on failure.:
 *  - /var/tmp
 *  - $TMPDIR
 *  - /tmp
 */
std::string get_system_temp_directory() {
  boost::filesystem::path path;
#ifndef _WIN32
  char* tmpdir = getenv("TMPDIR");
#else
  char* tmpdir = getenv("TMP");
#endif
  // try $GRAPHLAB_TMPDIR first
  if (boost::filesystem::is_directory("/var/tmp")) {
    path = "/var/tmp";
  } else if(tmpdir && boost::filesystem::is_directory(tmpdir)) {
    path = tmpdir;
  } else {
    if (boost::filesystem::is_directory("/tmp")) {
      path = "/tmp";
    }
  }
  return path.string();
}

static bool check_cache_file_location(std::string val) {
  boost::algorithm::trim(val);
  std::vector<std::string> paths;
  boost::algorithm::split(paths,
      val,
#ifndef _WIN32
      boost::algorithm::is_any_of(":"));
#else
      boost::algorithm::is_any_of(";"));
#endif
  if (paths.size() == 0) 
    throw std::string("Value cannot be empty");
  for (std::string path: paths) {
    if (!boost::filesystem::is_directory(path))
      throw std::string("Directory: ") + path + " does not exist";
  }
  return true;
}

static bool check_cache_file_hdfs_location(std::string val) {
  if (get_protocol(val) == "hdfs") {
    if (get_file_status(val) == file_status::DIRECTORY) {
      // test hdfs write permission by createing a test directory 
      namespace fs = boost::filesystem;
      std::string host, port, hdfspath;
      std::tie(host, port, hdfspath) = parse_hdfs_url(val);
      auto& hdfs = graphlab::hdfs::get_hdfs(host, std::stoi(port));
      fs::path temp_dir (hdfspath);
      temp_dir /= std::string("test-") + std::to_string(random::rand());
      bool success = hdfs.create_directories(temp_dir.string());
      if (!success) {
        throw std::string("Cannot write to ") + val;
      }
      hdfs.delete_file_recursive(temp_dir.string());
      return true;
    } else {
      throw std::string("Directory: ") + val + " does not exist";
    }
  }
  throw std::string("Invalid hdfs path: ") + val;
}

EXPORT const size_t FILEIO_INITIAL_CAPACITY_PER_FILE = 1024;
EXPORT size_t FILEIO_MAXIMUM_CACHE_CAPACITY_PER_FILE = 128 * 1024 * 1024;
EXPORT size_t FILEIO_MAXIMUM_CACHE_CAPACITY = 2LL * 1024 * 1024 * 1024;
EXPORT size_t FILEIO_READER_BUFFER_SIZE = 16 * 1024;
EXPORT size_t FILEIO_WRITER_BUFFER_SIZE = 96 * 1024;

REGISTER_GLOBAL(int64_t, FILEIO_MAXIMUM_CACHE_CAPACITY, true); 
REGISTER_GLOBAL(int64_t, FILEIO_MAXIMUM_CACHE_CAPACITY_PER_FILE, true) 
REGISTER_GLOBAL(int64_t, FILEIO_READER_BUFFER_SIZE, false);
REGISTER_GLOBAL(int64_t, FILEIO_WRITER_BUFFER_SIZE, false); 


static constexpr char CACHE_PREFIX[] = "cache://";
static constexpr char TMP_CACHE_PREFIX[] = "cache://tmp/";
std::string get_cache_prefix() { return CACHE_PREFIX; }
std::string get_temp_cache_prefix() { return TMP_CACHE_PREFIX; }


EXPORT std::string CACHE_FILE_LOCATIONS = "CHANGEME";
EXPORT std::string CACHE_FILE_HDFS_LOCATION = "";

REGISTER_GLOBAL_WITH_CHECKS(std::string,
                            CACHE_FILE_LOCATIONS,
                            true,
                            check_cache_file_location);

REGISTER_GLOBAL_WITH_CHECKS(std::string,
                            CACHE_FILE_HDFS_LOCATION,
                            true,
                            check_cache_file_hdfs_location);

std::string get_cache_file_locations() {
  return CACHE_FILE_LOCATIONS; 
}

void set_cache_file_locations(std::string value) {
  CACHE_FILE_LOCATIONS = value;
}

std::string get_cache_file_hdfs_location() {
  return CACHE_FILE_HDFS_LOCATION;
}

// Default SSL location for RHEL and FEDORA
#ifdef __linux__
EXPORT std::string FILEIO_ALTERNATIVE_SSL_CERT_DIR = "/etc/pki/tls/certs";
EXPORT std::string FILEIO_ALTERNATIVE_SSL_CERT_FILE = "/etc/pki/tls/certs/ca-bundle.crt";
#else
EXPORT std::string FILEIO_ALTERNATIVE_SSL_CERT_DIR = "";
EXPORT std::string FILEIO_ALTERNATIVE_SSL_CERT_FILE = "";
#endif
EXPORT int64_t FILEIO_INSECURE_SSL_CERTIFICATE_CHECKS = 0;
REGISTER_GLOBAL(std::string, FILEIO_ALTERNATIVE_SSL_CERT_FILE, true);
REGISTER_GLOBAL(std::string, FILEIO_ALTERNATIVE_SSL_CERT_DIR, true);
REGISTER_GLOBAL(int64_t, FILEIO_INSECURE_SSL_CERTIFICATE_CHECKS, true);

const std::string& get_alternative_ssl_cert_dir() {
  return FILEIO_ALTERNATIVE_SSL_CERT_DIR;
}

const std::string& get_alternative_ssl_cert_file() {
  return FILEIO_ALTERNATIVE_SSL_CERT_FILE;
}

const bool insecure_ssl_cert_checks() {
  return FILEIO_INSECURE_SSL_CERTIFICATE_CHECKS != 0;
}

}

}
