# Format Governance Change Specification

## ADDED Requirements

### Requirement: Same-name format family coexistence is documented
The README SHALL state, near the first format description, that the two `fqc`/`.fqc`
implementations are same-name products with distinct format families, and SHALL NOT
describe them as versions of the same format.

#### Scenario: User opens the README first screen
- **GIVEN** a user viewing the README first screen
- **WHEN** the README first mentions `.fqc` or the archive format
- **THEN** it SHALL present a coexistence note covering repository, implementation
  language, format family ID and full magic for both implementations
- **AND** it SHALL NOT use "C++/Rust 版本" phrasing that implies the same format

#### Scenario: User reads about cross-implementation decode
- **WHEN** a user reads the format documentation
- **THEN** the documentation SHALL state that extension alone cannot determine the
  format, that the reader must check the magic, and that the two implementations
  cannot decode each other's archives

### Requirement: Same-name binary PATH risk is documented
The README SHALL warn that installing both implementations produces two binaries named
`fqc`, and that the one earlier in `PATH` wins.

#### Scenario: User installs both binaries
- **WHEN** a user follows the installation instructions for both implementations
- **THEN** the README SHALL remind about the same-name binary `PATH` overlap risk

### Requirement: Coexistence claims stay within implemented behavior
Documentation SHALL NOT claim cross-implementation automatic dispatch or family identity
exposure that is not yet implemented.

#### Scenario: User searches for automatic dispatch
- **WHEN** the documentation describes family rejection
- **THEN** it SHALL only describe the currently implemented rejection behavior
- **AND** it SHALL NOT claim automatic dispatch across implementations
