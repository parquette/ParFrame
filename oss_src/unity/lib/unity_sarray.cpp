/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#include <cmath>
#include <boost/heap/priority_queue.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/date_time/local_time/local_time.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <unity/lib/unity_sarray.hpp>
#include <unity/lib/unity_sframe.hpp>
#include <unity/lib/flex_dict_view.hpp>
#include <unity/lib/unity_global.hpp>
#include <unity/lib/unity_global_singleton.hpp>
#include <unity/lib/variant.hpp>
#include <fileio/temp_files.hpp>
#include <fileio/curl_downloader.hpp>
#include <fileio/sanitize_url.hpp>
#include <fileio/fs_utils.hpp>
#include <graphlab/util/file_line_count_estimator.hpp>
#include <util/cityhash_gl.hpp>
#include <util/hash_value.hpp>
#include <parallel/atomic.hpp>
#include <parallel/lambda_omp.hpp>
#include <unity/lib/unity_sarray_binary_operations.hpp>
#include <sframe/csv_line_tokenizer.hpp>
#include <sframe/parallel_csv_parser.hpp>
#include <sframe/generic_avro_reader.hpp>
#include <flexible_type/flexible_type_spirit_parser.hpp>
#include <sframe/sframe_constants.hpp>
#include <serialization/oarchive.hpp>
#include <serialization/iarchive.hpp>
#include <unity/lib/auto_close_sarray.hpp>
#include <unity/lib/unity_global.hpp>
#include <unity/lib/image_util.hpp>
#include <sframe_query_engine/operators/all_operators.hpp>
#include <sframe_query_engine/operators/operator_properties.hpp>
#include <sframe_query_engine/planning/planner.hpp>
#include <sframe_query_engine/planning/optimization_engine.hpp>
#include <sframe_query_engine/util/aggregates.hpp>
#include <sframe/rolling_aggregate.hpp>
#include <unity/lib/gl_sarray.hpp>

namespace graphlab {

using namespace query_eval;

static std::shared_ptr<sarray<flexible_type>> get_empty_sarray() {
  // make empty sarray and keep it around, reusing it whenever
  // I need an empty sarray . We are intentionally leaking this object.
  // Otherwise the termination of this will race against the cleanup of the 
  // cache files.
  static std::shared_ptr<sarray<flexible_type> >* empty_sarray = nullptr;
  static graphlab::mutex static_sa_lock;
  std::lock_guard<graphlab::mutex> guard(static_sa_lock);
  if (empty_sarray == nullptr) {
    empty_sarray = new std::shared_ptr<sarray<flexible_type>>();
    (*empty_sarray) = std::make_shared<sarray<flexible_type>>();
    (*empty_sarray)->open_for_write(1);
    (*empty_sarray)->set_type(flex_type_enum::FLOAT);
    (*empty_sarray)->close();
  }
  return *empty_sarray;
}

unity_sarray::unity_sarray() {
  // make empty sarray and keep it around, reusing it whenever
  // I need an empty sarray
  clear();
}

unity_sarray::~unity_sarray() {
}

unity_sarray::unity_sarray(const unity_sarray& other) {
  construct_from_unity_sarray(other);
};

unity_sarray& unity_sarray::operator=(const unity_sarray& other) {
  construct_from_unity_sarray(other);
  return *this;
};

void unity_sarray::construct_from_vector(const std::vector<flexible_type>& vec, 
                                         flex_type_enum type) {
  clear();

  auto sarray_ptr = std::make_shared<sarray<flexible_type>>();

  sarray_ptr->open_for_write();
  sarray_ptr->set_type(type);

  // ok. copy into the writer.
  graphlab::copy(vec.begin(), vec.end(), *sarray_ptr);
  sarray_ptr->close();

  construct_from_sarray(sarray_ptr);
}

void unity_sarray::construct_from_const(const flexible_type& value, size_t size) {
  log_func_entry();
  clear();
  auto type = value.get_type();
  // if I get a None, lets make a constant column of float, all None
  if (type == flex_type_enum::UNDEFINED) {
    type = flex_type_enum::FLOAT;
  }
  m_planner_node = op_constant::make_planner_node(value, type, size);
}

void unity_sarray::construct_from_sarray(std::shared_ptr<sarray<flexible_type>> s_ptr) {
  clear();
  m_planner_node = op_sarray_source::make_planner_node(s_ptr);
}

void unity_sarray::construct_from_planner_node(
    std::shared_ptr<query_eval::planner_node> input) {
  clear();
  
  materialize_options opts;
  opts.only_first_pass_optimizations = true;
  m_planner_node = optimization_engine::optimize_planner_graph(input, opts);

  // Do we need to materialize it for safety's sake?
  if(planner().online_materialization_recommended(m_planner_node)) {
    logstream(LOG_INFO) << "Forced materialization of SArray due to size of lazy graph: " << std::endl;
    m_planner_node = planner().materialize_as_planner_node(m_planner_node);
  }
}

void unity_sarray::construct_from_sarray_index(std::string index) {
  logstream(LOG_INFO) << "Construct sarray from location: " 
                      << sanitize_url(index) << std::endl;
  clear();
  auto status = fileio::get_file_status(index);


  if (fileio::is_web_protocol(index)) {
    // if it is a web protocol, we cannot be certain what type of file it is.
    // HEURISTIC:
    //   assume it is a "directory" and try to load dir_archive.ini
    if (fileio::try_to_open_file(index + "/dir_archive.ini")) {
      status = fileio::file_status::DIRECTORY;
    } else {
      status = fileio::file_status::REGULAR_FILE;
    }
  }

  if (status == fileio::file_status::MISSING) {
    // missing file. fail quick
    log_and_throw_io_failure(sanitize_url(index) + " not found.");
  } if (status == fileio::file_status::REGULAR_FILE) {
    // its a regular file, load it normally
    auto sarray_ptr = std::make_shared<sarray<flexible_type>>(index);
    construct_from_sarray(sarray_ptr);
  } else if (status == fileio::file_status::DIRECTORY) {
    // its a directory, open the directory and verify that it contains an
    // sarray and then load it if it does
    dir_archive dirarc;
    dirarc.open_directory_for_read(index);
    std::string content_value;
    if (dirarc.get_metadata("contents", content_value) == false ||
        content_value != "sarray") {
      log_and_throw("Archive does not contain an SArray");
    }
    std::string prefix = dirarc.get_next_read_prefix();
    auto sarray_ptr = std::make_shared<sarray<flexible_type>>(prefix + ".sidx");
    construct_from_sarray(sarray_ptr);
    dirarc.close();
  }
}

/**
 * Constructs an SArray from a url. Each line of the file will be a row in the
 * resultant SArray, and each row will be of string type. If URL is a directory,
 * or a glob, each matching file will be appended.
 */
void unity_sarray::construct_from_files(std::string url,
                                        flex_type_enum type) {
  std::vector<std::pair<std::string, fileio::file_status>> file_and_status =
      fileio::get_glob_files(url);

  log_func_entry();
  logstream(LOG_INFO) 
      << "Construct sarray from url: " << sanitize_url(url) <<  " type: "
      << flex_type_enum_to_name(type) << std::endl;
  clear();
  csv_line_tokenizer tokenizer;
  tokenizer.delimiter = "\n";
  tokenizer.init();
  sframe sf;
  sf.init_from_csvs(url, 
                    tokenizer, 
                    false /* use_header */ , 
                    false /* continue on failure */ ,
                    false /* store_errors */ ,
                    {{"X1", type}} /* type */ ,
                    std::vector<std::string>(),
                    0 /* row_limit */ );
  auto sarray_ptr = sf.select_column(0);
  construct_from_sarray(sarray_ptr);
}

void unity_sarray::construct_from_autodetect(std::string url, flex_type_enum type) {
  auto status = fileio::get_file_status(url);

  if (fileio::is_web_protocol(url)) {
    // if it is a web protocol, we cannot be certain what type of file it is.
    // HEURISTIC:
    //   assume it is a "directory" and try to load dir_archive.ini
    if (fileio::try_to_open_file(url + "/dir_archive.ini")) {
      status = fileio::file_status::DIRECTORY;
    } else {
      status = fileio::file_status::REGULAR_FILE;
    }
  }

  if (status == fileio::file_status::MISSING) {
    // missing file. might be a glob. try again using construct_from_file
    construct_from_files(url, type);
  } else if (status == fileio::file_status::DIRECTORY) {
    // it is a directory. first see if it is a directory holding an sarray
    bool is_directory_archive = fileio::try_to_open_file(url + "/dir_archive.ini");
    if (is_directory_archive) {
      construct_from_sarray_index(url);
    } else {
      construct_from_files(url, type);
    }
  } else {
    // its a regular file. This is the tricky case
    if (boost::ends_with(url, ".sidx")) {
      construct_from_sarray_index(url);
    } else {
      construct_from_files(url, type);
    }
  }
}

void unity_sarray::construct_from_avro(std::string url) {
  auto status = fileio::get_file_status(url);
  if (status == fileio::file_status::MISSING) {
    log_and_throw_io_failure(std::string("Cannot open ") + sanitize_url(url));
  } else {
    log_func_entry();

    generic_avro_reader reader(url);
    auto type = reader.get_flex_type();

    if (type == flex_type_enum::UNDEFINED)
      log_and_throw("Avro schema is undefined");

    logstream(LOG_INFO) << "Construct sarray from AVRO url: " << sanitize_url(url) <<  " type: "
                        << flex_type_enum_to_name(type) << std::endl;

    auto sarray_ptr = std::make_shared<sarray<flexible_type>>();
    sarray_ptr->open_for_write(1);
    sarray_ptr->set_type(type);

    auto output = sarray_ptr->get_output_iterator(0);
    bool has_more = true;
    size_t num_read = 0;
    size_t progress_interval = 10000;

    flexible_type record;
    while (has_more) {
      if ((num_read >= progress_interval) && (num_read % progress_interval == 0)) {
        logprogress_stream << "Added " << num_read << " records to SArray"
                           << std::endl;
      }
      std::tie(has_more, record) = reader.read_one_flexible_type();

      if (record.get_type() != flex_type_enum::UNDEFINED) {
        (*output) = std::move(record);
        ++output;
        ++num_read;
      } else {
        logstream(LOG_WARNING) << "ignoring undefined record" << std::endl;
      }
    }

    sarray_ptr->close();
    construct_from_sarray(sarray_ptr);
  }
}

void unity_sarray::save_array(std::string target_directory) {
  if (!m_planner_node) {
   log_and_throw("Invalid Sarray");
  }

  dir_archive dirarc;
  dirarc.open_directory_for_write(target_directory);
  dirarc.set_metadata("contents", "sarray");
  std::string prefix = dirarc.get_next_write_prefix();
  save_array_by_index_file(prefix + ".sidx");
  dirarc.close();
}

void unity_sarray::save_array_by_index_file(std::string index_file) {
  // TODO not implemented yet.
  auto sa = get_underlying_sarray();
  sa->save(index_file);
}

void unity_sarray::clear() {
  m_planner_node = 
      query_eval::op_sarray_source::make_planner_node(get_empty_sarray());
}

void unity_sarray::save(oarchive& oarc) const {
  oarc << true;
  std::string prefix = oarc.get_prefix();
  const_cast<unity_sarray*>(this)->save_array_by_index_file(prefix + ".sidx");
}

void unity_sarray::load(iarchive& iarc) {
  clear();
  bool has_sarray;
  iarc >> has_sarray;
  if (has_sarray) {
    std::string prefix = iarc.get_prefix() + ".sidx";
    construct_from_sarray_index(prefix);
  }
}

size_t unity_sarray::size() {
  Dlog_func_entry();
  auto length = infer_planner_node_length(m_planner_node);
  if (length == -1) {
    return get_underlying_sarray()->size();
  } else {
    return length;
  }
}

bool unity_sarray::has_size() {
  auto length = infer_planner_node_length(m_planner_node);
  return length != -1;
}

std::shared_ptr<sarray<flexible_type> > unity_sarray::get_underlying_sarray() {
  Dlog_func_entry();
  auto sf = query_eval::planner().materialize(m_planner_node);
  ASSERT_EQ(sf.num_columns(), 1);
  return sf.select_column(0);
}

std::shared_ptr<planner_node> unity_sarray::get_planner_node() {
  return m_planner_node;
}

flex_type_enum unity_sarray::dtype() {
  Dlog_func_entry();
  auto nodetype = infer_planner_node_type(m_planner_node);
  ASSERT_EQ(nodetype.size(), 1);
  return nodetype[0];
}

std::shared_ptr<unity_sarray_base> unity_sarray::head(size_t nrows) {
  auto sa_head = std::make_shared<sarray<flexible_type>>();
  sa_head->open_for_write(1);
  sa_head->set_type(dtype());
  auto out = sa_head->get_output_iterator(0);
  size_t row_counter = 0;
  if (nrows > 0) {
    auto callback = [&out, &row_counter, nrows](size_t segment_id,
                                                const std::shared_ptr<sframe_rows>& data) {
      for (const auto& row : (*data)) {
        *out = row[0];
        ++out;
        ++row_counter;
        if (row_counter == nrows) return true;
      }
      return false;
    };
    try {
      query_eval::planner().materialize(this->get_planner_node(),
                                        callback, 
                                        1, /* process in as 1 segment */
                                        false /* do not partially materialize */ );
    } catch (...) {
      // it's ok, let's try it again with partial materialize
      sa_head = std::make_shared<sarray<flexible_type>>();
      sa_head->open_for_write(1);
      sa_head->set_type(dtype());
      auto out = sa_head->get_output_iterator(0);
      row_counter = 0;
      query_eval::planner().materialize(this->get_planner_node(),
                                        callback,
                                        1 /* process in as 1 segment */,
                                        true /* partial materialize */);
    }
  }
  sa_head->close();
  auto ret = std::make_shared<unity_sarray>();
  ret->construct_from_sarray(sa_head);
  return ret;
}

std::shared_ptr<unity_sarray_base> unity_sarray::transform(const std::string& lambda,
                                                           flex_type_enum type,
                                                           bool skip_undefined,
                                                           int seed) {
  log_func_entry();

  // create a le_transform operator to lazily evaluate this
  auto lambda_node = 
      query_eval::op_lambda_transform::
              make_planner_node(m_planner_node,
                                lambda,
                                type,
                                std::vector<std::string>(),
                                skip_undefined,
                                seed);
  auto ret_unity_sarray = std::make_shared<unity_sarray>();
  ret_unity_sarray->construct_from_planner_node(lambda_node);
  return ret_unity_sarray;
}


std::shared_ptr<unity_sarray_base> unity_sarray::transform_native(
    const function_closure_info& toolkit_fn_closure,
    flex_type_enum type,
    bool skip_undefined,
    int seed) {
  auto native_execute_function =
                  get_unity_global_singleton()
                  ->get_toolkit_function_registry()
                  ->get_native_function(toolkit_fn_closure);

  auto fn = 
      [native_execute_function, skip_undefined](const sframe_rows::row& f)->flexible_type {
        if (skip_undefined && f[0].get_type() == flex_type_enum::UNDEFINED) {
          return flex_undefined();
        } else {
          variant_type var = f[0];
          return variant_get_value<flexible_type>(native_execute_function({var}));
        }
      };
  auto ret_sarray = std::make_shared<unity_sarray>();

  ret_sarray->construct_from_planner_node(
      query_eval::op_transform::make_planner_node(m_planner_node, fn, type, seed));

  return ret_sarray;
}

std::shared_ptr<unity_sarray_base> unity_sarray::transform_lambda(
    std::function<flexible_type(const flexible_type&)> function,
    flex_type_enum type,
    bool skip_undefined,
    int seed) {

  auto fn = [function, type, skip_undefined](const sframe_rows::row& f)->flexible_type {
    if (skip_undefined && f[0].get_type() == flex_type_enum::UNDEFINED) {
      return flex_undefined();
    } else {
      flexible_type ret = function(f[0]);
      if (ret.get_type() == type || ret.get_type() == flex_type_enum::UNDEFINED) {
        return ret; 
      } else {
        flexible_type changed_ret(type);
        changed_ret.soft_assign(ret);
        return changed_ret;
      }
    }
  };
  auto ret_sarray = std::make_shared<unity_sarray>();

  ret_sarray->construct_from_planner_node(
      query_eval::op_transform::make_planner_node(m_planner_node, fn, type, seed));

  return ret_sarray;
}

std::shared_ptr<unity_sarray_base> unity_sarray::append(
    std::shared_ptr<unity_sarray_base> other) {

  std::shared_ptr<unity_sarray> other_unity_sarray =
      std::static_pointer_cast<unity_sarray>(other);

  if (this->dtype() != other->dtype()) {
    log_and_throw("Both SArrays have to have the same value type");
  }

  auto ret_sarray = std::make_shared<unity_sarray>();

  ret_sarray->construct_from_planner_node(
      query_eval::op_append::make_planner_node(m_planner_node, 
                                               other_unity_sarray->m_planner_node));

  return ret_sarray;
}

std::shared_ptr<unity_sarray_base> unity_sarray::vector_slice(size_t start, size_t end) {
  log_func_entry();
  auto this_dtype = dtype();
  if (this_dtype != flex_type_enum::LIST && this_dtype != flex_type_enum::VECTOR) {
    log_and_throw("Cannot slice a non-vector array.");
  }
  if (end <= start) {
    log_and_throw("end of slice must be greater than start of slice.");
  }

  flex_type_enum output_dtype =
      (end == start + 1 && this_dtype == flex_type_enum::VECTOR) ? 
        flex_type_enum::FLOAT : this_dtype;

  auto fn = 
      [=](const sframe_rows::row& row)->flexible_type {
        const auto& f = row[0];
        if (f.get_type() == flex_type_enum::UNDEFINED) {
          return f;
        } else {
          // if we can slice from the array
          if (end <= f.size()) {
            // yup we have enough room to slice the array.
            flexible_type ret;
            if (output_dtype == flex_type_enum::FLOAT) {
              // length 1
              ret.reset(flex_type_enum::FLOAT);
              ret.soft_assign(f[start]);
            } else {
              // length many
              ret.reset(output_dtype);
              for (size_t i = start; i < end; ++i) {
                if (this_dtype == flex_type_enum::VECTOR) {
                  ret.push_back(f[i]);
                } else {
                  ret.push_back(f.array_at(i));
                }
              }
            }
            return ret;
          } else {
            // no room to slice the array. fail.
            return FLEX_UNDEFINED;
          }
        }
      };

  auto ret_sarray = std::make_shared<unity_sarray>();

  ret_sarray->construct_from_planner_node(
      query_eval::op_transform::make_planner_node(m_planner_node, fn, output_dtype));

  return ret_sarray;
}

std::shared_ptr<unity_sarray_base> unity_sarray::filter(const std::string& lambda,
                                                        bool skip_undefined, int seed) {
  return logical_filter(
     std::static_pointer_cast<unity_sarray>(transform(lambda, 
                                                      flex_type_enum::UNDEFINED, 
                                                      skip_undefined, 
                                                      seed)));
}


std::shared_ptr<unity_sarray_base>
unity_sarray::logical_filter(std::shared_ptr<unity_sarray_base> index) {
  log_func_entry();

  ASSERT_TRUE(index != nullptr);
  std::shared_ptr<unity_sarray> other_array = 
      std::static_pointer_cast<unity_sarray>(index);
  // Checking the size of index array is the same 
  if (this->size() != other_array->size()) {
    log_and_throw("Logical filter array must have the same size");
  }

  std::shared_ptr<unity_sarray> other_array_binarized = 
      std::static_pointer_cast<unity_sarray>(
      other_array->transform_lambda(
            [](const flexible_type& f)->flexible_type {
              return (flex_int)(!f.is_zero());
            }, flex_type_enum::INTEGER, true, 0));

  auto ret = std::make_shared<unity_sarray>();
  ret->construct_from_planner_node(
      op_logical_filter::make_planner_node(m_planner_node, 
                                           other_array_binarized->m_planner_node));
  return ret;
}


std::shared_ptr<unity_sarray_base> unity_sarray::topk_index(size_t k, bool reverse) {
  log_func_entry();

  unity_sarray_binary_operations::
      check_operation_feasibility(dtype(), dtype(), "<");

  auto sarray_ptr = get_underlying_sarray();
  // check that I have less than comparable of this type
  struct pqueue_value_type {
    flexible_type val;
    size_t segment_id;
    size_t segment_offset;
    bool operator<(const pqueue_value_type& other) const {
      return val < other.val;
    }
  };
  typedef boost::heap::priority_queue<pqueue_value_type,
          //  darn this is ugly this defines the comparator type
          //  as a generic std::function
          boost::heap::compare<
              std::function<bool(const pqueue_value_type&,
                                 const pqueue_value_type&)> > > pqueue_type;

  // ok done. now we need to merge the values from all of the queues
  pqueue_type::value_compare comparator;
  if (reverse) {
    comparator = std::less<pqueue_value_type>();
  } else {
    comparator =
        [](const pqueue_value_type& a, const pqueue_value_type& b)->bool {
          return !(a < b);
        };
  }

  auto sarray_reader = sarray_ptr->get_reader(thread::cpu_count());
  std::vector<pqueue_type> queues(sarray_reader->num_segments(),
                                  pqueue_type(comparator));

  // parallel insert into num-segments of priority queues
  parallel_for(0, sarray_reader->num_segments(),
               [&](size_t idx) {
                 auto begin = sarray_reader->begin(idx);
                 auto end = sarray_reader->end(idx);
                 size_t ctr = 0;
                 while(begin != end) {
                   if (!((*begin).is_na())) {
                     queues[idx].push(pqueue_value_type{*begin, idx, ctr});
                     if (queues[idx].size() > k) queues[idx].pop();
                   }
                   ++ctr;
                   ++begin;
                 }
               });
  pqueue_type master_queue(comparator);

  for(auto& subqueue : queues) {
    for (auto& pqueue_value: subqueue) {
      master_queue.push(pqueue_value);
      if (master_queue.size() > k) master_queue.pop();
    }
  }
  // good. now... split this into the collection of segments as the
  // values to flag as true
  std::vector<std::vector<size_t> > values_to_flag(sarray_reader->num_segments());
  for (auto& pqueue_value: master_queue) {
    values_to_flag[pqueue_value.segment_id].push_back(pqueue_value.segment_offset);
  }
  // sort the subsequences
  for (auto& subvec: values_to_flag) {
    std::sort(subvec.begin(), subvec.end());
  }

  // now we need to write out the segments
  auto out_sarray = std::make_shared<sarray<flexible_type>>();
  out_sarray->open_for_write(sarray_reader->num_segments());
  out_sarray->set_type(flex_type_enum::INTEGER);

  parallel_for(0, sarray_reader->num_segments(),
               [&](size_t idx) {
                 auto output = out_sarray->get_output_iterator(idx);
                 size_t ctr = 0;
                 size_t subvecidx = 0;
                 size_t target_elements = sarray_reader->segment_length(idx);
                 // write some mix of 0 and 1s. outputing 1s
                 // for each time the ctr an entry in values_to_flag[idx]
                 while(ctr < target_elements) {
                   // when we run out of elements in subvec we break
                   if (subvecidx >= values_to_flag[idx].size()) break;
                   if (values_to_flag[idx][subvecidx] == ctr) {
                       (*output) = 1;
                       ++subvecidx;
                    } else {
                       (*output) = 0;
                    }
                    ++output;
                    ++ctr;
                 }
                 // here we have ran out of elements in the sub vector
                 // and we just output all zeros
                 while(ctr < target_elements) {
                   (*output) = 0;
                    ++output;
                    ++ctr;
                 }
               });

  out_sarray->close();
  std::shared_ptr<unity_sarray> ret_unity_sarray(new unity_sarray());
  ret_unity_sarray->construct_from_sarray(out_sarray);

  return ret_unity_sarray;
}


size_t unity_sarray::num_missing() {
  log_func_entry();
  auto reductionfn = [](const flexible_type& f, size_t& n_missing)->void {
    if (f.get_type() == flex_type_enum::UNDEFINED) ++n_missing;
  };
  auto combinefn = [](const size_t& left, size_t& right)->void {
    right += left;
  };

  return query_eval::reduce<size_t>(m_planner_node, 
                                    reductionfn, combinefn, 0);
}

bool unity_sarray::all() {
  log_func_entry();
  
  auto reductionfn = [](const flexible_type& f, int& segment_all)->void {
    segment_all &= !f.is_zero();
  };

  return query_eval::reduce<int>(m_planner_node, 
                                 reductionfn, reductionfn, 1) > 0;
}

bool unity_sarray::any() {
  log_func_entry();
  auto reductionfn = [](const flexible_type& f, int& segment_all)->bool {
    segment_all |= !f.is_zero();
    // keep continuing if we have not
    // hit a non-empty case.
    // \note Earlier versions of reduce allowed for early stopping by
    // returning false. This is currently, not yet supported in the new
    // query evaluator and is simply ignored. We will maintain it this way
    // for now, pending future enhancements to the query evaluator.
    return !segment_all; 
  };

  return query_eval::reduce<int>(m_planner_node, 
                                 reductionfn, reductionfn, 0) > 0;
}

flexible_type unity_sarray::max() {
  log_func_entry();

  flex_type_enum cur_type = dtype();

  if(cur_type == flex_type_enum::INTEGER ||
       cur_type == flex_type_enum::DATETIME||
     cur_type == flex_type_enum::FLOAT) {

    flexible_type max_val;
    if(cur_type == flex_type_enum::INTEGER) {
      max_val = std::numeric_limits<flex_int>::lowest();
    } else if(cur_type == flex_type_enum::DATETIME) {
      max_val = flex_date_time(
          flexible_type_impl::ptime_to_time_t(boost::posix_time::min_date_time));
    } else if(cur_type == flex_type_enum::FLOAT) {
      max_val = std::numeric_limits<flex_float>::lowest();
    }

    auto reductionfn = [&](const flexible_type& f, flexible_type& maxv)->void {
                          if (f.get_type() != flex_type_enum::UNDEFINED) {
                            if (maxv.get_type() == flex_type_enum::UNDEFINED) maxv = max_val;
                            if(f > maxv) maxv = f;
                          }
                        };

    max_val =
      query_eval::reduce<flexible_type>(m_planner_node, reductionfn,
                                        reductionfn, flex_undefined());

    return max_val;
  } else {
    log_and_throw("Cannot perform on non-numeric types!");
  }
}

flexible_type unity_sarray::min() {
  log_func_entry();

  flex_type_enum cur_type = dtype();
  if(cur_type == flex_type_enum::INTEGER ||
     cur_type == flex_type_enum::DATETIME||
     cur_type == flex_type_enum::FLOAT) {

    flexible_type min_val;
    if(cur_type == flex_type_enum::INTEGER) {
      min_val = std::numeric_limits<flex_int>::max();
    } else if(cur_type == flex_type_enum::DATETIME) {
      min_val = flex_date_time(
          flexible_type_impl::ptime_to_time_t(boost::posix_time::max_date_time));
    } else if(cur_type == flex_type_enum::FLOAT) {
      min_val = std::numeric_limits<flex_float>::max();
    }
    auto reductionfn = [&](const flexible_type& f, flexible_type& minv)->void {
                    if (f.get_type() != flex_type_enum::UNDEFINED) {
                      if (minv.get_type() == flex_type_enum::UNDEFINED) minv = min_val;
                      if(f < minv) minv = f;
                    }
                  };

    min_val =
        query_eval::reduce<flexible_type>(m_planner_node, reductionfn,
                                          reductionfn, flex_undefined());
    return min_val;
  } else {
    log_and_throw("Cannot perform on non-numeric types!");
  }

  return flex_undefined();
}

flexible_type unity_sarray::sum() {
  log_func_entry();

  if(this->size() > 0) {
    flex_type_enum cur_type = dtype();
    if(cur_type == flex_type_enum::INTEGER ||
       cur_type == flex_type_enum::FLOAT) {

      flexible_type start_val;
      if(cur_type == flex_type_enum::INTEGER) {
        start_val = flex_int(0);
      } else {
        start_val = flex_float(0);
      }

      auto reductionfn =
          [](const flexible_type& f, flexible_type& sum)->void {
            if (f.get_type() != flex_type_enum::UNDEFINED) {
              sum += f;
            }
          };

      flexible_type sum_val =
        query_eval::reduce<flexible_type>(m_planner_node, reductionfn, 
                                          reductionfn, start_val);

      return sum_val;
    } else if (cur_type == flex_type_enum::VECTOR) {

      bool failure = false;
      auto reductionfn =
          [&failure](const flexible_type& f, std::pair<bool, flexible_type>& sum)->bool {
                      if (f.get_type() != flex_type_enum::UNDEFINED) {
                        if (sum.first == false) {
                          // initial val
                          sum.first = true;
                          sum.second = f;
                        } else if (sum.second.size() == f.size()) {
                          // accumulation
                          sum.second += f;
                        } else {
                          // length mismatch. fail
                          failure = true;
                          return false;
                        }
                      }
                      return true;
                    };

      auto combinefn =
          [&failure](const std::pair<bool, flexible_type>& f, std::pair<bool, flexible_type>& sum)->bool {
            if (sum.first == false) {
              // initial state
              sum = f;
            } else if (f.first == false) {
              // there is no f to add.
              return true;
            } else if (sum.second.size() == f.second.size()) {
              // accumulation
              sum.second += f.second;
            } else {
              // length mismatch
              failure = true;
              return false;
            }
            return true;
          };

      std::pair<bool, flexible_type> start_val{false, flex_vec()};
      std::pair<bool, flexible_type> sum_val =
        query_eval::reduce<std::pair<bool, flexible_type> >(m_planner_node, reductionfn,
                                                            combinefn , start_val);

      // failure indicates there is a missing value, or there is vector length
      // mismatch
      if (failure) {
        log_and_throw("Cannot perform sum over vectors of variable length.");
      }

      return sum_val.second;

    } else {
      log_and_throw("Cannot perform on non-numeric types!");
    }
  }

  return flex_undefined();
}

flexible_type unity_sarray::mean() {
  log_func_entry();


  flex_type_enum cur_type = dtype();
  if(cur_type == flex_type_enum::INTEGER ||
     cur_type == flex_type_enum::FLOAT ) {

    std::pair<double, size_t> start_val{0.0, 0.0}; // mean, and size
    auto reductionfn =
        [](const flexible_type& f,
           std::pair<double, size_t>& mean)->void {
          if (f.get_type() != flex_type_enum::UNDEFINED) {
            // Divide done each time to keep from overflowing
            ++mean.second;
            mean.first += (flex_float(f) - mean.first) / double(mean.second);
          }
        };

    // second reduction function to aggregate result
    auto aggregatefn = [](const std::pair<double, size_t>& f,
                          std::pair<double, size_t> &mean)->void {
      // weighted sum of the two
      if (mean.second + f.second > 0) {
        mean.first =
            mean.first * ((double)mean.second / (double)(mean.second + f.second)) +
            f.first * ((double)f.second / (double)(mean.second + f.second));
        mean.second += f.second;
      }
    };

    std::pair<double, size_t> mean_val =
        query_eval::reduce<std::pair<double, size_t> >(m_planner_node, reductionfn, 
                                                       aggregatefn, start_val);

    if (mean_val.second == 0) return flex_undefined();
    else return mean_val.first;


  } else if(cur_type == flex_type_enum::VECTOR) {

    std::pair<flexible_type, size_t> start_val{flexible_type(), 0}; // mean, and size
    auto reductionfn =
        [](const flexible_type& f,
           std::pair<flexible_type, size_t>& mean)->void {
          // In the first operation in case of vector, initialzed vector will be size 0
          // so we cannot simply add. Copy instead.
          if (mean.second == 0){
            ++mean.second;
            mean.first = f;
          } else {
            if (f.get_type() == flex_type_enum::VECTOR && f.size() != mean.first.size()){
              log_and_throw("Cannot perform mean on SArray with vectors of different lengths.");
            }
            // Divide done each time to keep from overflowing
            ++mean.second;
            mean.first += (f - mean.first) / double(mean.second);
          }
        };

    // second reduction function to aggregate result
    auto aggregatefn = [](const std::pair<flexible_type, size_t>& f,
                          std::pair<flexible_type, size_t> &mean)->void {
      // weighted sum of the two
      if (mean.second > 0 &&  f.second > 0) {
        if (mean.first.get_type() == flex_type_enum::VECTOR && f.first.size() != mean.first.size()){
          log_and_throw("Cannot perform mean on SArray with vectors of different lengths.");
        }
        mean.first =
            mean.first * ((double)mean.second / (double)(mean.second + f.second)) +
            f.first * ((double)f.second / (double)(mean.second + f.second));
        mean.second += f.second;
        // If count of mean is 0, simply copy the other over since we cannot add
        // vectors of different lengths.
      } else if (f.second > 0){
        mean.first = f.first;
        mean.second = f.second;
      }
    };

    std::pair<flexible_type, size_t> mean_val =
        query_eval::reduce<std::pair<flexible_type, size_t> >(m_planner_node, reductionfn,
                                                              aggregatefn, start_val);

    if (mean_val.second == 0) return flex_undefined();
    else return mean_val.first;
  } else {
    log_and_throw("Cannot perform on types that are not numeric or vector!");
  }
}

flexible_type unity_sarray::std(size_t ddof) {
  log_func_entry();
  flexible_type variance = this->var(ddof);
  // Return whatever error happens
  if(variance.get_type() == flex_type_enum::UNDEFINED) {
    return variance;
  }

  return std::sqrt((flex_float)variance);
}

flexible_type unity_sarray::var(size_t ddof) {
  log_func_entry();

  if(this->size() > 0) {
    size_t size = this->size();

    flex_type_enum cur_type = dtype();
    if(cur_type == flex_type_enum::INTEGER ||
       cur_type == flex_type_enum::FLOAT) {

      if(ddof >= size) {
        log_and_throw("Cannot calculate with degrees of freedom <= 0");
      }

      // formula from
      // http://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Incremental_Algorithm
      struct incremental_var : public IS_POD_TYPE {
        size_t n = 0;
        double mean = 0;
        double m2 = 0;
      };
      auto reductionfn = [](const flexible_type& f, incremental_var& var)->bool {
                      if (f.get_type() != flex_type_enum::UNDEFINED) {
                        ++var.n;
                        double delta = flex_float(f) - var.mean;
                        var.mean += delta / var.n;
                        var.m2 += delta * (flex_float(f) - var.mean);
                      }
                      return true;
                    };

      auto aggregatefn = [](const incremental_var& f, incremental_var& var)->bool {
        double delta = f.mean - var.mean;
        if (var.n + f.n > 0) {
          var.mean =
              var.mean * ((double)var.n / (double)(var.n + f.n)) +
              f.mean * ((double)f.n / (double)(var.n + f.n));
          var.m2 += f.m2 + delta * var.n * delta * f.n /(double)(var.n + f.n);
          var.n += f.n;
        }
        return true;
      };

      incremental_var var =
        query_eval::reduce<incremental_var>(m_planner_node, reductionfn, 
                                            aggregatefn, incremental_var());

      // Divide by degrees of freedom and return
      return var.m2 / flex_float(var.n - ddof);
    } else {
      log_and_throw("Cannot perform on non-numeric types!");
    }
  }
  return flex_undefined();
}

std::shared_ptr<unity_sarray_base> unity_sarray::str_to_datetime(std::string format) {
  log_func_entry();
  flex_type_enum current_type = dtype();
  if (current_type != flex_type_enum::STRING) {
    log_and_throw("input SArray must be string type.");
  }
  if (format == "ISO") {
    format = "%Y%m%dT%H%M%S%F%q";
  }

  const size_t max_n_threads = thread::cpu_count();
  std::vector<std::shared_ptr<std::istringstream> > streams(max_n_threads);
  for (size_t index = 0; index < max_n_threads;index++) {
    std::shared_ptr<std::istringstream> ss(new std::istringstream);
    ss->exceptions(std::ios_base::failbit);
    ss->imbue(std::locale(ss->getloc(),new boost::local_time::local_time_input_facet(format)));
    streams[index] = ss;
  }
  auto transform_fn = [streams, format](const flexible_type& f)->flexible_type {
    size_t thread_idx = thread::thread_id();
    const auto& stream = streams[thread_idx];
    try {
      boost::local_time::local_date_time ldt(boost::posix_time::not_a_date_time);
      stream->str(f.get<flex_string>());
      (*stream) >> ldt; // do the parse

      boost::posix_time::ptime p = ldt.utc_time();
      std::time_t _time = flexible_type_impl::ptime_to_time_t(p);
      int32_t microseconds = flexible_type_impl::ptime_to_fractional_microseconds(p);
      int32_t timezone_offset = flex_date_time::EMPTY_TIMEZONE;
      if(ldt.zone()) {
        timezone_offset = 
            (int32_t)ldt.zone()->base_utc_offset().total_seconds() / 
            flex_date_time::TIMEZONE_RESOLUTION_IN_SECONDS;
      }
      return flexible_type(flex_date_time(_time,timezone_offset, microseconds));
    } catch(std::exception& ex) {
      log_and_throw("Unable to interpret " + f.get<flex_string>() + 
                    " as string with " + format + " format");
    }
  };
  auto ret = transform_lambda(transform_fn, 
                              flex_type_enum::DATETIME, 
                              true /*skip undefined*/, 
                              0 /*random seed*/);
  // force materialization
  ret->materialize();
  return ret;
}

std::shared_ptr<unity_sarray_base> unity_sarray::datetime_to_str(const std::string format) {
  flex_type_enum current_type = dtype();
  if (current_type != flex_type_enum::DATETIME) {
    log_and_throw("input SArray must be datetime type.");
  }

  const size_t max_n_threads = thread::cpu_count();
  std::vector<std::shared_ptr<std::ostringstream> > streams(max_n_threads);
  for (size_t index = 0; index < max_n_threads;index++) {
    std::shared_ptr<std::ostringstream> ss(new std::ostringstream);
    ss->exceptions(std::ios_base::failbit);
    ss->imbue(std::locale(ss->getloc(),
                          new boost::local_time::local_time_facet(format.c_str())));
    streams[index] = ss;
  }

  using namespace boost;
  using namespace local_time;
  using namespace gregorian;
  using posix_time::time_duration;

  time_zone_names empty_timezone("", "", "", ""); 
  time_duration empty_utc_offset(0,0,0);
  dst_adjustment_offsets empty_adj_offsets(time_duration(0,0,0), 
                                           time_duration(0,0,0), 
                                           time_duration(0,0,0));
  time_zone_ptr empty_tz(
      new custom_time_zone(empty_timezone, empty_utc_offset, 
                           empty_adj_offsets, 
                           boost::shared_ptr<dst_calc_rule>()));

  auto transform_fn = [format, streams, empty_tz](const flexible_type& f) -> flexible_type {
    size_t thread_idx = thread::thread_id();
    const auto& stream = streams[thread_idx];
    flexible_type ret;
    try {
      const flex_date_time & dt = f.get<flex_date_time>();
      if (dt.time_zone_offset() != flex_date_time::EMPTY_TIMEZONE) {
        std::string prefix = "0.";
        int sign_adjuster = 1;
        if(dt.time_zone_offset() < 0) {
          sign_adjuster = -1;
          prefix = "-0.";
        }
        // prepend a GMT0. or GMT-0. to the string for the timezone information
        // TODO: This can be optimized by precomputing this for all zones outsize
        // of the function. 
        boost::local_time::time_zone_ptr zone(
            new boost::local_time::posix_time_zone(
                "GMT" + prefix + 
                std::to_string(sign_adjuster * 
                               dt.time_zone_offset() * 
                               flex_date_time::TIMEZONE_RESOLUTION_IN_MINUTES)));
        boost::local_time::local_date_time az(
            flexible_type_impl::ptime_from_time_t(dt.posix_timestamp(), 
                                                  dt.microsecond()), zone);
        (*stream) << az;
      } else {
        boost::local_time::local_date_time az(
            flexible_type_impl::ptime_from_time_t(dt.posix_timestamp(), 
                                                  dt.microsecond()),
            empty_tz);
        (*stream) << az;
      }
      ret = stream->str();
      stream->str(std::string()); // need to clear stringstream buffer
    } catch(...) {
      log_and_throw("Unable to interpret " + f.get<flex_string>() 
                    + " as string with " + format + " format");
    }
    return ret;
  };

  auto ret = transform_lambda(transform_fn, 
                              flex_type_enum::STRING, 
                              true /*skip undefined*/, 
                              0 /*random seed*/);
  // force materialization
  ret->materialize();
  return ret;
}

std::shared_ptr<unity_sarray_base> unity_sarray::astype(flex_type_enum dtype, 
                                                        bool undefined_on_failure) {
  auto ret = lazy_astype(dtype, undefined_on_failure);
  if (undefined_on_failure == false && 
      this->dtype() == flex_type_enum::STRING && dtype != flex_type_enum::STRING) {
    // if we are parsing, materialize
    ret->materialize();
  }
  return ret;
}
std::shared_ptr<unity_sarray_base> unity_sarray::lazy_astype(flex_type_enum dtype, 
                                                             bool undefined_on_failure) {
  log_func_entry();

  flex_type_enum current_type = this->dtype();
  // Special path for converting image sarray to vector type.
  if (current_type == flex_type_enum::IMAGE && 
      dtype == flex_type_enum::VECTOR) {
      return image_util::image_sarray_to_vector_sarray(
          std::static_pointer_cast<unity_sarray>(shared_from_this()),
          undefined_on_failure);
  }

  // if no changes. just keep the identity function
  if (dtype == current_type) {
    return std::static_pointer_cast<unity_sarray>(shared_from_this());
  }

  if(! (flex_type_is_convertible(current_type, dtype) ||
        (current_type == flex_type_enum::STRING && dtype == flex_type_enum::INTEGER) ||
        (current_type == flex_type_enum::STRING && dtype == flex_type_enum::FLOAT) ||
        (current_type == flex_type_enum::STRING && dtype == flex_type_enum::VECTOR) ||
        (current_type == flex_type_enum::STRING && dtype == flex_type_enum::LIST) ||
        (current_type == flex_type_enum::STRING && dtype == flex_type_enum::DICT)
       )) {
    log_and_throw("Not able to cast to given type");
  }


  // The assigment operator takes care of casting
  if (current_type == flex_type_enum::STRING) {
    flexible_type_parser parser;
    // we need to treat strings with special care
    // we need to perform a lexical cast
    auto transform_fn = [dtype,undefined_on_failure,parser]
        (const flexible_type& f)mutable ->flexible_type {
      if (f.get_type() == flex_type_enum::UNDEFINED) return f;
      flexible_type ret;
      try {
        if (dtype == flex_type_enum::INTEGER) {
          ret = std::stoll(f.get<flex_string>());
        } else if (dtype == flex_type_enum::FLOAT) {
          ret = std::stod(f.get<flex_string>());
        } else if (dtype == flex_type_enum::VECTOR) {
          bool success;
          const std::string& val = f.get<flex_string>();
          const char* c = val.c_str();
          std::tie(ret, success) = parser.vector_parse(&c, val.length());
          if (!success) {
            if (undefined_on_failure) ret = FLEX_UNDEFINED;
            else log_and_throw("Cannot convert to array");
          }
        } else if (dtype == flex_type_enum::LIST) {
          bool success;
          const std::string& val = f.get<flex_string>();
          const char* c = val.c_str();
          std::tie(ret, success) = parser.recursive_parse(&c, val.length());
          if (!success) {
            if (undefined_on_failure) ret = FLEX_UNDEFINED;
            else log_and_throw("Cannot convert to list");
          }
        } else if (dtype == flex_type_enum::DICT) {
          bool success;
          const std::string& val = f.get<flex_string>();
          const char* c = val.c_str();
          std::tie(ret, success) = parser.dict_parse(&c, val.length());
          if (!success) {
            if (undefined_on_failure) ret = FLEX_UNDEFINED;
            else log_and_throw("Cannot convert to dict");
          }
        }
      } catch(const std::string& s) {
        if (undefined_on_failure) ret = FLEX_UNDEFINED;
        else log_and_throw("Unable to interpret " + 
                           f.get<flex_string>() + " as the target type.");
      } catch(const std::exception& s) {
        if (undefined_on_failure) ret = FLEX_UNDEFINED;
        else log_and_throw("Unable to interpret " + 
                           f.get<flex_string>() + " as the target type.");
      }
      return ret;
    };

    auto ret = transform_lambda(transform_fn, 
                                dtype,
                                true /*skip undefined*/, 
                                0 /*random seed*/);
    return ret;
  } else {
    auto ret = transform_lambda([dtype](const flexible_type& f)->flexible_type {
                                  flexible_type ret(dtype);
                                  ret.soft_assign(f);
                                  return ret;
                                }, 
                                dtype,
                                true /*skip undefined*/, 
                                0 /*random seed*/);
    return ret;
  }
}

std::shared_ptr<unity_sarray_base> unity_sarray::clip(flexible_type lower,
                                                      flexible_type upper) {
  log_func_entry();
  flex_type_enum cur_type = dtype();
  if(cur_type == flex_type_enum::INTEGER ||
     cur_type == flex_type_enum::FLOAT ||
     cur_type == flex_type_enum::VECTOR) {
    // Check types of lower and upper for numeric/undefined
    if((lower.get_type() != flex_type_enum::INTEGER &&
        lower.get_type() != flex_type_enum::FLOAT &&
        lower.get_type() != flex_type_enum::UNDEFINED) ||
       (upper.get_type() != flex_type_enum::INTEGER &&
        upper.get_type() != flex_type_enum::FLOAT &&
        upper.get_type() != flex_type_enum::UNDEFINED)) {
      log_and_throw("Must give numeric thresholds!");
    }

    // If undefined, that threshold doesn't exist
    bool clip_lower = !(lower.get_type() == flex_type_enum::UNDEFINED);
    bool clip_upper = !(upper.get_type() == flex_type_enum::UNDEFINED);

    if(clip_lower && clip_upper) {
      if(lower > upper) {
        log_and_throw("Upper clip value must be less than lower value.");
      }
    } else if(!clip_lower && !clip_upper) {
      // No change to the SArray, just return the same one
      return std::static_pointer_cast<unity_sarray>(shared_from_this());
    }

    bool threshold_is_float = (lower.get_type() == flex_type_enum::FLOAT) ||
                              (upper.get_type() == flex_type_enum::FLOAT);

    flex_type_enum new_type = cur_type;
    if(cur_type == flex_type_enum::INTEGER && threshold_is_float) {
      // If the threshold is float, the result sarray is always float
      new_type = flex_type_enum::FLOAT;
    } else if(cur_type == flex_type_enum::FLOAT && !threshold_is_float) {
      // Threshold must be a float to compare against a list of floats
      if(clip_lower) lower = flex_float(lower);
      if(clip_upper) upper = flex_float(upper);
    }

    auto transformfn = [=](const flexible_type& f)->flexible_type {
      if (f.get_type() == flex_type_enum::UNDEFINED) return f;
      else if (f.get_type() == flex_type_enum::VECTOR) {
        flexible_type newf = f;
        for (size_t i = 0;i < newf.size(); ++i) {
          if(clip_lower && (newf[i] < lower)) {
            newf[i] = lower;
          } else if(clip_upper && (newf[i] > upper)) {
            newf[i] = upper;
          }
        }
        return newf;
      } else {
        // float or integer
        if(clip_lower && (f < lower)) {
          return lower;
        } else if(clip_upper && (f > upper)) {
          return upper;
        }
      }
      return f;
    };

    auto ret = transform_lambda(transformfn, 
                                new_type,
                                true /*skip undefined*/, 
                                0 /*random seed*/);
    return ret;
  } else {
    log_and_throw("Cannot perform on non-numeric types");
  }
}


size_t unity_sarray::nnz() {
  log_func_entry();
  auto reductionfn = 
      [&](const flexible_type& f, size_t& ctr)->void {
        if(!f.is_zero()) ++ctr;
      };
  auto combinefn = 
      [&](const size_t& f, size_t& ctr)->void {
        ctr += f;
      };
  return query_eval::reduce<size_t>(m_planner_node, 
                                    reductionfn, combinefn, 0);
}

std::shared_ptr<unity_sarray_base> unity_sarray::scalar_operator(flexible_type other,
                                                                 std::string op,
                                                                 bool right_operator) {
  flex_type_enum left_type, right_type;

  if (!right_operator) {
    // this is a left operator. we are doing array [op] other
    left_type = dtype();
    right_type = other.get_type();
  } else {
    // this is a right operator. we are doing other [op] array
    left_type = other.get_type();
    right_type = dtype();
  }

  // check for correctness and figure out the output type,
  // and get the operation we need to perform

  unity_sarray_binary_operations::
      check_operation_feasibility(left_type, right_type, op);

  flex_type_enum output_type = unity_sarray_binary_operations::
      get_output_type(left_type, right_type, op);

  auto binaryfn = unity_sarray_binary_operations::
      get_binary_operator(left_type, right_type, op);

  // quick exit for empty array
  if (size() == 0) {
    std::shared_ptr<unity_sarray> ret(new unity_sarray);
    ret->construct_from_vector(std::vector<flexible_type>(), output_type);
    return ret;
  }

  // create the lazy evalation transform operator from the source
  std::shared_ptr<unity_sarray> ret_unity_sarray(new unity_sarray());
  if (other.get_type() != flex_type_enum::UNDEFINED) {
    auto transformfn = [=](const flexible_type& f)->flexible_type {
          if (f.get_type() == flex_type_enum::UNDEFINED) {
            return f;
          } else {
            return right_operator ? binaryfn(other, f) : binaryfn(f, other);
          }
        };
    return transform_lambda(transformfn, 
                            output_type,
                            true /*skip undefined*/, 
                            0 /*random seed*/);
  } else {
    auto transformfn =  
        [=](const flexible_type& f)->flexible_type {
          return right_operator ? binaryfn(other, f) : binaryfn(f, other);
        };

    return transform_lambda(transformfn, 
                            output_type,
                            false/*skip undefined*/, 
                            0 /*random seed*/);
  }

  return ret_unity_sarray;
}


void unity_sarray::construct_from_unity_sarray(const unity_sarray& other) {
  m_planner_node = other.m_planner_node;
}

std::shared_ptr<unity_sarray_base> unity_sarray::left_scalar_operator(
    flexible_type other, std::string op) {
  log_func_entry();
  return scalar_operator(other, op, false);
}

std::shared_ptr<unity_sarray_base> unity_sarray::right_scalar_operator(
    flexible_type other, std::string op) {
  log_func_entry();
  return scalar_operator(other, op, true);
}

std::shared_ptr<unity_sarray_base> unity_sarray::vector_operator(
    std::shared_ptr<unity_sarray_base> other, std::string op) {
  log_func_entry();
  unity_sarray_binary_operations::check_operation_feasibility(dtype(), other->dtype(), op);

  flex_type_enum output_type =
      unity_sarray_binary_operations::get_output_type(dtype(), other->dtype(), op);
  // empty arrays all around. quick exit
  if (this->size() == 0 && other->size() == 0) {
    auto ret = std::make_shared<unity_sarray>();
    ret->construct_from_vector(std::vector<flexible_type>(), output_type);
    return ret;
  }
  // array mismatch
  if (this->size() != other->size()) {
    log_and_throw(std::string("Array size mismatch"));
  }

  // we are ready to perform the transform. Build the transform operation
  auto transformfn =
      unity_sarray_binary_operations::get_binary_operator(dtype(), other->dtype(), op);

  bool op_is_not_equality_compare = (op != "==" && op != "!=");
  bool op_is_equality = (op == "==");
  auto transform_fn_with_undefined_checking =
      [=](const sframe_rows::row& frow, 
          const sframe_rows::row& grow)->flexible_type {
        const auto& f = frow[0];
        const auto& g = grow[0];
        if (f.get_type() == flex_type_enum::UNDEFINED ||
            g.get_type() == flex_type_enum::UNDEFINED) {
          if (op_is_not_equality_compare) {
            // op is not == or !=
            return FLEX_UNDEFINED;
          } else if (op_is_equality) {
            // op is ==
            return f.get_type() == g.get_type();
          } else {
            // op is !=
            return f.get_type() != g.get_type();
          }
        }
        else return transformfn(f, g);
      };
  auto ret = std::make_shared<unity_sarray>();
  std::shared_ptr<unity_sarray> other_unity_sarray =
      std::static_pointer_cast<unity_sarray>(other);
  ret->construct_from_planner_node(
      op_binary_transform::make_planner_node(m_planner_node,
                                             other_unity_sarray->m_planner_node,
                                             transform_fn_with_undefined_checking,
                                             output_type));
  return ret;
}

std::shared_ptr<unity_sarray_base> unity_sarray::drop_missing_values() {
  log_func_entry();
  auto filterfn = [&](const flexible_type& f)->flexible_type {
    return !f.is_na();
  };

  auto filtered_out =  
      std::static_pointer_cast<unity_sarray>(
          transform_lambda(filterfn, 
                           flex_type_enum::INTEGER, 
                           false, 
                           0));
  return logical_filter(filtered_out);
}

std::shared_ptr<unity_sarray_base>
unity_sarray::fill_missing_values(flexible_type default_value) {
  log_func_entry();

  if(!flex_type_is_convertible(default_value.get_type(), this->dtype())) {
    log_and_throw("Default value must be convertible to column type");
  }

  auto transform_fn = [default_value](const flexible_type &f)->flexible_type {
    if(f.is_na()) {
      return default_value;
    }
    return f;
  };

  return transform_lambda(transform_fn, this->dtype(), false, 0);
}

std::shared_ptr<unity_sarray_base> unity_sarray::tail(size_t nrows) {
  log_func_entry();
  size_t maxrows = std::min<size_t>(size(), nrows);
  size_t end = size();
  size_t start = end - maxrows;
  return copy_range(start, 1, end);
}


std::shared_ptr<unity_sarray_base> unity_sarray::make_uniform_boolean_array(size_t size,
                                                                            float percent,
                                                                            int random_seed) {
  // create a sequential sarray
  auto seq = std::static_pointer_cast<unity_sarray>(
    unity_sarray::create_sequential_sarray(size, 0, false));

  flex_int seed_hash = flexible_type((flex_int)(random_seed)).hash();
  size_t sample_limit = std::numeric_limits<size_t>::max() * percent;
  auto filter_fn = [sample_limit, seed_hash](const flexible_type& val)->flexible_type {
        size_t d = hash64(val.get<flex_int>() ^ seed_hash);
        return d <= sample_limit;
      };
  return seq->transform_lambda(filter_fn, flex_type_enum::INTEGER, false, 0);
}

std::shared_ptr<unity_sarray_base> unity_sarray::sample(float percent, 
                                                        int random_seed) {
  // create a sequential sarray
  auto seq = make_uniform_boolean_array(size(), percent, random_seed);
  return logical_filter(seq);
}

std::shared_ptr<unity_sarray_base>
unity_sarray::count_bag_of_words(std::map<std::string, flexible_type> options) {
  log_func_entry();
  if (this->dtype() != flex_type_enum::STRING) {
    log_and_throw("Only string type is supported for word counting.");
  }

  bool to_lower = true;
  if (options.find("to_lower") != options.end()) {
    to_lower = options["to_lower"];
  }

  graphlab::flex_list delimiter_list;
  if (options.find("delimiters") != options.end()) {
    delimiter_list = options["delimiters"];
  }

  std::set<char> delimiters;
  for (auto it = delimiter_list.begin(); it != delimiter_list.end(); ++it) {
    // iterate through flexible_types storing the delimiters
    // cast each to a string and take first char in string
    // insert into std::set for quicker look-ups than graphlab::flex_list
    delimiters.insert(it->to<std::string>().at(0));
  }

  auto is_delimiter = [delimiters](const char c)->bool {
    return delimiters.find(c) != delimiters.end();
  };

  auto transformfn = [to_lower, is_delimiter](const flexible_type& f)->flexible_type {
    flex_dict ret;
    const std::string& str = f.get<flex_string>();

    // Tokenizing the string by space, and add to mape
    // Here we optimize for speed to reduce the string malloc
    size_t word_begin = 0;
    // skip leading delimiters
    while (word_begin < str.size() && (is_delimiter(str[word_begin])))
      ++word_begin;

    std::string word;
    flexible_type word_flex;

    // count bag of words
    std::unordered_map<flexible_type, size_t> ret_count;

    for (size_t i = word_begin; i < str.size(); ++i) {
      if (is_delimiter(str[i])) {
        // find the end of thw word, make a substring, and transform to lower case
        word = std::string(str, word_begin, i - word_begin);
        if  (to_lower)
          std::transform(word.begin(), word.end(), word.begin(), ::tolower);
        word_flex = std::move(word);

        // add the word to map
        ret_count[word_flex]++;

        // keep skipping delimiters, and reset word_begin
        while (i < str.size() && (is_delimiter(str[i])))
          ++i;
        word_begin = i;
      }
    }

    // add the last word
    if (word_begin < str.size()) {
      word = std::string(str, word_begin, str.size() - word_begin);
      if  (to_lower)
        std::transform(word.begin(), word.end(), word.begin(), ::tolower);
      word_flex = std::move(word);
      ret_count[word_flex]++;
    }

    // convert to dictionary
    for(auto& val : ret_count) {
      ret.push_back({val.first, flexible_type(val.second)});
    }
    return ret;
  };

  return transform_lambda(transformfn, flex_type_enum::DICT, true, 0);
}


std::shared_ptr<unity_sarray_base>
unity_sarray::count_ngrams(size_t n, std::map<std::string, flexible_type> options) {

  log_func_entry();
  if (this->dtype() != flex_type_enum::STRING) {
    log_and_throw("Only string type is supported for n-gram counting.");
  }

  bool to_lower = true;
  if (options.find("to_lower") != options.end()) {
    to_lower = options["to_lower"];
  }


  auto transformfn = [to_lower, n](const flexible_type& f)->flexible_type {
    if (f.get_type() == flex_type_enum::UNDEFINED) return f;

    //Maps that hold id's, n-grams, and counts.
    typedef std::pair<std::deque<size_t>, std::deque<size_t> > deque_pair;

    std::unordered_map<hash_value, deque_pair > ngram_id_map;
    std::unordered_map<hash_value, size_t > id_count_map;

    std::string lower;

    //Do a string copy if need to convert to lowercase

    if (to_lower){
      lower = boost::algorithm::to_lower_copy(f.get<flex_string>());
    }

    const std::string& str =  (to_lower) ? lower : f.get<flex_string>();



    // Tokenizing the string by space, and add to map
    // Here we optimize for speed to reduce the string malloc
    size_t word_begin = 0;
    size_t word_end = 0;

    flex_dict ret;
    std::deque<size_t> begin_deque;
    std::deque<size_t> end_deque;
    hash_value ngram_id;
    bool end_of_doc = false;

    while (1){

      // Getting the next word until we have n of them
      while (begin_deque.size() < n){
      // skip leading spaces
        while (word_begin < str.size()
          && (std::ispunct(str[word_begin])
            || std::isspace(str[word_begin])))
          ++word_begin;
      //If you reach the end, break out of loops.
        if (word_begin >= str.size()) {
          end_of_doc = true;
          break;
        }
      // Find end of word
        word_end = word_begin;
        while (!std::ispunct(str[word_end])
          && !std::isspace(str[word_end])
          && word_end < str.size())
          ++word_end;
      //Add to n-gram deque
        begin_deque.push_back(word_begin);
        end_deque.push_back(word_end);
        word_begin = word_end + 1;
      }

      // If not end of doc, add n-grams to maps
      if (end_of_doc){
        break;
      }else{

        //Combines hashes of all the words in n-gram in order dependent way,
        //producing a new hash.
        ngram_id = 0;

        assert(begin_deque.size() == n);
        assert(end_deque.size() == n);

        for(size_t i = 0; i < n; ++i){
          size_t word_length = end_deque[i]- begin_deque[i];
          uint128_t ngram_hash = hash128(&str[begin_deque[i]],word_length);
          ngram_id = hash128_combine(ngram_id.hash(), ngram_hash);
        }

        // Add deques to a map. These point to an instance of the n-gram in the
        // document string, to avoid unnecessary copies.
        if (ngram_id_map.count(ngram_id) == 0){
          ngram_id_map[ngram_id] = std::make_pair(begin_deque,end_deque);
        }

        id_count_map[ngram_id]++;


        // Slide along 1 word.
        begin_deque.pop_front();
        end_deque.pop_front();

      }
    }



  //Convert to dictionary

    std::string to_copy;

    for(auto& val : id_count_map) {
      size_t word_length;
      std::deque<size_t> ngram_begin_deque = ngram_id_map[val.first].first;
      std::deque<size_t> ngram_end_deque = ngram_id_map[val.first].second;
      to_copy.resize (0 , ' ');

      for (size_t i = 0; i <  n - 1; ++i){
        word_length = ngram_end_deque[i]- ngram_begin_deque[i];
        to_copy.append(&str[ngram_begin_deque[i]], word_length);
        to_copy.append( " ", 1);
      }

      word_length = ngram_end_deque[n -1]- ngram_begin_deque[n-1];
      to_copy.append(&str[ngram_begin_deque[n-1]], word_length);

      ret.push_back({move(to_copy), flexible_type(val.second)});
    }
    return ret;
  };

  return transform_lambda(transformfn, flex_type_enum::DICT, true, 0);
}

std::shared_ptr<unity_sarray_base>
unity_sarray::count_character_ngrams(size_t n, std::map<std::string, flexible_type> options) {
  log_func_entry();

  if (this->dtype() != flex_type_enum::STRING) {
    log_and_throw("Only string type is supported for word counting.");
  }

  bool to_lower = true;
  bool ignore_space = true;
  if (options.find("to_lower") != options.end()) {
    to_lower = options["to_lower"];
  }

  if (options.find("ignore_space") != options.end()) {
    ignore_space = options["ignore_space"];
  }


  auto transformfn = [to_lower, ignore_space, n](const flexible_type& f)->flexible_type {
    if (f.get_type() == flex_type_enum::UNDEFINED) return f;

    //Maps that hold id's, n-grams, and counts.

    typedef std::pair<std::deque<size_t>, size_t > deque_count_pair;

    std::unordered_map<hash_value, deque_count_pair > ngram_id_map;


    std::string lower;

    //Do a string copy if need to convert to lowercase

    if (to_lower){
      lower = boost::algorithm::to_lower_copy(f.get<flex_string>());
    }

    const std::string& str =  (to_lower) ? lower : f.get<flex_string>();



    // Tokenizing the string by space, and add to map
    // Here we optimize for speed to reduce the string malloc
    size_t character_location = 0;

    flex_dict ret;
    std::deque<size_t> character_deque;
    hash_value ngram_id;
    bool end_of_doc = false;

    while (1){

      // Getting the next word until we have n of them
      while (character_deque.size() < n){
      // skip leading spaces
        while (character_location < str.size()
          && (std::ispunct(str[character_location])
          || (std::isspace(str[character_location]) && ignore_space)))
          ++character_location;
      //If you reach the end, break out of loops.
        if (character_location >= str.size()) {
          end_of_doc = true;
          break;
        }
      //Add to n-gram deque
        character_deque.push_back(character_location);
        ++character_location;
      }

    // If not end of doc, add n-grams to maps
      if (end_of_doc){
        break;
      }else{
    //Combines hashes of all the words in n-gram in order dependent way,
    //producing a new hash.

        ngram_id = 0;

        assert(character_deque.size() == n);

        for(size_t i = 0; i < n; ++i){
          uint128_t ngram_hash = hash128(&str[character_deque[i]],1);
          ngram_id = hash128_combine(ngram_id.hash(),ngram_hash);
        }

    // Add deques to a map. These point to an instance of the n-gram
    //in the document string, to avoid unnecessary copies.
        if (ngram_id_map.count(ngram_id) == 0){
          ngram_id_map[ngram_id] = std::make_pair(character_deque,1);
        } else {
          ngram_id_map[ngram_id].second++;
        }

    // Slide along 1 character.
        character_deque.pop_front();

      }
    }

  //Convert to dictionary

    std::string to_copy;

    for(auto& val : ngram_id_map) {
      std::deque<size_t> ngram_character_deque = val.second.first;
      to_copy.resize (0 , ' ');

      for (size_t i = 0; i <  n; ++i){
        to_copy.append(&str[ngram_character_deque[i]], 1);
      }

      ret.push_back({move(to_copy), flexible_type(val.second.second)});
    }
    return ret;
  };

  return transform_lambda(transformfn, flex_type_enum::DICT, true, 0);
}

std::shared_ptr<unity_sarray_base>
unity_sarray::dict_trim_by_keys(const std::vector<flexible_type>& keys, bool exclude) {
  log_func_entry();

  if (this->dtype() != flex_type_enum::DICT) {
    log_and_throw("Only dictionary type is supported for trim by keys.");
  }

  std::set<flexible_type> keyset(keys.begin(), keys.end());

  auto transformfn = [exclude, keyset](const flexible_type& f)->flexible_type {
    if (f.get_type() == flex_type_enum::UNDEFINED) return f;

    flex_dict ret;
    const flex_dict& input = f.get<flex_dict>();
    for(auto& val : input) {
      bool is_in_key = val.first.get_type() == flex_type_enum::UNDEFINED ? false : keyset.count(val.first);
      if (exclude != is_in_key) {
        ret.push_back(std::make_pair<flexible_type, flexible_type>(
          flexible_type(val.first), flexible_type(val.second)));
      }
    }

    return ret;
  };

  return transform_lambda(transformfn, flex_type_enum::DICT, true, 0);
}

std::shared_ptr<unity_sarray_base>
unity_sarray::dict_trim_by_values(const flexible_type& lower, const flexible_type& upper) {
  log_func_entry();

  if (this->dtype() != flex_type_enum::DICT) {
    log_and_throw("Only dictionary type is supported for trim by keys.");
  }

  bool has_lower_bound = lower.get_type() != flex_type_enum::UNDEFINED;
  bool has_upper_bound = upper.get_type() != flex_type_enum::UNDEFINED;

  // invalid parameter
  if (has_upper_bound && has_lower_bound && lower > upper) {
    log_and_throw("Low bound must be higher than upper bound.");
  }

  // nothing to trim
  if (!has_upper_bound && !has_lower_bound) {
    return std::static_pointer_cast<unity_sarray>(shared_from_this());
  }

  auto transformfn = [=](const flexible_type& f)->flexible_type {
    if (f.get_type() == flex_type_enum::UNDEFINED) return f;

    flex_dict ret;
    const flex_dict& input = f.get<flex_dict>();
    for(auto& val : input) {
      bool lower_bound_match =
        !has_lower_bound ||
        !flex_type_has_binary_op(val.second.get_type(), lower.get_type(), '<') ||
        val.second >= lower;

      bool upper_bound_match =
        !has_upper_bound ||
        !flex_type_has_binary_op(val.second.get_type(), upper.get_type(), '<') ||
        val.second <= upper;

      if (lower_bound_match && upper_bound_match) {
        ret.push_back(std::make_pair<flexible_type, flexible_type>(
          flexible_type(val.first), flexible_type(val.second)));
      }
    }

    return ret;
  };

  return transform_lambda(transformfn, flex_type_enum::DICT, true, 0);
}

std::shared_ptr<unity_sarray_base> unity_sarray::dict_keys() {
  log_func_entry();
  if (this->dtype() != flex_type_enum::DICT) {
    log_and_throw("Only dictionary type is supported for trim by keys.");
  }

  auto transformfn = [](const flexible_type& f)->flexible_type {
    if (f.get_type() == flex_type_enum::UNDEFINED) return f;
    return flex_dict_view(f).keys();
  };
  return transform_lambda(transformfn, flex_type_enum::LIST, true, 0);
}

std::shared_ptr<unity_sarray_base> unity_sarray::dict_values() {
  log_func_entry();

  if (this->dtype() != flex_type_enum::DICT) {
    log_and_throw("Only dictionary type is supported for trim by keys.");
  }

 auto transformfn = [](const flexible_type& f)->flexible_type {
    if (f.get_type() == flex_type_enum::UNDEFINED) return f;
    return flex_dict_view(f).values();
  };

  return transform_lambda(transformfn, flex_type_enum::LIST, true, 0);
}

std::shared_ptr<unity_sarray_base>
unity_sarray::dict_has_any_keys(const std::vector<flexible_type>& keys) {
  log_func_entry();

  if (this->dtype() != flex_type_enum::DICT) {
    log_and_throw("Only dictionary type is supported for trim by keys.");
  }

  std::set<flexible_type> keyset(keys.begin(), keys.end());

  auto transformfn = [keyset](const flexible_type& f)->int {
    if (f.get_type() == flex_type_enum::UNDEFINED) return f;

    for(auto& val : f.get<flex_dict>()) {
      bool is_in_key = val.first.get_type() == flex_type_enum::UNDEFINED ? false : keyset.count(val.first);
      if (is_in_key) return 1;
    }

    return 0;
  };

  return transform_lambda(transformfn, flex_type_enum::INTEGER, true, 0);
}

std::shared_ptr<unity_sarray_base>
unity_sarray::dict_has_all_keys(const std::vector<flexible_type>& keys) {
  log_func_entry();
  if (this->dtype() != flex_type_enum::DICT) {
    log_and_throw("Only dictionary type is supported for trim by keys.");
  }

  auto transformfn = [keys](const flexible_type& f)->int {
    if (f.get_type() == flex_type_enum::UNDEFINED) return f;

    // make sure each key exists in the dictionary
    const flex_dict_view& v(f);
    for(auto& key: keys) {
      if (!v.has_key(key)) return 0;
    }

    return 1;
  };

  return transform_lambda(transformfn, flex_type_enum::INTEGER, true, 0);
}


std::shared_ptr<unity_sarray_base> unity_sarray::item_length() {
  log_func_entry();

  std::set<flex_type_enum> supported_types(
    {flex_type_enum::DICT, flex_type_enum::VECTOR, flex_type_enum::LIST});

  if (!supported_types.count(this->dtype())) {
    log_and_throw("item_length() is only applicable for SArray of type list, dict and array.");
  }

  auto transformfn = [](const flexible_type& f)->flexible_type {
    if (f.get_type() == flex_type_enum::UNDEFINED) return f;

    return f.size();
  };

  return transform_lambda(transformfn, flex_type_enum::INTEGER, true, 0);
}

std::shared_ptr<unity_sframe_base >unity_sarray::unpack_dict(
  const std::string& column_name_prefix,
  const std::vector<flexible_type>& limit,
  const flexible_type& na_value) {
  log_func_entry();

  if (this->dtype() != flex_type_enum::DICT) {
    throw "unpack_dict is only applicable to SArray of dictionary type.";
  }
  // we need 2 passes here. Lets make sure we are materialized.
  materialize();

  bool has_key_limits = limit.size() > 0;

  std::map<flexible_type, flex_type_enum> key_valuetype_map;

  // prefill the key_value_type map if limit is given
  if (has_key_limits) {
    for(auto v : limit) {
      key_valuetype_map[v] = flex_type_enum::UNDEFINED;
    }
  }

  // Logic to converge two types:
  // 1. if one of the type is UNDEFINED, use the other
  // 2. if one of the type is float and the other is int, use float
  // 3. if one can be converted to another, use the more generic one
  // 4. if none of the above, use STRING
  auto type_combine_fn = [](flex_type_enum t1, flex_type_enum t2)->flex_type_enum {
    if (t1 == flex_type_enum::UNDEFINED) {
      return t2;
    } else if (t2 == flex_type_enum::UNDEFINED) {
      return t1;
    } else if ((t1 == flex_type_enum::INTEGER && t2 == flex_type_enum::FLOAT) ||
               (t2 == flex_type_enum::INTEGER && t1 == flex_type_enum::FLOAT)) {
      return flex_type_enum::FLOAT;
    } else {
      if (flex_type_is_convertible(t1, t2)) {
        return t2;
      } else if (flex_type_is_convertible(t2, t1)) {
        return t1;
      } else {
        return flex_type_enum::STRING;
      }
    }
  };

  // extract dictionary keys and value types from all rows
  auto reductionfn = [has_key_limits, type_combine_fn](
      const flexible_type& f,
      std::map<flexible_type, flex_type_enum>& key_valuetype_map)->bool {

    if (f != FLEX_UNDEFINED) {
      const flex_dict& input = f.get<flex_dict>();
      for(auto& v : input) {
        flex_type_enum new_type = v.second.get_type();
        auto position = key_valuetype_map.find(v.first);

        // new key
        if (position == key_valuetype_map.end()) {

          // skip the key if limited
          if (has_key_limits) continue;

          key_valuetype_map[v.first] = new_type;

        } else {
          // do appropriate type conversion
          key_valuetype_map[v.first] = type_combine_fn(new_type, position->second);
        }
      }
    }
    return true;
  };

  auto combinefn = [type_combine_fn]
          (std::map<flexible_type, flex_type_enum>& mapping,
           std::map<flexible_type, flex_type_enum>& aggregate)->bool {

    for(auto& val : mapping) {
      auto position = aggregate.find(val.first);
      if (position == aggregate.end()) {
        aggregate[val.first] = val.second;
      } else {
        position->second = type_combine_fn(position->second, val.second);
      }
    }
    return true;
  };

  key_valuetype_map =
    query_eval::reduce<std::map<flexible_type, flex_type_enum>>
    (m_planner_node, reductionfn, combinefn, key_valuetype_map);

  if (key_valuetype_map.size() == 0) {
    throw "Nothing to unpack, SArray is empty";
  }

  // generate column types
  std::vector<flex_type_enum> column_types(key_valuetype_map.size());
  std::vector<flexible_type> unpacked_keys(key_valuetype_map.size());
  size_t i = 0;
  for(auto val : key_valuetype_map) {
    unpacked_keys[i] = val.first;
    column_types[i] = val.second == flex_type_enum::UNDEFINED ? flex_type_enum::FLOAT : val.second;
    i++;
  }

  return unpack(column_name_prefix, unpacked_keys, column_types, na_value);
}



std::shared_ptr<unity_sframe_base> unity_sarray::expand(
  const std::string& column_name_prefix,
  const std::vector<flexible_type>& expanded_column_elements,
  const std::vector<flex_type_enum>& expanded_column_types) {
  log_func_entry();

  auto mytype = this->dtype();
  if (mytype != flex_type_enum::DATETIME) {
    throw("Cannot expand an SArray of type that is not datetime type");
  }

  if (expanded_column_elements.size() != expanded_column_types.size()) {
    throw "Expanded column names and types length do not match";
  }

  if (expanded_column_elements.size() == 0) {
    throw "Please provide at least one column to expand datetime to";
  }

  // generate column names
  std::vector<std::string> column_names;
  column_names.reserve(expanded_column_elements.size());
  for (auto& key : expanded_column_elements) {
    if (column_name_prefix.empty()) {
      column_names.push_back((flex_string)key);
    } else {
      column_names.push_back(column_name_prefix + "." + (flex_string)key);
    }
  }
  enum class date_element_type {
    YEAR, MONTH, DAY, HOUR, MINUTE, SECOND, WEEKDAY, ISOWEEKDAY, TMWEEKDAY, US, TZONE
  };
  std::vector<date_element_type> date_elements(expanded_column_elements.size());
  for (size_t i = 0; i < expanded_column_elements.size(); ++i) {
    auto& cur_element = expanded_column_elements[i];
    if(cur_element == "year") date_elements[i] = date_element_type::YEAR;
    else if(cur_element == "month") date_elements[i] = date_element_type::MONTH;
    else if(cur_element == "day") date_elements[i] = date_element_type::DAY;
    else if(cur_element == "hour") date_elements[i] = date_element_type::HOUR;
    else if(cur_element == "minute") date_elements[i] = date_element_type::MINUTE;
    else if(cur_element == "second") date_elements[i] = date_element_type::SECOND;
    else if(cur_element == "weekday") date_elements[i] = date_element_type::WEEKDAY;
    else if(cur_element == "isoweekday") date_elements[i] = date_element_type::ISOWEEKDAY;
    else if(cur_element == "tmweekday") date_elements[i] = date_element_type::TMWEEKDAY;
    else if(cur_element == "us") date_elements[i] = date_element_type::US;
    else if(cur_element == "tzone") date_elements[i] = date_element_type::TZONE;
  }

  // TODO: Performance, instead of performing a string comparison everywhere,
  // we should use a enum comparison.
  auto transformfn = [date_elements]
      (const sframe_rows::row& row, sframe_rows::row& ret) {
        DASSERT_EQ(ret.size(), date_elements.size());

        if (row[0].get_type() == flex_type_enum::UNDEFINED) {
          for(size_t i = 0; i < ret.size(); i++) ret[i] = flex_undefined();
        } else {
          for(size_t i = 0; i < date_elements.size() ; i++) {
            const flex_date_time & dt = row[0].get<flex_date_time>();
            boost::posix_time::ptime ptime_val = 
                flexible_type_impl::ptime_from_time_t(dt.shifted_posix_timestamp());
            tm _tm = boost::posix_time::to_tm(ptime_val);
            switch(date_elements[i]) {
              case date_element_type::YEAR:
                ret[i] = _tm.tm_year + 1900;
                break;
              case date_element_type::MONTH:
                // +1 is for adjustment with datetime object in python,
                // tm is from 0-11 while datetime is from 1-12
                ret[i] = _tm.tm_mon + 1;
                break;
              case date_element_type::DAY:
                ret[i] = _tm.tm_mday;
                break;
              case date_element_type::HOUR:
                ret[i] = _tm.tm_hour;
                break;
              case date_element_type::MINUTE:
                ret[i] = _tm.tm_min;
                break;
              case date_element_type::SECOND:
                ret[i] = _tm.tm_sec;
                break;
              case date_element_type::WEEKDAY:
                // tm day has Sunday = 0, Sat = 6
                // Python weekday has Monday = 0, Sun = 6
                // so rotate by 1
                ret[i] = (_tm.tm_wday + 6) % 7;
                break;
              case date_element_type::ISOWEEKDAY:
                // tm day has Sunday = 0, Monday = 1, Sat = 6
                // Python isoweekday has Monday = 1, Sun = 7
                ret[i] = ((_tm.tm_wday + 6) % 7) + 1;
                break;
              case date_element_type::TMWEEKDAY:
                ret[i] = _tm.tm_wday;
                break;
              case date_element_type::US:
                ret[i] = dt.microsecond();
                break;
              case date_element_type::TZONE:
                ret[i] = dt.time_zone_offset() * flex_date_time::TIMEZONE_RESOLUTION_IN_HOURS;
                break;
              default:
                __builtin_unreachable();
            }
          }
        }
      };

  auto ret_node = 
      op_generalized_transform::make_planner_node(m_planner_node, 
                                                  transformfn,
                                                  expanded_column_types);

  std::shared_ptr<unity_sframe> ret (new unity_sframe());
  ret->construct_from_planner_node(ret_node, column_names);
  return ret;
}


std::shared_ptr<unity_sframe_base> unity_sarray::unpack(
  const std::string& column_name_prefix,
  const std::vector<flexible_type>& unpacked_keys,
  const std::vector<flex_type_enum>& column_types,
  const flexible_type& na_value) {
  log_func_entry();

  auto mytype = this->dtype();
  if (mytype != flex_type_enum::DICT &&
    mytype != flex_type_enum::LIST &&
    mytype != flex_type_enum::VECTOR) {
    throw("Cannot unpack an SArray of type that is not list/array/dict type");
  }

  if (unpacked_keys.size() != column_types.size()) {
    throw "unpacked column names and types length do not match";
  }

  if (unpacked_keys.size() == 0) {
    throw "Please provide at least one column to unpack to";
  }

  // generate column names
  std::vector<std::string> column_names;
  column_names.reserve(unpacked_keys.size());
  for (auto& key : unpacked_keys) {
    if (column_name_prefix.empty()) {
      column_names.push_back((flex_string)key);
    } else {
      column_names.push_back(column_name_prefix + "." + (flex_string)key);
    }
  }
  auto coltype = dtype();
  auto transformfn = [coltype, unpacked_keys, na_value](const sframe_rows::row& row,
                                                        sframe_rows::row& ret) {
    const auto& val = row[0];
    if (val.get_type() == flex_type_enum::UNDEFINED) {
      for(size_t i = 0; i < ret.size() ; i++) ret[i] = FLEX_UNDEFINED;
    } else {
      if (coltype == flex_type_enum::DICT) {
        const flex_dict_view& dict_val(val);
        for(size_t i = 0; i < ret.size() ; i++) {
          if (dict_val.has_key(unpacked_keys[i]) && dict_val[unpacked_keys[i]] != na_value) {
            ret[i] = dict_val[unpacked_keys[i]];
          } else {
            ret[i] = FLEX_UNDEFINED;
          }
        }
      } else if(coltype == flex_type_enum::LIST) {
        for(size_t i = 0; i < ret.size(); i++) {
          size_t index = unpacked_keys[i].get<flex_int>();
          if (val.size() <= index || val.array_at(index) == na_value) {
            ret[i] = FLEX_UNDEFINED;
          } else {
            ret[i] = val.array_at(index);
          }
        }
      } else {
        DASSERT_MSG(coltype == flex_type_enum::VECTOR, "coltype for unpack is not expected!");
        for(size_t i = 0; i < ret.size(); i++) {
          size_t index = unpacked_keys[i].get<flex_int>();
          if (val.size() <= index || val[index] == na_value || std::isnan(val[index])) {
            ret[i] = FLEX_UNDEFINED;
          } else {
            ret[i] = val[index];
          }
        }
      }
    }
  };

  auto ret_node = op_generalized_transform::make_planner_node(m_planner_node, 
                                                              transformfn,
                                                              column_types);

  std::shared_ptr<unity_sframe> ret (new unity_sframe());
  ret->construct_from_planner_node(ret_node, column_names);
  // do some validation by calling head on it
  ret->head(100);
  return ret;
}

void unity_sarray::begin_iterator() {
  Dlog_func_entry();
  auto sarray_ptr = get_underlying_sarray();

  // nothing to iterate over. quit
  if (!sarray_ptr || size() == 0) return;
  iterator_sarray_ptr = sarray_ptr->get_reader();
  // init the iterators
  iterator_current_segment_iter.reset(new sarray_iterator<flexible_type>(iterator_sarray_ptr->begin(0)));
  iterator_current_segment_enditer.reset(new sarray_iterator<flexible_type>(iterator_sarray_ptr->end(0)));
  iterator_next_segment_id = 1;
}


std::vector<flexible_type> unity_sarray::iterator_get_next(size_t len) {
  Dlog_func_entry();
  std::vector<flexible_type> ret;
  // nothing to iterate over. quit
  if (!iterator_sarray_ptr || size() == 0) return ret;
  // try to extract len elements
  ret.reserve(len);
  // loop across segments
  while(1) {
    // loop through current segment
    while(*iterator_current_segment_iter != *iterator_current_segment_enditer) {
      ret.push_back(**iterator_current_segment_iter);
      ++(*iterator_current_segment_iter);
      if (ret.size() >= len) break;
    }
    if (ret.size() >= len) break;
    // if we run out of data in the current segment, advance to the next segment
    // if we run out of segments, quit.
    if (iterator_next_segment_id >= iterator_sarray_ptr->num_segments()) break;
    iterator_current_segment_iter.reset(new sarray_iterator<flexible_type>(
        iterator_sarray_ptr->begin(iterator_next_segment_id)));
    iterator_current_segment_enditer.reset(new sarray_iterator<flexible_type>(
        iterator_sarray_ptr->end(iterator_next_segment_id)));
    ++iterator_next_segment_id;
  }

  return ret;
}


void unity_sarray::materialize() {
  get_underlying_sarray();
}

bool unity_sarray::is_materialized() {
  auto optimized_node = optimization_engine::optimize_planner_graph(get_planner_node(),
                                                                    materialize_options());
  if (is_source_node(optimized_node)) {
    m_planner_node = optimized_node;
    return true;
  }
  return false;
}


size_t unity_sarray::get_content_identifier() {
  if (is_materialized()) {
    index_file_information index_info = get_underlying_sarray()->get_index_info();
    // compute a hash which uniquely identifies the sarray.
    // This can be done by computing a hash of all the segment file names
    // and the segment sizes. (really, just the segment file names are
    // probably sufficient), but I have a bit of paranoia since it is technically
    // possible to interpret a longer sarray as a shorter one by changing the
    // perceived segment size.
    size_t hash_val = graphlab::hash64(index_info.segment_files);
    for (auto& segment_size: index_info.segment_sizes) {
      hash_val = graphlab::hash64_combine(hash_val, graphlab::hash64(segment_size));
    }
    return hash_val;
  } else {
    return random::rand();
  }
}

std::shared_ptr<unity_sarray_base>
unity_sarray::copy_range(size_t start, size_t step, size_t end) {
  Dlog_func_entry();
  if (step == 0) log_and_throw("Range step size must be at least 1");
  // end cannot be past the end
  end = std::min(end, size());
  if (end <= start) {
    // return an empty array of the appropriate type
    std::shared_ptr<unity_sarray> ret(new unity_sarray);
    ret->construct_from_vector(std::vector<flexible_type>(), dtype());
    return ret;
  }

  // construct output
  auto out_sarray = std::make_shared<sarray<flexible_type>>();
  out_sarray->open_for_write();
  out_sarray->set_type(dtype());

  // copy me into out
  auto sarray_ptr = get_underlying_sarray();
  graphlab::copy_range(*sarray_ptr, *out_sarray, start, step, end);
  out_sarray->close();

  std::shared_ptr<unity_sarray> ret(new unity_sarray);
  ret->construct_from_sarray(out_sarray);
  return ret;
}

std::shared_ptr<unity_sarray_base> unity_sarray::
create_sequential_sarray(ssize_t size, ssize_t start, bool reverse) {
    if(size < 0) {
      log_and_throw("Must give size as >= 0");
    }

    auto row_num_sarray = std::make_shared<sarray<flexible_type>>();
    row_num_sarray->open_for_write(1);
    row_num_sarray->set_type(flex_type_enum::INTEGER);

    auto out_iter = row_num_sarray->get_output_iterator(0);
    for(ssize_t i = 0; i < size; ++i) {
      if(reverse) {
        *out_iter = start-i;
      } else {
        *out_iter = start+i;
      }
      ++out_iter;
    }

    row_num_sarray->close();

    std::shared_ptr<unity_sarray> row_num_column(new unity_sarray());
    row_num_column->construct_from_sarray(row_num_sarray);

    return row_num_column;
}

std::shared_ptr<unity_sarray_base> unity_sarray::builtin_rolling_apply(
    const std::string &fn_name,
    ssize_t start,
    ssize_t end,
    size_t min_observations) {
  log_func_entry();
  std::shared_ptr<unity_sarray> ret(new unity_sarray());

  auto agg_op = get_builtin_group_aggregator(fn_name);

  auto sarray_ptr = get_underlying_sarray();
  auto windowed_array = graphlab::rolling_aggregate::rolling_apply(*sarray_ptr,
      agg_op,
      start,
      end,
      min_observations);
  ret->construct_from_sarray(windowed_array);
  return ret;
}

std::shared_ptr<unity_sarray_base> unity_sarray::builtin_cumulative_aggregate(
    const std::string& name) {
  log_func_entry();
  gl_sarray in(std::make_shared<unity_sarray>(*this));
  return in.builtin_cumulative_aggregate(name).get_proxy();
}


} // namespace graphlab
