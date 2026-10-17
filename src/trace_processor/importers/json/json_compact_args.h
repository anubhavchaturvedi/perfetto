/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_JSON_JSON_COMPACT_ARGS_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_JSON_JSON_COMPACT_ARGS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/util/json_parser.h"

namespace perfetto::trace_processor {

class ArgsInserter;
class TraceStorage;

// Stores the args of JSON events in a compact, pre-flattened form.
//
// Encode() flattens the "args" object of an event, using the same key naming
// and traversal as json::AddJsonValueToArgs(), into a sequence of leaves
// (flat key, key, typed value) and array element markers. Keys and short
// strings are replaced by ids into tables owned by this class, and numbers are
// stored as varints or raw doubles. AddArgs() replays the sequence into an
// args inserter, making the same calls json::AddJsonValueToArgs() would have
// made on the original JSON, in the same order.
//
// Strings are interned into the trace's StringPool by AddArgs(), the first
// time an arg uses them, i.e. at the same point json::AddJsonValueToArgs()
// would have interned them. Interning them when encoding would instead
// interleave them with the strings interned by the tokenizer and change the
// StringPool ids, some of which are visible: JSON events with string pids and
// tids use the id of the string as the pid or tid.
class JsonCompactArgs {
 public:
  explicit JsonCompactArgs(TraceStorage* storage);
  ~JsonCompactArgs();

  JsonCompactArgs(const JsonCompactArgs&) = delete;
  JsonCompactArgs& operator=(const JsonCompactArgs&) = delete;

  // Encodes |args|, the JSON text of an object, into |out|.
  //
  // Array elements are numbered as by an inserter which has not seen any
  // array with the same key yet. For args merged into an inserter which may
  // have (those of end events), pass |allow_arrays| = false.
  //
  // Returns false if |args| cannot be replayed exactly from the encoding: if
  // it contains an array and |allow_arrays| is false, if it is malformed
  // (json::AddJsonValueToArgs() would stop half way) or if a key cannot be
  // added to the string tables. Callers should then keep the JSON text.
  bool Encode(std::string_view args,
              bool allow_arrays,
              std::vector<uint8_t>* out);

  // Adds the args encoded by Encode() to |inserter|.
  void AddArgs(std::string_view encoded, ArgsInserter* inserter);

 private:
  // A table of distinct strings, each with a dense id.
  class StringTable {
   public:
    StringTable();
    ~StringTable();

    // Returns the id of |str|, adding it to the table if needed. Returns
    // nullopt if the table is full or, in the unlikely case of a hash
    // collision, if the hash of |str| is already used by another string.
    std::optional<uint32_t> Intern(std::string_view str);

    // Returns the StringPool id of the string with id |id|, interning the
    // string into |storage| on first use.
    StringPool::Id Resolve(uint32_t id, TraceStorage* storage);

    size_t size() const { return resolved_.size(); }

   private:
    std::string_view Get(uint32_t id) const;

    base::FlatHashMap<uint64_t,
                      uint32_t,
                      base::AlreadyHashed<uint64_t>,
                      base::LinearProbe,
                      /*AppendOnly=*/true>
        index_;
    std::vector<char> data_;
    // Offset of each string in |data_|, plus the end of the last one.
    std::vector<uint32_t> offsets_;
    // StringPool id of each string; null until resolved.
    std::vector<StringPool::Id> resolved_;
  };

  struct Frame {
    uint32_t flat_key_size;
    uint32_t key_size;
    // Whether the frame is an array or is below one, in which case keys differ
    // from flat keys.
    bool in_array;
    bool is_array;
    // For arrays: the key id of the array and the index of its next element.
    uint32_t array_key = 0;
    uint32_t next_index = 0;
  };

  TraceStorage* const storage_;
  StringTable keys_;
  StringTable values_;

  // Scratch state of Encode().
  json::Iterator it_;
  std::string flat_key_;
  std::string key_;
  std::vector<Frame> stack_;
  // Per key id: the last Encode() call (by generation) which used the key for
  // an array, and the index of the next element of that array. Within one
  // call, an array whose key was already used continues the numbering, as
  // the inserter does.
  std::vector<uint32_t> array_generation_;
  std::vector<uint32_t> array_next_index_;
  uint32_t generation_ = 0;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_JSON_JSON_COMPACT_ARGS_H_
