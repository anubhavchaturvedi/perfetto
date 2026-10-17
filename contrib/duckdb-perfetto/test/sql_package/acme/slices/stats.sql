-- A stand-in for a private PerfettoSQL package (e.g. com.meta.pytorch):
-- slice counts and durations per thread.
CREATE PERFETTO TABLE acme_thread_stats AS
SELECT t.name AS thread_name, count(*) AS slice_count, sum(s.dur) AS total_dur
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t USING (utid)
GROUP BY t.name;
