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
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

class StringExportOutput : public TraceProcessor::ExportOutput {
 public:
  base::Status Write(const void* data, size_t size) override {
    bytes.append(static_cast<const char*>(data), size);
    return base::OkStatus();
  }
  std::string bytes;
};

base::Status Load(TraceProcessor* tp, const std::string& bytes) {
  constexpr size_t kChunk = 64 * 1024;
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

Config CompressionConfig(const char* compression) {
  Config config;
  config.enable_dev_features = true;
  config.dev_flags["export-compression"] = compression;
  return config;
}

std::unique_ptr<TraceProcessor> LoadOrDie(const std::string& bytes,
                                          const Config& config = Config()) {
  auto tp = TraceProcessor::CreateInstance(config);
  base::Status status = Load(tp.get(), bytes);
  PERFETTO_CHECK(status.ok());
  return tp;
}

std::string ExportOrDie(TraceProcessor* tp,
                        TraceProcessor::ExportFormat format) {
  StringExportOutput output;
  base::Status status = tp->Export(format, &output);
  PERFETTO_CHECK(status.ok());
  return std::move(output.bytes);
}

// Enough slices with args for the args and slice tables to compress.
std::string JsonTrace() {
  std::string json = R"({"traceEvents":[)";
  for (int i = 0; i < 2000; ++i) {
    json += base::StackString<256>(
                R"(%s{"name":"slice %d","ph":"X","ts":%d,"dur":5,"pid":1,)"
                R"("tid":%d,"args":{"index":%d,"label":"label %d"}})",
                i ? "," : "", i % 50, i * 10, 1 + i % 4, i, i % 7)
                .ToStdString();
  }
  return json + "]}";
}

constexpr char kSummaryQuery[] =
    "SELECT count(*), sum(dur), group_concat(DISTINCT name), "
    "(SELECT count(*) FROM args), "
    "(SELECT sum(int_value) FROM args WHERE key = 'args.index') "
    "FROM slice";

std::string Summary(TraceProcessor* tp) {
  auto it = tp->ExecuteQuery(kSummaryQuery);
  PERFETTO_CHECK(it.Next());
  std::string summary;
  for (uint32_t i = 0; i < it.ColumnCount(); ++i) {
    SqlValue value = it.Get(i);
    summary += value.type == SqlValue::kString
                   ? std::string(value.string_value)
                   : std::to_string(value.long_value);
    summary += "|";
  }
  return summary;
}

TEST(TraceExportCompressionTest, ZstdArchiveRoundTrips) {
  std::string trace = JsonTrace();
  auto plain = LoadOrDie(trace);
  auto zstd = LoadOrDie(trace, CompressionConfig("zstd"));
  std::string uncompressed =
      ExportOrDie(plain.get(), TraceProcessor::ExportFormat::kPerfetto);
  std::string compressed =
      ExportOrDie(zstd.get(), TraceProcessor::ExportFormat::kPerfetto);
  EXPECT_LT(compressed.size() * 2, uncompressed.size());

  // Compressed tables declare the table format which older readers reject.
  EXPECT_NE(uncompressed.find(R"("format":1,)"), std::string::npos);
  EXPECT_EQ(uncompressed.find(R"("format":2,)"), std::string::npos);
  EXPECT_NE(compressed.find(R"("format":2,)"), std::string::npos);
  EXPECT_EQ(compressed.find(R"("format":1,)"), std::string::npos);

  // Reading needs no flag.
  auto reloaded = LoadOrDie(compressed);
  EXPECT_EQ(Summary(reloaded.get()), Summary(plain.get()));
}

TEST(TraceExportCompressionTest, DefaultExportIsUnchanged) {
  // kArrowTar is not compared: it includes tables, such as the build flags,
  // which differ when developer flags are set.
  std::string trace = JsonTrace();
  auto plain = LoadOrDie(trace);
  auto none = LoadOrDie(trace, CompressionConfig("none"));
  EXPECT_TRUE(
      ExportOrDie(none.get(), TraceProcessor::ExportFormat::kPerfetto) ==
      ExportOrDie(plain.get(), TraceProcessor::ExportFormat::kPerfetto));
}

TEST(TraceExportCompressionTest, ZstdArrowTar) {
  std::string trace = JsonTrace();
  auto plain = LoadOrDie(trace);
  auto zstd = LoadOrDie(trace, CompressionConfig("zstd"));
  std::string uncompressed =
      ExportOrDie(plain.get(), TraceProcessor::ExportFormat::kArrowTar);
  std::string compressed =
      ExportOrDie(zstd.get(), TraceProcessor::ExportFormat::kArrowTar);
  // kArrowTar includes every table, even empty ones, whose framing dominates
  // and is not compressed.
  EXPECT_LT(compressed.size(), uncompressed.size());
}

TEST(TraceExportCompressionTest, RejectsUnknownCompression) {
  auto tp = LoadOrDie(JsonTrace(), CompressionConfig("lz4"));
  StringExportOutput output;
  base::Status status =
      tp->Export(TraceProcessor::ExportFormat::kPerfetto, &output);
  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.message().find("export-compression"), std::string::npos);
}

}  // namespace
}  // namespace perfetto::trace_processor
