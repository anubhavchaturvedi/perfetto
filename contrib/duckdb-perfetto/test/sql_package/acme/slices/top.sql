-- Depends on another module of the same package.
INCLUDE PERFETTO MODULE acme.slices.stats;

CREATE PERFETTO VIEW acme_top_thread AS
SELECT * FROM acme_thread_stats ORDER BY total_dur DESC LIMIT 1;
