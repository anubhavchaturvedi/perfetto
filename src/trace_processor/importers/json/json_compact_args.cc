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

#include "src/trace_processor/importers/json/json_compact_args.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/variant.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/json_parser.h"

namespace perfetto::trace_processor {
namespace {

using protozero::proto_utils::ParseVarInt;
using protozero::proto_utils::WriteVarInt;
using protozero::proto_utils::ZigZagDecode;
using protozero::proto_utils::ZigZagEncode;

// Every op of the encoding starts with a varint "tag": the op in the low
// |kOpBits| bits, then the |kHasFlatKeyBit| bit, then the id of the key.
// Leaves with |kHasFlatKeyBit| are followed by the id of their flat key, which
// differs from the key below arrays. Then comes the value, if any.
enum Op : uint32_t {
  // An element of the array whose key has the id in the tag.
  kArrayElement = 0,
  // A zigzag varint.
  kInt = 1,
  // 8 bytes, in host order.
  kDouble = 2,
  // A varint: the id of the string in |values_|.
  kString = 3,
  // A varint size, then the bytes of the string.
  kInlineString = 4,
  kTrue = 5,
  kFalse = 6,
};
constexpr uint32_t kOpBits = 3;
constexpr uint32_t kOpMask = (1u << kOpBits) - 1;
constexpr uint32_t kHasFlatKeyBit = 1u << kOpBits;
constexpr uint32_t kTagFlagBits = kOpBits + 1;
constexpr size_t kMaxVarIntSize = 10;

// String values up to this size are stored in |values_|, longer ones (which
// are less likely to repeat) inline.
constexpr size_t kMaxTableValueSize = 256;

// Caps the size of the strings in each table, so that traces with many
// distinct strings do not keep a second copy of all of them: once full, new
// values are stored inline and events with new keys keep their JSON text.
constexpr size_t kMaxTableBytes = 64 * 1024 * 1024;

uint64_t ReadVarInt(const uint8_t** ptr, const uint8_t* end) {
  uint64_t value = 0;
  const uint8_t* next = ParseVarInt(*ptr, end, &value);
  PERFETTO_CHECK(next != *ptr);
  *ptr = next;
  return value;
}

uint32_t ReadId(const uint8_t** ptr, const uint8_t* end) {
  return static_cast<uint32_t>(ReadVarInt(ptr, end));
}

}  // namespace

JsonCompactArgs::StringTable::StringTable() : offsets_{0} {}
JsonCompactArgs::StringTable::~StringTable() = default;

std::optional<uint32_t> JsonCompactArgs::StringTable::Intern(
    std::string_view str) {
  uint64_t hash = base::MurmurHashValue(str);
  if (uint32_t* id = index_.Find(hash); id) {
    if (PERFETTO_UNLIKELY(Get(*id) != str)) {
      return std::nullopt;
    }
    return *id;
  }
  if (data_.size() + str.size() > kMaxTableBytes) {
    return std::nullopt;
  }
  auto id = static_cast<uint32_t>(resolved_.size());
  data_.insert(data_.end(), str.begin(), str.end());
  offsets_.push_back(static_cast<uint32_t>(data_.size()));
  resolved_.push_back(StringPool::Id::Null());
  index_.Insert(hash, id);
  return id;
}

std::string_view JsonCompactArgs::StringTable::Get(uint32_t id) const {
  uint32_t begin = offsets_[id];
  uint32_t end = offsets_[id + 1];
  // Never return a null data pointer: the StringPool interns those as the null
  // string rather than as the empty one.
  if (begin == end) {
    return std::string_view("", 0);
  }
  return {data_.data() + begin, end - begin};
}

StringPool::Id JsonCompactArgs::StringTable::Resolve(uint32_t id,
                                                     TraceStorage* storage) {
  StringPool::Id& resolved = resolved_[id];
  if (resolved.is_null()) {
    std::string_view str = Get(id);
    resolved = storage->InternString(base::StringView(str.data(), str.size()));
  }
  return resolved;
}

JsonCompactArgs::JsonCompactArgs(TraceStorage* storage) : storage_(storage) {}
JsonCompactArgs::~JsonCompactArgs() = default;

// Mirrors json::AddJsonValueToArgs(): any change in how that function names
// keys, numbers array elements or skips values must be reflected here.
bool JsonCompactArgs::Encode(std::string_view args,
                             bool allow_arrays,
                             std::vector<uint8_t>* out) {
  out->clear();
  it_.Reset(args.data(), args.data() + args.size());
  if (!it_.ParseStart() ||
      it_.parse_stack().back() != json::Iterator::ParseType::kObject) {
    return false;
  }
  if (PERFETTO_UNLIKELY(++generation_ == 0)) {
    std::fill(array_generation_.begin(), array_generation_.end(), 0u);
    generation_ = 1;
  }
  flat_key_.assign("args");
  key_.assign("args");
  stack_.clear();
  stack_.push_back(Frame{static_cast<uint32_t>(flat_key_.size()),
                         static_cast<uint32_t>(key_.size()), false, false});
  for (;;) {
    json::Iterator::ParseType parse_type = it_.parse_stack().back();
    switch (it_.ParseAndRecurse()) {
      case json::ReturnCode::kOk:
        break;
      case json::ReturnCode::kEndOfScope: {
        const Frame& frame = stack_.back();
        if (frame.is_array) {
          array_generation_[frame.array_key] = generation_;
          array_next_index_[frame.array_key] = frame.next_index;
        }
        stack_.pop_back();
        if (stack_.empty()) {
          return true;
        }
        continue;
      }
      case json::ReturnCode::kIncompleteInput:
      case json::ReturnCode::kError:
        // json::AddJsonValueToArgs() would keep the args before the error.
        return false;
    }

    const json::JsonValue& value = it_.value();
    if (value.index() == base::variant_index<json::JsonValue, json::Null>()) {
      continue;
    }
    Frame& frame = stack_.back();
    flat_key_.resize(frame.flat_key_size);
    key_.resize(frame.key_size);
    bool in_array = frame.in_array;
    if (parse_type == json::Iterator::ParseType::kArray) {
      // The inserter numbers the elements of an array (null ones excepted)
      // by the key of the array.
      uint8_t buf[kMaxVarIntSize];
      uint8_t* end = WriteVarInt(
          uint64_t{frame.array_key} << kTagFlagBits | kArrayElement, buf);
      out->insert(out->end(), buf, end);
      key_ += '[';
      key_ += std::to_string(frame.next_index++);
      key_ += ']';
    } else {
      std::string_view k = it_.key();
      key_ += '.';
      key_ += k;
      flat_key_ += '.';
      flat_key_ += k;
    }

    if (value.index() == base::variant_index<json::JsonValue, json::Object>()) {
      stack_.push_back(Frame{static_cast<uint32_t>(flat_key_.size()),
                             static_cast<uint32_t>(key_.size()), in_array,
                             false});
      continue;
    }
    if (value.index() == base::variant_index<json::JsonValue, json::Array>()) {
      if (!allow_arrays) {
        return false;
      }
      std::optional<uint32_t> array_key = keys_.Intern(key_);
      if (!array_key) {
        return false;
      }
      if (*array_key >= array_generation_.size()) {
        array_generation_.resize(keys_.size(), 0);
        array_next_index_.resize(keys_.size(), 0);
      }
      uint32_t next_index = array_generation_[*array_key] == generation_
                                ? array_next_index_[*array_key]
                                : 0;
      stack_.push_back(Frame{static_cast<uint32_t>(flat_key_.size()),
                             static_cast<uint32_t>(key_.size()), true, true,
                             *array_key, next_index});
      continue;
    }

    // A leaf: its key is followed by its flat key if they differ.
    std::optional<uint32_t> key_id = keys_.Intern(key_);
    if (!key_id) {
      return false;
    }
    std::optional<uint32_t> flat_key_id;
    if (in_array) {
      flat_key_id = keys_.Intern(flat_key_);
      if (!flat_key_id) {
        return false;
      }
    }
    // Assemble the op in |buf| to append it to |out| in one go.
    uint8_t buf[3 * kMaxVarIntSize + sizeof(double)];
    uint8_t* ptr = buf;
    auto write_tag = [&](uint32_t op) {
      ptr = WriteVarInt(uint64_t{*key_id} << kTagFlagBits |
                            (in_array ? kHasFlatKeyBit : 0) | op,
                        ptr);
      if (in_array) {
        ptr = WriteVarInt(*flat_key_id, ptr);
      }
    };
    std::string_view inline_str;
    switch (value.index()) {
      case base::variant_index<json::JsonValue, int64_t>():
        write_tag(kInt);
        ptr =
            WriteVarInt(ZigZagEncode(base::unchecked_get<int64_t>(value)), ptr);
        break;
      case base::variant_index<json::JsonValue, double>(): {
        write_tag(kDouble);
        double d = base::unchecked_get<double>(value);
        memcpy(ptr, &d, sizeof(d));
        ptr += sizeof(d);
        break;
      }
      case base::variant_index<json::JsonValue, std::string_view>(): {
        std::string_view str = base::unchecked_get<std::string_view>(value);
        std::optional<uint32_t> value_id;
        if (str.size() <= kMaxTableValueSize) {
          value_id = values_.Intern(str);
        }
        if (value_id) {
          write_tag(kString);
          ptr = WriteVarInt(*value_id, ptr);
        } else {
          write_tag(kInlineString);
          ptr = WriteVarInt(str.size(), ptr);
          inline_str = str;
        }
        break;
      }
      case base::variant_index<json::JsonValue, bool>():
        write_tag(base::unchecked_get<bool>(value) ? kTrue : kFalse);
        break;
      default:
        PERFETTO_FATAL("Unexpected JSON value");
    }
    out->insert(out->end(), buf, ptr);
    out->insert(out->end(), inline_str.begin(), inline_str.end());
  }
}

void JsonCompactArgs::AddArgs(std::string_view encoded,
                              ArgsInserter* inserter) {
  const auto* ptr = reinterpret_cast<const uint8_t*>(encoded.data());
  const uint8_t* end = ptr + encoded.size();
  while (ptr < end) {
    uint64_t tag = ReadVarInt(&ptr, end);
    auto op = static_cast<uint32_t>(tag & kOpMask);
    auto key = static_cast<uint32_t>(tag >> kTagFlagBits);
    if (op == kArrayElement) {
      inserter->IncrementArrayEntryIndex(keys_.Resolve(key, storage_));
      continue;
    }
    // As json::AddJsonValueToArgs() does, intern the flat key, then the key,
    // then the value.
    StringPool::Id flat_key_id;
    StringPool::Id key_id;
    if (tag & kHasFlatKeyBit) {
      flat_key_id = keys_.Resolve(ReadId(&ptr, end), storage_);
      key_id = keys_.Resolve(key, storage_);
    } else {
      flat_key_id = key_id = keys_.Resolve(key, storage_);
    }
    switch (op) {
      case kInt:
        inserter->AddArg(
            flat_key_id, key_id,
            Variadic::Integer(ZigZagDecode(ReadVarInt(&ptr, end))));
        break;
      case kDouble: {
        double d;
        PERFETTO_CHECK(static_cast<size_t>(end - ptr) >= sizeof(d));
        memcpy(&d, ptr, sizeof(d));
        ptr += sizeof(d);
        inserter->AddArg(flat_key_id, key_id, Variadic::Real(d));
        break;
      }
      case kString:
        inserter->AddArg(
            flat_key_id, key_id,
            Variadic::String(values_.Resolve(ReadId(&ptr, end), storage_)));
        break;
      case kInlineString: {
        auto size = static_cast<size_t>(ReadVarInt(&ptr, end));
        PERFETTO_CHECK(static_cast<size_t>(end - ptr) >= size);
        StringPool::Id str = storage_->InternString(
            base::StringView(reinterpret_cast<const char*>(ptr), size));
        ptr += size;
        inserter->AddArg(flat_key_id, key_id, Variadic::String(str));
        break;
      }
      case kTrue:
      case kFalse:
        inserter->AddArg(flat_key_id, key_id, Variadic::Boolean(op == kTrue));
        break;
      default:
        PERFETTO_FATAL("Invalid JSON args encoding");
    }
  }
}

}  // namespace perfetto::trace_processor
