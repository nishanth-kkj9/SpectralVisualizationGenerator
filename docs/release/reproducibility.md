# Reproducibility

- Same binary + same input + same configuration ⇒ **byte-identical output**
  (pinned by `test_equivalence`: two runs, identical 524652-byte PNG).
- CLI and GUI call the same `run_job` pipeline; equivalent configurations
  produce equivalent analytical results by construction.
- Outputs are written atomically (temp file + rename): reruns and overwrites
  are idempotent, interrupted jobs leave no partial files.
- Batch mirrors the input tree under the output dir, so each file's result is
  independently verifiable.
