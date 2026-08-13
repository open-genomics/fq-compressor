# Verification: recognize-indexed-fqc-family

- Status: `Completed`
- Date: 2026-08-13

| Case | Evidence | Result |
|---|---|---|
| Own sequential fixture | `AcceptsOwnSequentialFrozenFixture` | passed |
| Indexed family reject | `RejectsIndexedFrozenFixtureAsKnownFamily` | passed |
| Unknown / truncated | `RejectsUnknownMagic` / `RejectsTruncatedMagic` | passed |
| Verify/decompress | `VerifyAndDecompressRejectIndexedWithoutOutput` | passed |

Commands: `./scripts/build.sh clang-debug`, `./scripts/test.sh clang-debug` — 16/16 passed.
