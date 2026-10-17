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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/importers/common/global_metadata_tracker.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/plugins/trace_export/perfetto_export_manifest.h"
#include "src/trace_processor/plugins/trace_export/perfetto_export_session.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_manifest_state.h"
#include "src/trace_processor/types/variadic.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/system_info.pbzero.h"
#include "protos/perfetto/common/trace_stats.pbzero.h"
#include "protos/perfetto/config/trace_config.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/trace_uuid.pbzero.h"

namespace perfetto::trace_processor::trace_export {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;

// The wall-time stats time the loading session itself, so a reload reports its
// own values (see perfetto_export_session.h).
constexpr char kStatsQuery[] =
    "SELECT name, idx, severity, source, value, machine_id, trace_id "
    "FROM stats "
    "WHERE name NOT IN ('parse_trace_duration_ns', "
    "                   'guess_trace_type_duration_ns') "
    "ORDER BY name, idx, machine_id, trace_id";

// The loader adds the archive's own manifest attribute.
constexpr char kMetadataQuery[] =
    "SELECT name, key_type, int_value, str_value, machine_id, trace_id "
    "FROM metadata "
    "WHERE name != 'manifest_attribute.perfetto_export_writer_version' "
    "ORDER BY name, machine_id, trace_id, int_value, str_value";

class StringExportOutput : public TraceProcessor::ExportOutput {
 public:
  base::Status Write(const void* data, size_t size) override {
    bytes.append(static_cast<const char*>(data), size);
    return base::OkStatus();
  }
  std::string bytes;
};

base::Status Load(TraceProcessor* tp, const std::string& bytes) {
  constexpr size_t kChunk = 4096;
  for (size_t i = 0; i < bytes.size(); i += kChunk) {
    size_t len = std::min(kChunk, bytes.size() - i);
    auto buf = std::make_unique<uint8_t[]>(len);
    memcpy(buf.get(), bytes.data() + i, len);
    base::Status status = tp->Parse(std::move(buf), len);
    if (!status.ok()) {
      return status;
    }
  }
  return tp->NotifyEndOfFile();
}

std::unique_ptr<TraceProcessor> LoadOrDie(const std::string& bytes) {
  auto tp = TraceProcessor::CreateInstance(Config());
  base::Status status = Load(tp.get(), bytes);
  PERFETTO_CHECK(status.ok());
  return tp;
}

std::string ExportPerfetto(TraceProcessor* tp) {
  StringExportOutput output;
  base::Status status =
      tp->Export(TraceProcessor::ExportFormat::kPerfetto, &output);
  PERFETTO_CHECK(status.ok());
  return std::move(output.bytes);
}

std::string ToString(const SqlValue& value) {
  switch (value.type) {
    case SqlValue::kNull:
      return "NULL";
    case SqlValue::kLong:
      return std::to_string(value.long_value);
    case SqlValue::kDouble:
      return std::to_string(value.double_value);
    case SqlValue::kString:
      return std::string("'") + value.string_value + "'";
    case SqlValue::kBytes:
      return "<bytes>";
  }
  return "?";
}

std::vector<std::string> QueryRows(TraceProcessor* tp, const std::string& sql) {
  std::vector<std::string> rows;
  auto it = tp->ExecuteQuery(sql);
  while (it.Next()) {
    std::string row;
    for (uint32_t i = 0; i < it.ColumnCount(); ++i) {
      row += (i ? "|" : "") + ToString(it.Get(i));
    }
    rows.push_back(std::move(row));
  }
  EXPECT_TRUE(it.Status().ok()) << it.Status().message();
  return rows;
}

int64_t QueryInt(TraceProcessor* tp, const std::string& sql) {
  auto it = tp->ExecuteQuery(sql);
  EXPECT_TRUE(it.Next()) << sql;
  return it.Get(0).long_value;
}

// Replaces the first occurrence of |from| with |to|, which must have the same
// length so the tar framing stays valid.
void ReplaceInPlace(std::string* archive,
                    const std::string& from,
                    const std::string& to) {
  PERFETTO_CHECK(from.size() == to.size());
  size_t pos = archive->find(from);
  PERFETTO_CHECK(pos != std::string::npos);
  archive->replace(pos, from.size(), to);
}

// Two partially overlapping complete events (an import error), a flow end
// without a start and a display time unit: all recorded as per-trace stats.
constexpr char kJsonTrace[] = R"({"displayTimeUnit":"ns","traceEvents":[
{"name":"a","ph":"X","ts":0,"dur":10,"pid":1,"tid":1},
{"name":"b","ph":"X","ts":5,"dur":10,"pid":1,"tid":1},
{"name":"f","ph":"f","id":7,"ts":2,"pid":1,"tid":1,"bp":"e"}
]})";

// A proto trace with machine-scoped and trace-scoped metadata, a trace uuid,
// indexed stats and a session name which is not printable ASCII.
std::string ProtoTrace() {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;
  {
    auto* packet = trace->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* config = packet->set_trace_config();
    config->add_buffers()->set_size_kb(1024);
    config->set_unique_session_name("session \xF0\x9F\x98\x80 \x7f");
  }
  {
    auto* packet = trace->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* uuid = packet->set_trace_uuid();
    uuid->set_msb(0x1234);
    uuid->set_lsb(0x5678);
  }
  {
    auto* packet = trace->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* info = packet->set_system_info();
    auto* utsname = info->set_utsname();
    utsname->set_sysname("Linux");
    utsname->set_release("6.1");
    utsname->set_version("#1 SMP");
    utsname->set_machine("aarch64");
    info->set_timezone_off_mins(-420);
  }
  {
    auto* packet = trace->add_packet();
    packet->set_trusted_packet_sequence_id(1);
    auto* stats = packet->set_trace_stats();
    auto* buffer = stats->add_buffer_stats();
    buffer->set_buffer_size(1024 * 1024);
    buffer->set_bytes_written(4096);
    buffer->set_chunks_discarded(3);
    stats->set_producers_connected(2);
    stats->set_chunks_discarded(5);
  }
  return trace.SerializeAsString();
}

TEST(PerfettoExportSessionTest, JsonRoundTripRestoresParseHealth) {
  auto original = LoadOrDie(kJsonTrace);
  std::vector<std::string> stats = QueryRows(original.get(), kStatsQuery);
  std::vector<std::string> metadata = QueryRows(original.get(), kMetadataQuery);
  // The trace has per-trace stats worth restoring, including an import error.
  ASSERT_GT(QueryInt(original.get(),
                     "SELECT count(*) FROM stats WHERE value != 0 AND "
                     "trace_id IS NOT NULL AND severity = 'error'"),
            0);

  auto reloaded = LoadOrDie(ExportPerfetto(original.get()));
  EXPECT_EQ(QueryRows(reloaded.get(), kStatsQuery), stats);
  EXPECT_EQ(QueryRows(reloaded.get(), kMetadataQuery), metadata);

  // Metadata describes the original trace rather than the archive.
  EXPECT_THAT(QueryRows(reloaded.get(),
                        "SELECT str_value FROM metadata "
                        "WHERE name = 'trace_type'"),
              ElementsAre("'json'"));
  EXPECT_EQ(QueryInt(reloaded.get(),
                     "SELECT int_value FROM metadata "
                     "WHERE name = 'trace_size_bytes'"),
            static_cast<int64_t>(strlen(kJsonTrace)));
  EXPECT_THAT(QueryRows(reloaded.get(),
                        "SELECT str_value FROM metadata "
                        "WHERE name LIKE 'manifest_attribute.%'"),
              Not(IsEmpty()));
}

TEST(PerfettoExportSessionTest, ProtoRoundTripRestoresMetadataAndStats) {
  auto original = LoadOrDie(ProtoTrace());
  std::vector<std::string> stats = QueryRows(original.get(), kStatsQuery);
  std::vector<std::string> metadata = QueryRows(original.get(), kMetadataQuery);
  ASSERT_THAT(metadata, Contains("'system_name'|'single'|NULL|'Linux'|0|NULL"));
  ASSERT_THAT(stats, Contains("'traced_buf_bytes_written'|0|'info'|'trace'|"
                              "4096|0|0"));
  std::string uuid = QueryRows(original.get(),
                               "SELECT str_value FROM metadata "
                               "WHERE name = 'trace_uuid'")[0];

  auto reloaded = LoadOrDie(ExportPerfetto(original.get()));
  EXPECT_EQ(QueryRows(reloaded.get(), kStatsQuery), stats);
  EXPECT_EQ(QueryRows(reloaded.get(), kMetadataQuery), metadata);
  // The trace's own uuid replaces the one hashed from the archive bytes.
  EXPECT_THAT(QueryRows(reloaded.get(),
                        "SELECT str_value FROM metadata "
                        "WHERE name = 'trace_uuid'"),
              ElementsAre(uuid));
}

TEST(PerfettoExportSessionTest, ReexportIsStable) {
  auto original = LoadOrDie(kJsonTrace);
  auto reloaded = LoadOrDie(ExportPerfetto(original.get()));
  auto reloaded_twice = LoadOrDie(ExportPerfetto(reloaded.get()));
  EXPECT_EQ(QueryRows(reloaded_twice.get(), kStatsQuery),
            QueryRows(original.get(), kStatsQuery));
  EXPECT_EQ(QueryRows(reloaded_twice.get(), kMetadataQuery),
            QueryRows(original.get(), kMetadataQuery));
}

TEST(PerfettoExportSessionTest, LoaderKeepsItsOwnWallTimeStats) {
  auto original = LoadOrDie(kJsonTrace);
  auto reloaded = LoadOrDie(ExportPerfetto(original.get()));
  // Each session reports one row for its own parse time.
  EXPECT_EQ(QueryInt(reloaded.get(),
                     "SELECT count(*) FROM stats "
                     "WHERE name = 'parse_trace_duration_ns'"),
            1);
}

TEST(PerfettoExportSessionTest, ArchiveWithoutSessionStillLoads) {
  auto original = LoadOrDie(kJsonTrace);
  std::string archive = ExportPerfetto(original.get());
  // An archive from a writer without session support: the block is absent
  // (here: renamed to a key which readers ignore).
  ReplaceInPlace(&archive, "\"__exported_session\"", "\"__exported_sessioX\"");

  auto reloaded = LoadOrDie(archive);
  EXPECT_EQ(QueryInt(reloaded.get(), "SELECT count(*) FROM slice"),
            QueryInt(original.get(), "SELECT count(*) FROM slice"));
  // Nothing is restored, and the archive members are not traces.
  EXPECT_EQ(QueryInt(reloaded.get(),
                     "SELECT count(*) FROM stats WHERE trace_id IS NOT NULL"),
            0);
  EXPECT_EQ(QueryInt(reloaded.get(),
                     "SELECT count(*) FROM metadata "
                     "WHERE name IN ('trace_type', 'trace_size_bytes')"),
            0);
}

TEST(PerfettoExportSessionTest, MalformedSessionIsRejected) {
  auto original = LoadOrDie(kJsonTrace);
  std::string archive = ExportPerfetto(original.get());
  ReplaceInPlace(&archive, "\"value\":", "\"valuX\":");

  auto reloaded = TraceProcessor::CreateInstance(Config());
  base::Status status = Load(reloaded.get(), archive);
  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.message().find("__exported_session"), std::string::npos)
      << status.message();
}

TEST(PerfettoExportSessionTest, SessionWithUnsupportedFormatIsIgnored) {
  auto original = LoadOrDie(kJsonTrace);
  std::string archive = ExportPerfetto(original.get());
  // The tables keep format 1; only the session block claims a newer format.
  ReplaceInPlace(&archive, "\"__exported_session\":{\"format\":1",
                 "\"__exported_session\":{\"format\":9");

  auto reloaded = LoadOrDie(archive);
  EXPECT_EQ(QueryInt(reloaded.get(), "SELECT count(*) FROM slice"),
            QueryInt(original.get(), "SELECT count(*) FROM slice"));
  EXPECT_EQ(QueryInt(reloaded.get(),
                     "SELECT count(*) FROM stats WHERE trace_id IS NOT NULL"),
            0);
}

class PerfettoExportSessionRestoreTest : public ::testing::Test {
 protected:
  using MachineId = tables::MachineTable::Id;
  using TraceId = tables::TraceFileTable::Id;

  PerfettoExportSessionRestoreTest()
      : stats_tracker_(&storage_), metadata_tracker_(&storage_) {
    session_.format = kPerfettoExportFormatVersion;
  }

  uint32_t Restore() {
    return RestorePerfettoExportSession(session_, &storage_, &metadata_tracker_,
                                        &stats_tracker_);
  }

  void AddStat(const char* name,
               std::optional<int64_t> idx,
               int64_t value,
               std::optional<uint32_t> machine_id,
               std::optional<uint32_t> trace_id) {
    session_.stats.push_back({name, idx, value, machine_id, trace_id});
  }

  void AddMetadata(const char* name,
                   const char* key_type,
                   std::optional<int64_t> int_value,
                   std::optional<std::string> str_value,
                   std::optional<uint32_t> machine_id,
                   std::optional<uint32_t> trace_id) {
    session_.metadata.push_back({name, key_type, int_value,
                                 std::move(str_value), machine_id, trace_id});
  }

  uint32_t MetadataRowsNamed(const char* name) {
    StringId id = storage_.InternString(name);
    uint32_t count = 0;
    for (auto it = storage_.metadata_table().IterateRows(); it; ++it) {
      count += it.name() == id;
    }
    return count;
  }

  TraceStorage storage_;
  GlobalStatsTracker stats_tracker_;
  GlobalMetadataTracker metadata_tracker_;
  TraceManifestState::ExportedSession session_;
};

TEST_F(PerfettoExportSessionRestoreTest, RestoresStats) {
  AddStat("json_display_time_unit", std::nullopt, 1, 0, 1);
  AddStat("traced_buf_bytes_written", 2, 4096, 0, 1);
  AddStat("sorter_push_event_out_of_order", std::nullopt, 3, std::nullopt,
          std::nullopt);
  EXPECT_EQ(Restore(), 0u);
  EXPECT_EQ(stats_tracker_.GetStats(MachineId(0), TraceId(1),
                                    stats::json_display_time_unit),
            1);
  EXPECT_EQ(stats_tracker_
                .GetIndexedStats(MachineId(0), TraceId(1),
                                 stats::traced_buf_bytes_written, 2)
                .value_or(-1),
            4096);
  EXPECT_EQ(
      stats_tracker_.GetGlobalStats(stats::sorter_push_event_out_of_order), 3);
}

TEST_F(PerfettoExportSessionRestoreTest, GlobalCountersSumBothSessions) {
  stats_tracker_.SetGlobalStats(stats::sorter_push_event_out_of_order, 2);
  stats_tracker_.SetGlobalStats(stats::parse_trace_duration_ns, 10);
  AddStat("sorter_push_event_out_of_order", std::nullopt, 3, std::nullopt,
          std::nullopt);
  AddStat("parse_trace_duration_ns", std::nullopt, 1000, std::nullopt,
          std::nullopt);
  EXPECT_EQ(Restore(), 0u);
  EXPECT_EQ(
      stats_tracker_.GetGlobalStats(stats::sorter_push_event_out_of_order), 5);
  // The loading session's wall time wins.
  EXPECT_EQ(stats_tracker_.GetGlobalStats(stats::parse_trace_duration_ns), 10);
}

TEST_F(PerfettoExportSessionRestoreTest, SkipsStatsThisVersionDoesNotDefine) {
  AddStat("no_such_stat", std::nullopt, 1, 0, 1);
  // Scope mismatches: a per-trace stat without ids and a global one with ids.
  AddStat("json_display_time_unit", std::nullopt, 1, std::nullopt,
          std::nullopt);
  AddStat("sorter_push_event_out_of_order", std::nullopt, 1, 0, 1);
  // Type mismatches: an index on a single stat and none on an indexed one.
  AddStat("json_display_time_unit", 3, 1, 0, 1);
  AddStat("traced_buf_bytes_written", std::nullopt, 1, 0, 1);
  EXPECT_EQ(Restore(), 5u);
  EXPECT_EQ(stats_tracker_.GetStats(MachineId(0), TraceId(1),
                                    stats::json_display_time_unit),
            0);
}

TEST_F(PerfettoExportSessionRestoreTest, RestoresMetadata) {
  AddMetadata("trace_type", "single", std::nullopt, "json", std::nullopt, 1);
  AddMetadata("system_name", "single", std::nullopt, "Linux", 0, std::nullopt);
  AddMetadata("trace_time_clock_id", "single", 6, std::nullopt, std::nullopt,
              std::nullopt);
  AddMetadata("all_data_source_flushed_ns", "multi", 1, std::nullopt, 0, 1);
  AddMetadata("all_data_source_flushed_ns", "multi", 2, std::nullopt, 0, 1);
  AddMetadata("trace_attribute.user", "single", std::nullopt, "me", 0, 1);
  // Repeated dynamic keys were appended, not set.
  AddMetadata("cr-legacy", "single", 1, std::nullopt, std::nullopt,
              std::nullopt);
  AddMetadata("cr-legacy", "single", 2, std::nullopt, std::nullopt,
              std::nullopt);
  EXPECT_EQ(Restore(), 0u);

  auto trace_type = metadata_tracker_.GetMetadata(std::nullopt, TraceId(1),
                                                  metadata::trace_type);
  ASSERT_TRUE(trace_type.has_value());
  EXPECT_STREQ(trace_type->string_value, "json");
  auto system_name = metadata_tracker_.GetMetadata(MachineId(0), std::nullopt,
                                                   metadata::system_name);
  ASSERT_TRUE(system_name.has_value());
  EXPECT_STREQ(system_name->string_value, "Linux");
  EXPECT_EQ(MetadataRowsNamed("all_data_source_flushed_ns"), 2u);
  EXPECT_EQ(MetadataRowsNamed("trace_attribute.user"), 1u);
  EXPECT_EQ(MetadataRowsNamed("cr-legacy"), 2u);
}

TEST_F(PerfettoExportSessionRestoreTest, TraceUuidReplacesLoaderUuid) {
  // The loader hashes a uuid from the archive bytes before the trace is known.
  metadata_tracker_.SetMetadata(
      std::nullopt, std::nullopt, metadata::trace_uuid,
      Variadic::String(storage_.InternString("archive-uuid")));
  AddMetadata("trace_uuid", "single", std::nullopt, "trace-uuid", std::nullopt,
              0);
  EXPECT_EQ(Restore(), 0u);
  EXPECT_EQ(MetadataRowsNamed("trace_uuid"), 1u);
  auto uuid = metadata_tracker_.GetMetadata(std::nullopt, TraceId(0),
                                            metadata::trace_uuid);
  ASSERT_TRUE(uuid.has_value());
  EXPECT_STREQ(uuid->string_value, "trace-uuid");
}

TEST_F(PerfettoExportSessionRestoreTest,
       SkipsMetadataThisVersionDoesNotDefine) {
  // Wrong value type, wrong key type, wrong scope and no value.
  AddMetadata("trace_type", "single", 1, std::nullopt, std::nullopt, 1);
  AddMetadata("trace_type", "multi", std::nullopt, "json", std::nullopt, 1);
  AddMetadata("system_name", "single", std::nullopt, "Linux", std::nullopt,
              std::nullopt);
  AddMetadata("trace_attribute.empty", "single", std::nullopt, std::nullopt, 0,
              1);
  // A static multi key of another version.
  AddMetadata("no_such_key", "multi", 1, std::nullopt, std::nullopt,
              std::nullopt);
  EXPECT_EQ(Restore(), 5u);
  EXPECT_EQ(storage_.metadata_table().row_count(), 0u);
}

TEST_F(PerfettoExportSessionRestoreTest, IgnoresUnsupportedFormat) {
  session_.format = kPerfettoExportFormatVersion + 1;
  AddStat("json_display_time_unit", std::nullopt, 1, 0, 1);
  AddMetadata("trace_type", "single", std::nullopt, "json", std::nullopt, 1);
  EXPECT_EQ(Restore(), 2u);
  EXPECT_EQ(storage_.metadata_table().row_count(), 0u);
  EXPECT_EQ(stats_tracker_.GetStats(MachineId(0), TraceId(1),
                                    stats::json_display_time_unit),
            0);
}

}  // namespace
}  // namespace perfetto::trace_processor::trace_export
