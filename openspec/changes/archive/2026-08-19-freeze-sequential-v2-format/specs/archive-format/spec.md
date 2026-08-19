# Sequential Archive Format Change Specification

## ADDED Requirements

### Requirement: Sequential archive identity
The implementation SHALL identify its archive contract as `fqc-sequential/v2` with command `fqc`, extension `.fqc`, and magic `46 51 43 56 32 0D 0A 1A`.

#### Scenario: Reader opens sequential archive
- **GIVEN** an archive with the sequential magic and supported v2 header
- **WHEN** the C++ reader opens it
- **THEN** it SHALL parse the sequential header before frames

### Requirement: V2 structural layout is normative
The specification SHALL define the global header, frame header and footer sizes, field order, endian and checksum coverage implemented by the v2 reader.

#### Scenario: Inspect frozen archive
- **WHEN** a structure test parses a frozen v2 archive
- **THEN** its offsets and sizes SHALL match the normative layout

### Requirement: Unsupported structural fields fail closed
The reader SHALL reject unsupported version, flags, profile, codec or reserved values before decoding payload with an unintended interpretation.

#### Scenario: Unsupported codec
- **GIVEN** an otherwise valid header containing an unsupported codec value and recomputed header checksum
- **WHEN** the reader opens it
- **THEN** it SHALL return an unsupported format/codec error

### Requirement: Checksums and truncation are enforced
Header, frame and footer integrity checks and exact reads SHALL reject corruption or truncation with the corresponding error category.

#### Scenario: Truncated frame
- **GIVEN** a frozen archive truncated inside a frame
- **WHEN** it is decoded or verified
- **THEN** the operation SHALL fail
- **AND** SHALL NOT report a successful partial archive

### Requirement: Frozen reader compatibility
The repository SHALL retain deterministic SE and PE v2 archives from a known generator commit with manifests and expected decoded FASTQ.

#### Scenario: Future reader processes fixtures
- **WHEN** the standard test suite decodes every valid frozen archive
- **THEN** output reads and metadata SHALL match the manifest

### Requirement: Writer byte stability is scoped
The format contract SHALL distinguish stable layout/semantics from compression payload bytes that are not declared canonical.

#### Scenario: Compression library changes representation
- **WHEN** a future writer emits different payload bytes that conform to v2
- **THEN** compatibility SHALL be judged by normative fields and reader behavior unless canonical encoding is explicitly specified
