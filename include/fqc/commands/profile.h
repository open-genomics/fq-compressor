// =============================================================================
// fq-compressor - Dataset Profile Detection
// =============================================================================

#pragma once

#include "fqc/common/error.h"
#include "fqc/common/types.h"
#include "fqc/format/archive.h"

#include <span>

namespace fqc::commands {

/// Infer a dataset profile from sampled records. Explicit header markers win;
/// otherwise short reads are Illumina and ENA/SRA/DDBJ run accessions on long
/// reads are ONT. Unmarked long reads fail closed.
[[nodiscard]] auto detectProfile(std::span<const ReadRecord> records)
    -> Result<format::DatasetProfile>;

}  // namespace fqc::commands
