/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2018 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#pragma once

#include <ctype.h>

#include "caf/detail/parser/ec.hpp"
#include "caf/detail/parser/fsm.hpp"
#include "caf/detail/parser/is_char.hpp"
#include "caf/detail/parser/read_atom.hpp"
#include "caf/detail/parser/read_number_or_timespan.hpp"
#include "caf/detail/parser/read_string.hpp"
#include "caf/detail/scope_guard.hpp"

namespace caf {
namespace detail {
namespace parser {

/// Reads an INI formateed input and produces a series of key/value pairs.
template <class Iterator, class Sentinel, class Consumer>
void read_ini(state<Iterator, Sentinel>& ps, Consumer& consumer) {
  using std::swap;
  std::string tmp;
  auto is_alnum_or_dash = [](char x) {
    return isalnum(x) || x == '-' || x == '_';
  };
  bool in_section = false;
  auto begin_section = [&] {
    if (in_section)
      consumer.end_section();
    else
      in_section = true;
    std::string section_key;
    swap(tmp, section_key);
    consumer.begin_section(std::move(section_key));
  };
  auto g = make_scope_guard([&] {
    if (in_section)
      consumer.end_section();
  });
  start();
  term_state(init) {
    input(is_char<' '>, init)
    input(is_char<'\t'>, init)
    input(is_char<';'>, leading_comment)
    input(is_char<'['>, start_section)
  }
  // A comment before the first section starts. Jumps back to init after
  // hitting newline.
  term_state(leading_comment) {
    input(is_char<'\n'>, init)
    any_input(leading_comment)
  }
  // Read the section key after reading an '['.
  state(start_section) {
    input(is_char<' '>, start_section)
    input(is_char<'\t'>, start_section)
    action(isalpha, read_section_name, tmp = ch)
  }
  state(read_section_name) {
    action(is_alnum_or_dash, read_section_name, tmp += ch)
    epsilon(close_section)
  }
  // Wait for the closing ']', preceded by any number of whitespaces.
  state(close_section) {
    input(is_char<' '>, close_section)
    input(is_char<'\t'>, close_section)
    action(is_char<']'>, dispatch, begin_section())
  }
  // Dispatches to read sections, comments, or key/value pairs.
  term_state(dispatch) {
    input(is_char<' '>, dispatch)
    input(is_char<'\t'>, dispatch)
    input(is_char<'\n'>, dispatch)
    input(is_char<'['>, read_section_name)
    input(is_char<';'>, comment)
  }
  term_state(comment) {
    input(is_char<'\n'>, dispatch)
    any_input(comment)
  }
  fin();
}

} // namespace parser
} // namespace detail
} // namespace caf
