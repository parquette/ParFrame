/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
/**
 * \file
 * CSV Parser as adapted from Pandas
 */
#include <vector>
#include <string>
#include <cstdlib>
#include <algorithm>
#include <boost/config/warning_disable.hpp>
#include <sframe/csv_line_tokenizer.hpp>
#include <flexible_type/string_escape.hpp>
#include <flexible_type/flexible_type_spirit_parser.hpp>

namespace graphlab {

csv_line_tokenizer::csv_line_tokenizer() {
  field_buffer.resize(1024);
}


bool csv_line_tokenizer::tokenize_line(const char* str, size_t len, 
                                       std::function<bool (std::string&, size_t)> fn) {
  return tokenize_line_impl((char*)str, len, 
                            [&](char* buf, size_t len)->bool {
                              if (len == 0) {
                                std::string tmp;
                                return fn(tmp, 0);
                              } else {
                                std::string tmp(buf, len);
                                return fn(tmp, len);
                              }
                            },
                            [&](const char** buf, const char* bufend)->bool{
                              // now. this is actually quite annoying.
                              // This means I hit a '[' or a '{'.
                              const char* prevbuf = (*buf);
                              parser->general_flexible_type_parse(buf, bufend - (*buf));
                              if ((*buf) != prevbuf) {
                                // take the parsed section and turn it into a string
                                std::string v = std::string(prevbuf, ((*buf) - prevbuf));
                                fn(v, v.length());
                                return true;
                              }
                              return false;
                            },
                            [](){}
                            );
}

bool csv_line_tokenizer::tokenize_line(const char* str, size_t len,
                                       std::vector<std::string>& output) {
  output.clear();
  return tokenize_line_impl((char*)str, len,
                     [&](char* buf, size_t len)->bool {
                       if (len == 0) {
                         output.push_back(std::string());
                         return true;
                       } else {
                         while(len > 0 && std::isspace(buf[len - 1])) len--;
                         bool is_quoted = false;
                         if ((*buf) == quote_char) {
                           ++buf;
                           --len;
                         }
                         if (len > 0 && buf[len - 1] == quote_char) --len;
                         output.emplace_back(buf, len);
                         if (is_quoted) {
                           unescape_string(output.back(), escape_char, 
                                           quote_char, double_quote);
                         }
                         return true;
                        }
                     },
                    [&](const char** buf, const char* bufend)->bool{
                      // now. this is actually quite annoying.
                      // This means I hit a '[' or a '{'.
                      const char* prevbuf = (*buf);
                      parser->general_flexible_type_parse(buf, bufend - (*buf));
                      if ((*buf) != prevbuf) {
                        // take the parsed section and turn it into a string
                        std::string v = std::string(prevbuf, ((*buf) - prevbuf));
                        output.push_back(std::move(v));
                        return true;
                      }
                      return false;
                    },
                     [](){}
                     );
}

size_t csv_line_tokenizer::tokenize_line(char* str, size_t len,
                                         std::vector<flexible_type>& output,
                                         bool permit_undefined,
                                         const std::vector<size_t>* output_order) {
  size_t ctr = 0;
  size_t num_outputs = output.size();
  if (output_order != nullptr) num_outputs = output_order->size();
  bool success = 
  tokenize_line_impl(str, len,
                     [&](char* buf, size_t len)->bool {
                       if (ctr >= num_outputs) {
                         // special handling for space delimiters
                         // If we exceeded the expected number of output columns
                         // but if the remaining characters are empty or 
                         // all whitespace, we do not fail. 
                         // But instead simply ignore.
                         if (delimiter_is_space_but_not_tab) {
                           if (len == 0) return true;
                           for (size_t i = 0;i < len; ++i) {
                             if (!std::isspace(buf[i])) return false;
                           }
                           return true;
                         }
                         return false;
                       }
                       // get the output column
                       size_t output_idx = ctr;
                       if (output_order != nullptr) output_idx = (*output_order)[ctr];
                       // no output required
                       if (output_idx == (size_t)(-1)) {
                         ++ctr;
                         return true;
                       }

                       flex_type_enum outtype = output[output_idx].get_type();
                       // some types we permit UNDEFINED values when the
                       // length is 0. Except string. which will become an
                       // empty string.
                       if (len == 0) {
                         if (permit_undefined && 
                             outtype != flex_type_enum::STRING) {
                           output[output_idx].reset(flex_type_enum::UNDEFINED);
                         } else if (permit_undefined && 
                             outtype == flex_type_enum::STRING &&
                             empty_string_in_na_values) {
                           output[output_idx].reset(flex_type_enum::UNDEFINED);
                         } else {
                           output[output_idx] = flexible_type(outtype);
                         }
                         ++ctr;
                         return true;
                       } else {
                         // drop starting white space
                         while (std::isspace(*buf) && len > 0) {
                           ++buf;
                           --len;
                         }
                         bool success = parse_as(&buf, len, output[output_idx], true);
                         if (success) ++ctr;
                         return success;
                       }
                     },
                     [&](const char** buf, const char* bufend)->bool {
                       if (ctr >= num_outputs) return false;
                       // get the output column
                       size_t output_idx = ctr;
                       if (output_order != nullptr) output_idx = (*output_order)[ctr];
                       if (output_idx == (size_t)(-1)) {
                         // no output required. Just parse the contents 
                         // and drop the result
                         const char* prevbuf = (*buf);
                         parser->general_flexible_type_parse(buf, bufend - (*buf));
                         if ((*buf) != prevbuf) {
                           ++ctr;
                           return true;
                         } else {
                           return false;
                         }
                       }

                       if (output[output_idx].get_type() == flex_type_enum::STRING) {
                         // now. this is actually quite annoying.
                         // This means I hit a '[' or a '{'.
                         const char* prevbuf = (*buf);
                         parser->general_flexible_type_parse(buf, bufend - (*buf));
                         if ((*buf) != prevbuf) {
                           // take the parsed section and turn it into a string
                           std::string str = std::string(prevbuf, ((*buf) - prevbuf));
                           output[output_idx] = std::move(str);
                           ++ctr;
                           return true;
                         }
                         return false;
                       }
                       // no recursive parse. so parse_as will not modify the buffer
                       bool success = parse_as((char**)buf, bufend - (*buf), 
                                               output[output_idx], false);
                       if (success) ++ctr;
                       return success;
                     },
                     [&](){
                       --ctr;
                     });

  if (!success) return 0;
  return ctr;
};


bool csv_line_tokenizer::parse_as(char** buf, size_t len, 
                                  flexible_type& out, bool recursive_parse) {
  bool parse_success;
  // we are trying to parse a non-string, but this actually looks like a string
  // to me.  it might be some other type wrapped inside quote characters
  if (recursive_parse && 
      out.get_type() != flex_type_enum::STRING && 
      out.get_type() != flex_type_enum::UNDEFINED && 
      (*buf)[0] == quote_char && (*buf)[len - 1] == quote_char) {
    flexible_type tmp(flex_type_enum::STRING);
    // unescape the string inplace
    // skip the quote characters
    char* end_of_buf = (*buf) + len;
    ++(*buf); len -= 2;
    size_t new_length = 
        unescape_string(*buf, len, escape_char, quote_char, double_quote);
    bool ret = parse_as(buf, new_length, out, false);
    (*buf) = end_of_buf; 
    return ret;
  }

  /*
   * This is somewhat irregular:
   *  *buf does not get modified if parsing fails
   *  *buf gets modified if parsing succeeds EXCEPT if the parse
   *   result is a string in which case buf will not be modified.
   *
   *   This allows the section after the switch (na_value handling) to 
   *   correctly handle the cases where:
   *    - If the the raw string matches an na_value
   *    - If the parsed string (with escapes removed) matches an na_value
   */
  switch(out.get_type()) {
   case flex_type_enum::INTEGER:
     std::tie(out, parse_success) = parser->int_parse((const char**)buf, len);
     break;
   case flex_type_enum::FLOAT:
     std::tie(out, parse_success) = parser->double_parse((const char**)buf, len);
     break;
   case flex_type_enum::VECTOR:
     std::tie(out, parse_success) = parser->vector_parse((const char**)buf, len);
     break;
   case flex_type_enum::STRING:
     // STRING
     // right trim of the buffer. The
     // whitespace management of the parser already
     // takes care of the left trim
     {
       bool is_quoted = false;
       while(len > 0 && std::isspace((*buf)[len - 1])) len--;
       if (len >= 2 && (*buf)[0] == quote_char && (*buf)[len - 1] == quote_char) {
         out.mutable_get<flex_string>() = std::string((*buf)+1, len-2);
         is_quoted = true;
       } else {
         out.mutable_get<flex_string>() = std::string(*buf, len);
       }
       parse_success = true;
       if (is_quoted) {
         unescape_string(out.mutable_get<flex_string>(), escape_char, 
                         quote_char, double_quote);
       }
       break;
     }
   case flex_type_enum::DICT:
     std::tie(out, parse_success) = parser->dict_parse((const char**)buf, len);
     break;
   case flex_type_enum::LIST:
     std::tie(out, parse_success) = parser->recursive_parse((const char**)buf, len);
     break;
   case flex_type_enum::UNDEFINED:
     {
       // remember the original values
       // may need them later. see the comment above the switch
       char* original_buf = *buf;
       size_t original_len = len;
       std::tie(out, parse_success) = parser->general_flexible_type_parse((const char**)buf, len);
       // can we recursively parse this if it is a string?
       if (recursive_parse && 
           parse_success && 
           out.get_type() == flex_type_enum::STRING) {
         // make the string a parse buffer
         const flex_string& s = out.get<flex_string>();
         const char* cbegin = s.c_str();
         const char* c = cbegin;
         size_t clen = s.length();
         // trim trailing whitespace if any. (the parser will take care of 
         // any whitespace before
         while(clen > 0 && std::isspace(c[clen - 1])) clen--;
         // try to reparse
         flexible_type out2(flex_type_enum::UNDEFINED);
         bool parse_success2;
         std::tie(out2, parse_success2) = parser->non_string_flexible_type_parse(&c, clen);
         // parse was successful and we consumed the entire buffer
         // that's the output then.
         if (parse_success2 && (c - cbegin) == (int)clen) {
           out = out2;
         }
       }
       // output is a string. restore the pointers
       if (out.get_type() == flex_type_enum::STRING) {
         // restore the buffer pointers
         (*buf) = original_buf;
         original_len = len;
       }
     }
     break;
   default:
     parse_success = false;
     return false;
  }

  if (!na_values.empty()) {
    // process missing values
    // first, whether the parsed buffer matches the na values *exactly*
    if ((parse_success == false && out.get_type() != flex_type_enum::STRING) || 
        (parse_success == true && out.get_type() == flex_type_enum::STRING)) {
      while(len > 0 && std::isspace((*buf)[len - 1])) len--;
      for (const auto& na_value: na_values) {
        if (na_value.length() == len && strncmp(*buf, na_value.c_str(), len) == 0) {
          out.reset(flex_type_enum::UNDEFINED);
          parse_success = true;
          break;
        }
      }
    }
    // if it is a string, if it matches the string that was parsed, it is also 
    // an na_value 
    if (parse_success == true && out.get_type() == flex_type_enum::STRING) {
      const char* c = out.get<flex_string>().c_str();
      size_t clen = out.get<flex_string>().length();
      for (const auto& na_value: na_values) {
        if (na_value.length() == clen && strncmp(c, na_value.c_str(), clen) == 0) {
          out.reset(flex_type_enum::UNDEFINED);
          parse_success = true;
          break;
        }
      }
    }
  }
  return parse_success;
}



// reset the contents of the field buffer
#define BEGIN_FIELD() field_buffer_len = 0;

// insert a character into the field buffer. resizing it if necessary
#define PUSH_CHAR(c) if (field_buffer_len >= field_buffer.size()) field_buffer.resize(field_buffer.size() * 2); \
                     field_buffer[field_buffer_len++] = c;  \
                     escape_sequence = (c == escape_char);

// Finished parsing a field buffer. insert the token and reset the buffer
#define END_FIELD() if (!add_token(&(field_buffer[0]), field_buffer_len)) { good = false; keep_parsing = false; break; } \
                    field_buffer_len = 0;

// Reached the end of the line. stop
#define END_LINE() keep_parsing = false; break;

// current character matches first character of delimiter
// and delimiter is either a single character, or we need to do a 
// more expensive test.
#define DELIMITER_TEST() (delimiter_is_not_empty && \
                          (*buf) == delimiter_first_character) &&     \
      (delimiter_is_singlechar ||     \
       test_is_delimiter(buf, bufend, delimiter_begin, delimiter_end))

static inline bool test_is_delimiter(const char* c, const char* end, 
                                const char* delimiter, const char* delimiter_end) {
  // if I have more delimiter characters than the length of the string
  // quit.
  if (delimiter_end - delimiter > end - c) return false; 
  while (delimiter != delimiter_end) {
    if ((*c) != (*delimiter)) return false;
    ++c; ++delimiter;
  }
  return true;
}

static inline bool is_space_but_not_tab(char c) {
  return c != '\t' && std::isspace(c);
}

template <typename Fn, typename Fn2, typename Fn3>
bool csv_line_tokenizer::tokenize_line_impl(char* str, 
                                            size_t len,
                                            Fn add_token,
                                            Fn2 lookahead,
                                            Fn3 canceltoken) {
  ASSERT_MSG(parser, "Uninitialized tokenizer.");
  const char* buf = &(str[0]);
  const char* bufend= buf + len;
  const char* delimiter_begin = delimiter.c_str();
  const char* delimiter_end = delimiter_begin + delimiter.length();
  bool good = true;
  bool keep_parsing = true;
  // we switched state to start_field by encountering a delimiter
  bool start_field_with_delimiter_encountered = false;
  // this is set to true for the character immediately after an escape character
  // and false all other times
  bool escape_sequence = false;
  tokenizer_state state = tokenizer_state::START_FIELD; 
  field_buffer_len = 0;
  if (delimiter_is_new_line) {
    add_token(str, len);
    return true;
  }

  // this is adaptive. It can be either " or ' as we encounter it

  while(keep_parsing && buf != bufend) {
    // Next character in file
    bool is_delimiter = DELIMITER_TEST();
    // since escape_sequence can only be true for one character after it is
    // set to true. I need a flag here. if reset_escape_sequence is true, the
    // at the end of the loop, I clear escape_sequence
    bool reset_escape_sequence = escape_sequence;
    // skip to the last character of the delimiter
    if (is_delimiter) buf += delimiter.length() - 1;

    char c = *buf++;
    switch(state) {

     case tokenizer_state::START_FIELD:
       /* expecting field */
       // clear the flag
       if (c == quote_char) {
         // start quoted field
         start_field_with_delimiter_encountered = false;
         if (preserve_quoting == false) {
           BEGIN_FIELD();
           PUSH_CHAR(c);
           state = tokenizer_state::IN_QUOTED_FIELD;
         } else {
           BEGIN_FIELD();
           PUSH_CHAR(c);
           state = tokenizer_state::IN_FIELD;
         }
       } else if (is_space_but_not_tab(c) && skip_initial_space) {
         // do nothing
       } else if (is_delimiter) {
         /* save empty field */
         start_field_with_delimiter_encountered = true;
         // advance buffer
         BEGIN_FIELD();
         END_FIELD();
         // otherwise if we are joining consecutive delimiters, do nothing
       } else if (has_comment_char && c == comment_char) {
         // comment line
         start_field_with_delimiter_encountered = false;
         END_LINE();
       } else if (c == '[' || c == '{') {
         const char* prev = buf;
         start_field_with_delimiter_encountered = false;
         buf--; // shift back so we are on top of the bracketing character
         if (lookahead(&buf, bufend)) {
           // ok we have successfully parsed a field.
           // drop whitespace
           while(buf < bufend && std::isspace(*buf)) ++buf;
           if (buf == bufend) { 
             continue;
           } else if (DELIMITER_TEST()) { 
             start_field_with_delimiter_encountered = true;
             // skip past the delimiter
             buf += delimiter.length();
             continue;
           } else if(delimiter_is_space_but_not_tab) {
             // the lookahead parser may absorb whitespace
             // so if the delimiter is a whitespace, we immediately
             // advance to the next field
             continue;
           } else {
             // bad. the lookahead picked up a whole field. But
             // we do not see a delimiter.
             // fail the lookahead
             canceltoken();
             buf = prev;
             goto REGULAR_CHARACTER;
           }
         } else {
           buf = prev;
           // interpret as a regular character
           goto REGULAR_CHARACTER;
         }
       } else {
REGULAR_CHARACTER:
         start_field_with_delimiter_encountered = false;
         /* begin new unquoted field */
         PUSH_CHAR(c);
         state = tokenizer_state::IN_FIELD;
       }
       break;

     case tokenizer_state::IN_FIELD:
       /* in unquoted field */
       if (is_delimiter) {
         // End of field. End of line not reached yet
         END_FIELD();
         // advance buffer
         start_field_with_delimiter_encountered = true;
         state = tokenizer_state::START_FIELD;
       } else if (has_comment_char && c == comment_char) {
         // terminate this field
         END_FIELD();
         state = tokenizer_state::START_FIELD;
         END_LINE();
       } else {
         /* normal character - save in field */
         PUSH_CHAR(c);
       }
       break;

     case tokenizer_state::IN_QUOTED_FIELD:
       /* in quoted field */
       if (c == quote_char && !escape_sequence) {
         if (double_quote) {
           /* doublequote; " represented by "" */
           // look ahead one character
           // we are committed to preserving the buffer *exactly* here
           // so push two quotes
           if (buf + 1 < bufend && *buf == quote_char) {
             PUSH_CHAR(c);
             PUSH_CHAR(c);
             ++buf;
             break;
           }
         }
         /* end of quote part of field */
         PUSH_CHAR(c);
         state = tokenizer_state::IN_FIELD;
       }
       else {
         /* normal character - save in field */
         PUSH_CHAR(c);
       }
       break;
    }
    if (reset_escape_sequence) escape_sequence = false;
  }
  if (!good) return false;
  // cleanup 
  if (state != tokenizer_state::START_FIELD) {
    if (!add_token(&(field_buffer[0]), field_buffer_len)) { 
      return false;
    }
  } else {
    if (start_field_with_delimiter_encountered) {
      if (!add_token(NULL, 0)) { 
        return false;
      }
    }
  }
  return true;
}

void csv_line_tokenizer::init() {
  parser.reset(new flexible_type_parser(delimiter, escape_char));
  is_regular_line_terminator = line_terminator == "\n";
  if (is_regular_line_terminator) {
    delimiter_is_new_line = delimiter == "\n" || 
                            delimiter == "\r" || 
                            delimiter == "\r\n";
  } else {
    delimiter_is_new_line = delimiter == line_terminator;
  }

  delimiter_is_not_empty = !delimiter.empty();
  delimiter_is_space_but_not_tab = delimiter_is_not_empty && 
                      std::all_of(delimiter.begin(),
                                   delimiter.end(),
                                   [](char c)->bool {
                                     return is_space_but_not_tab(c);
                                   });
  delimiter_first_character = delimiter[0];
  delimiter_is_singlechar = delimiter.length() == 1;
  empty_string_in_na_values = false;
  for (auto& na_val: na_values) {
    empty_string_in_na_values |= na_val.length() == 0;
  }
  
}

} // namespace graphlab
