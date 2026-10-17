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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_TRACE_EXPORT_PERFETTO_EXPORT_SESSION_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_TRACE_EXPORT_PERFETTO_EXPORT_SESSION_H_

#include <cstdint>

#include "perfetto/base/status.h"
#include "src/trace_processor/types/trace_manifest_state.h"

namespace perfetto::trace_processor {
class GlobalMetadataTracker;
class GlobalStatsTracker;
class TraceStorage;
namespace json {
class JsonDictSerializer;
}  // namespace json
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::trace_export {

// The metadata and stats tables are not restored as tables: their rows are
// keyed through the trackers of the loading instance, which also write rows of
// their own. Instead, a kPerfetto manifest carries the rows of the exporting
// session in an internal `__exported_session` block and the loader replays them
// through those trackers.
//
// Precedence when both sessions have a value:
//   * Stats: the exporting session's values are added to the loader's, except
//     for the wall-time stats which time the loading session itself
//     (parse_trace_duration_ns, guess_trace_type_duration_ns): the loader's
//     values win. An archive load records no per-trace stats, so per-trace
//     values are restored as they were; global error counters sum both
//     sessions.
//   * Metadata: the exporting session's values win (e.g. trace_uuid replaces
//     the uuid hashed from the archive bytes). Entries only the loader writes,
//     such as the archive's manifest_attribute.* rows, are kept. The archive's
//     own members are not traces and get no trace_type or trace_size_bytes
//     metadata (see TraceFileTracker::StartParsing).
//
// Machine and trace ids keep the exporting session's numbering. Machine ids
// match the restored machine table; the trace_file table is not part of the
// archive, so trace ids do not refer to the loader's trace_file rows (as for
// the already restored __intrinsic_trace_diagnostics table).

// Writes the members of the `__exported_session` block for |storage|.
void WritePerfettoExportSession(const TraceStorage& storage,
                                json::JsonDictSerializer& session);

// Replays |session| through the trackers. Must be called once, after every
// table of the archive has been restored. Rows which do not match this
// version's definition of their key (e.g. an unknown stat or a different
// scope) are skipped. Returns the number of skipped rows.
uint32_t RestorePerfettoExportSession(
    const TraceManifestState::ExportedSession& session,
    TraceStorage* storage,
    GlobalMetadataTracker* metadata_tracker,
    GlobalStatsTracker* stats_tracker);

}  // namespace perfetto::trace_processor::trace_export

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_TRACE_EXPORT_PERFETTO_EXPORT_SESSION_H_
