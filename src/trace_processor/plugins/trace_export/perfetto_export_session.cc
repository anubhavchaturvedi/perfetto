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

#include "src/trace_processor/plugins/trace_export/perfetto_export_session.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/global_metadata_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/plugins/trace_export/perfetto_export_manifest.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/simple_json_serializer.h"

namespace perfetto::trace_processor::trace_export {
namespace {

using MachineId = tables::MachineTable::Id;
using TraceId = tables::TraceFileTable::Id;
using ExportedMetadataRow = TraceManifestState::ExportedMetadataRow;
using ExportedStatRow = TraceManifestState::ExportedStatRow;

// The manifest's JSON escaping only round-trips printable ASCII (and the
// whitespace escapes): other strings, e.g. non-BMP UTF-8, are base64-encoded.
bool RoundTripsAsJsonString(std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 0x20 && c <= 0x7e) || c == '\n' || c == '\r' || c == '\t';
  });
}

void AddExportedString(json::JsonDictSerializer& row,
                       const std::string& key,
                       std::string_view value) {
  if (RoundTripsAsJsonString(value)) {
    row.AddString(key, value);
  } else {
    row.AddString(key + "_b64", base::Base64Encode(value.data(), value.size()));
  }
}

void AddContext(json::JsonDictSerializer& row,
                std::optional<MachineId> machine_id,
                std::optional<TraceId> trace_id) {
  if (machine_id) {
    row.AddUint("machine_id", machine_id->value);
  }
  if (trace_id) {
    row.AddUint("trace_id", trace_id->value);
  }
}

// Stats which time the session itself rather than describe the trace: the
// loading session's values win.
bool IsLoaderOwnedStat(size_t key) {
  return key == stats::parse_trace_duration_ns ||
         key == stats::guess_trace_type_duration_ns;
}

bool StatScopeMatches(size_t key, const ExportedStatRow& row) {
  bool machine = row.machine_id.has_value();
  bool trace = row.trace_id.has_value();
  switch (stats::kScopes[key]) {
    case stats::Scope::kGlobal:
      return !machine && !trace;
    case stats::Scope::kMachine:
      return machine && !trace;
    case stats::Scope::kTrace:
      return !machine && trace;
    case stats::Scope::kMachineAndTrace:
      return machine && trace;
    case stats::Scope::kNumScopes:
      break;
  }
  return false;
}

bool MetadataScopeMatches(metadata::KeyId key, const ExportedMetadataRow& row) {
  bool machine = row.machine_id.has_value();
  bool trace = row.trace_id.has_value();
  // trace_uuid may be recorded before the trace id is known, in which case it
  // stays global (see GlobalMetadataTracker::GetContextIds).
  if (key == metadata::trace_uuid) {
    return !machine;
  }
  switch (metadata::kScopes[key]) {
    case metadata::Scope::kGlobal:
      return !machine && !trace;
    case metadata::Scope::kMachine:
      return machine && !trace;
    case metadata::Scope::kTrace:
      return !machine && trace;
    case metadata::Scope::kMachineAndTrace:
      return machine && trace;
    case metadata::Scope::kNumScopes:
      break;
  }
  return false;
}

std::optional<MachineId> ToMachineId(std::optional<uint32_t> id) {
  return id ? std::make_optional(MachineId(*id)) : std::nullopt;
}

std::optional<TraceId> ToTraceId(std::optional<uint32_t> id) {
  return id ? std::make_optional(TraceId(*id)) : std::nullopt;
}

uint32_t RestoreStats(const std::vector<ExportedStatRow>& rows,
                      GlobalStatsTracker* tracker) {
  base::FlatHashMap<std::string, size_t> key_by_name;
  for (size_t i = 0; i < stats::kNumKeys; ++i) {
    key_by_name.Insert(stats::kNames[i], i);
  }
  uint32_t skipped = 0;
  for (const ExportedStatRow& row : rows) {
    const size_t* key = key_by_name.Find(row.name);
    if (!key) {
      ++skipped;
      continue;
    }
    if (IsLoaderOwnedStat(*key)) {
      continue;
    }
    bool indexed = stats::kTypes[*key] == stats::kIndexed;
    if (indexed != row.idx.has_value() || !StatScopeMatches(*key, row) ||
        (indexed && (*row.idx < std::numeric_limits<int>::min() ||
                     *row.idx > std::numeric_limits<int>::max()))) {
      ++skipped;
      continue;
    }
    std::optional<MachineId> machine_id = ToMachineId(row.machine_id);
    std::optional<TraceId> trace_id = ToTraceId(row.trace_id);
    if (indexed) {
      tracker->IncrementIndexedStats(machine_id, trace_id, *key,
                                     static_cast<int>(*row.idx), row.value);
    } else {
      tracker->IncrementStats(machine_id, trace_id, *key, row.value);
    }
  }
  return skipped;
}

uint32_t RestoreMetadata(const std::vector<ExportedMetadataRow>& rows,
                         TraceStorage* storage,
                         GlobalMetadataTracker* tracker) {
  base::FlatHashMap<std::string, metadata::KeyId> key_by_name;
  for (size_t i = 0; i < metadata::kNumKeys; ++i) {
    key_by_name.Insert(metadata::kNames[i], static_cast<metadata::KeyId>(i));
  }

  // A dynamic key repeated within one (machine, trace) context was appended by
  // AppendDynamicMetadataLegacy, so it is replayed the same way.
  using Context =
      std::tuple<std::string, std::optional<uint32_t>, std::optional<uint32_t>>;
  std::map<Context, uint32_t> dynamic_counts;
  for (const ExportedMetadataRow& row : rows) {
    if (!key_by_name.Find(row.name)) {
      ++dynamic_counts[Context(row.name, row.machine_id, row.trace_id)];
    }
  }

  const char* single =
      metadata::kKeyTypeNames[static_cast<size_t>(metadata::KeyType::kSingle)];
  uint32_t skipped = 0;
  for (const ExportedMetadataRow& row : rows) {
    Variadic value = Variadic::Null();
    if (row.int_value) {
      value = Variadic::Integer(*row.int_value);
    } else if (row.str_value) {
      value = Variadic::String(
          storage->InternString(base::StringView(*row.str_value)));
    } else {
      ++skipped;
      continue;
    }
    std::optional<MachineId> machine_id = ToMachineId(row.machine_id);
    std::optional<TraceId> trace_id = ToTraceId(row.trace_id);

    if (const metadata::KeyId* key = key_by_name.Find(row.name); key) {
      metadata::KeyType key_type = metadata::kKeyTypes[*key];
      if (row.key_type !=
              metadata::kKeyTypeNames[static_cast<size_t>(key_type)] ||
          value.type != metadata::kValueTypes[*key] ||
          !MetadataScopeMatches(*key, row)) {
        ++skipped;
        continue;
      }
      if (key_type == metadata::KeyType::kSingle) {
        tracker->SetMetadata(machine_id, trace_id, *key, value);
      } else {
        tracker->AppendMetadata(machine_id, trace_id, *key, value);
      }
      continue;
    }

    // Dynamic keys are always single; anything else is a static key of
    // another version.
    if (row.key_type != single) {
      ++skipped;
      continue;
    }
    StringId name = storage->InternString(base::StringView(row.name));
    if (dynamic_counts[Context(row.name, row.machine_id, row.trace_id)] > 1) {
      tracker->AppendDynamicMetadataLegacy(machine_id, trace_id, name, value);
    } else {
      tracker->SetDynamicMetadata(machine_id, trace_id, name, value);
    }
  }
  return skipped;
}

}  // namespace

void WritePerfettoExportSession(const TraceStorage& storage,
                                json::JsonDictSerializer& session) {
  // The key_type column is not readable from C++, but it is implied by the
  // name: static keys have a fixed key type and dynamic keys are always single.
  base::FlatHashMap<std::string, metadata::KeyType> key_type_by_name;
  for (size_t i = 0; i < metadata::kNumKeys; ++i) {
    key_type_by_name.Insert(metadata::kNames[i], metadata::kKeyTypes[i]);
  }
  session.AddInt("format", kPerfettoExportFormatVersion);
  session.AddArray("metadata", [&](json::JsonArraySerializer& rows) {
    for (auto it = storage.metadata_table().IterateRows(); it; ++it) {
      rows.AppendDict([&](json::JsonDictSerializer& row) {
        std::string name = storage.GetString(it.name()).ToStdString();
        const metadata::KeyType* key_type = key_type_by_name.Find(name);
        AddExportedString(row, "name", name);
        row.AddString("key_type",
                      metadata::kKeyTypeNames[static_cast<size_t>(
                          key_type ? *key_type : metadata::KeyType::kSingle)]);
        if (std::optional<int64_t> int_value = it.int_value(); int_value) {
          row.AddInt("int_value", *int_value);
        }
        if (std::optional<StringId> str_value = it.str_value(); str_value) {
          base::StringView str = storage.GetString(*str_value);
          AddExportedString(row, "str_value",
                            std::string_view(str.data(), str.size()));
        }
        AddContext(row, it.machine_id(), it.trace_id());
      });
    }
  });
  session.AddArray("stats", [&](json::JsonArraySerializer& rows) {
    for (auto it = storage.stats_table().IterateRows(); it; ++it) {
      rows.AppendDict([&](json::JsonDictSerializer& row) {
        AddExportedString(row, "name", storage.GetString(it.name()).c_str());
        if (std::optional<int64_t> idx = it.idx(); idx) {
          row.AddInt("idx", *idx);
        }
        row.AddInt("value", it.value());
        AddContext(row, it.machine_id(), it.trace_id());
      });
    }
  });
}

uint32_t RestorePerfettoExportSession(
    const TraceManifestState::ExportedSession& session,
    TraceStorage* storage,
    GlobalMetadataTracker* metadata_tracker,
    GlobalStatsTracker* stats_tracker) {
  if (session.format != kPerfettoExportFormatVersion) {
    // The session describes the trace but is not needed to query the restored
    // tables, so an archive from a newer writer still loads without it.
    PERFETTO_ELOG(
        "perfetto_export: ignoring __exported_session with unsupported format "
        "%" PRId64,
        session.format);
    return static_cast<uint32_t>(session.metadata.size() +
                                 session.stats.size());
  }
  return RestoreMetadata(session.metadata, storage, metadata_tracker) +
         RestoreStats(session.stats, stats_tracker);
}

}  // namespace perfetto::trace_processor::trace_export
