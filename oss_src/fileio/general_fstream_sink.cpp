/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#include <boost/algorithm/string.hpp>
#include <fileio/general_fstream_sink.hpp>
#include <fileio/sanitize_url.hpp>
#include <logger/logger.hpp>
namespace graphlab {
namespace fileio_impl {


general_fstream_sink::general_fstream_sink(std::string file) {
  open_file(file, boost::ends_with(file, ".gz"));
}

general_fstream_sink::general_fstream_sink(std::string file, 
                                               bool gzip_compressed) {
  open_file(file, gzip_compressed);
}

void general_fstream_sink::open_file(std::string file, bool gzip_compressed) {
  sanitized_filename = sanitize_url(file);
  out_file = std::make_shared<union_fstream>(file, std::ios_base::out | std::ios_base::binary);
  is_gzip_compressed = gzip_compressed;
  if (gzip_compressed) {
    compressor = std::make_shared<boost::iostreams::gzip_compressor>();
  }
  // get the underlying stream inside the union stream
  underlying_stream = out_file->get_ostream();
}

bool general_fstream_sink::is_open() const {
  return underlying_stream && !underlying_stream->bad();
}

std::streamsize general_fstream_sink::write(const char* c, 
                                            std::streamsize bufsize) {
  if (is_gzip_compressed) {
    return compressor->write(*underlying_stream, c, bufsize);
  } else {
    underlying_stream->write(c, bufsize);
    if (underlying_stream->fail()) return 0;
    else return bufsize;
  }
}

general_fstream_sink::~general_fstream_sink() {
  // if I am the only reference to the object, close it.
  if (out_file && out_file.unique()) {
    try {
      close();
    } catch (...) {
      logstream(LOG_ERROR) << "Exception occured on closing " 
                           << sanitized_filename 
                           << ". The file may not be properly written" << std::endl;
    }
  }
}

void general_fstream_sink::close() {
  if (compressor) {
    compressor->close(*underlying_stream, std::ios_base::out);
    compressor.reset();
  }
  underlying_stream.reset();
  out_file.reset();
}


bool general_fstream_sink::good() const {
  return underlying_stream && underlying_stream->good();
}

bool general_fstream_sink::bad() const {
  // if stream is NULL. the stream is bad
  if (underlying_stream == nullptr) return true;
  return underlying_stream->bad();
}

bool general_fstream_sink::fail() const {
  // if stream is NULL. the stream is bad
  if (underlying_stream == nullptr) return true;
  return underlying_stream->fail();
}

size_t general_fstream_sink::get_bytes_written() const {
  if (underlying_stream) {
    return underlying_stream->tellp();
  } else {
    return (size_t)(-1);
  }
}

} // namespace fileio_impl
} // namespace graphlab
