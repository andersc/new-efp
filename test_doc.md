# EFP Test Documentation

This document describes the test suites and test cases for the Elastic Frame Protocol (EFP) library.

## Test Suites

### Bandwidth Manager Tests (`test_bandwidth_manager.cpp`)

Tests for the `BandwidthManager` class that provides congestion-aware streaming with per-stream bandwidth controls.

| Test Case | Description |
|-----------|-------------|
| BandwidthManager can be instantiated | Verifies basic construction and initial state |
| Per-stream configuration | Tests `setStreamConfig()` and `getStreamConfig()` for individual streams |
| Default stream multiplier is 1.0 | Confirms all streams start at full bandwidth |
| Stream is not dropped by default | Verifies no streams are dropped initially |
| Send passes through to underlying sender | Tests that `send()` invokes the underlying Sender |
| Send with vector overload | Tests the `std::vector` convenience overload |
| NACK processing updates NACK count | Verifies NACK processing updates statistics |
| RTT probe structure sizes | Validates `FrameType0RttProbe` (12 bytes) and `FrameType0RttResponse` (20 bytes) |
| Type0Subtype RTT enum values | Verifies RTT_PROBE=0x02 and RTT_RESPONSE=0x03 |
| Build RTT probe | Tests probe construction with incrementing sequence numbers |
| Build RTT response from probe | Tests response generation echoing probe data |
| Process RTT response updates RTT estimate | Verifies RTT measurement and statistics update |
| Manual multiplier overrides automatic | Tests `setManualMultiplier()` functionality |
| Clear manual multiplier returns to automatic | Tests `clearManualMultiplier()` behavior |
| Manual multiplier is clamped to stream config | Verifies multiplier respects min/max bounds |
| Statistics track probe counts | Tests probe statistics tracking |
| Statistics track bandwidth changes | Tests bandwidth change statistics |
| Jitter updates affect health evaluation | Tests jitter-based congestion detection |
| Access to underlying sender | Verifies `getSender()` provides access to underlying Sender |
| Sender statistics accessible | Tests `getSenderStatistics()` method |
| Custom BandwidthManagerConfig | Tests custom configuration parameters |
| Video and audio stream configuration example | Integration test with typical video/audio setup |

### NACK and Retransmission Tests (`test_nack.cpp`)

Tests for Type0 NACK frames and the retransmission mechanism.

| Test Case | Description |
|-----------|-------------|
| NACK frame structure sizes | Validates frame header sizes |
| Type0Subtype enum values | Verifies NACK subtype value |
| Sender processes valid NACK and queues retransmit | Tests NACK handling and retransmit queue |
| Sender rejects NACK with wrong frame type | Validates frame type checking |
| Sender rejects NACK with wrong subtype | Validates subtype checking |
| Sender rejects NACK that's too small | Validates size checking |
| NACK for non-existent fragment is ignored gracefully | Tests graceful handling of invalid NACKs |
| Batched NACK with multiple entries | Tests multiple fragments in one NACK |
| NACK with consecutive fragment range | Tests fragment range coalescing |
| Full round-trip: NACK triggers retransmit and frame completes | End-to-end retransmit test |
| processRetransmits invokes callback with byte-identical data | Verifies retransmit data integrity |
| processRetransmits respects aMaxCount limit | Tests rate limiting |
| processRetransmits on empty queue returns zero | Tests empty queue behavior |
| processRetransmits skips evicted fragments gracefully | Tests retention eviction handling |

### Bundle (Type4) Tests (`test_bundle.cpp`)

Tests for Type4 bundle frames that contain multiple fragments.

### Basic Tests (`test_basic.cpp`)

Core functionality tests for send/receive operations.

### Edge Cases Tests (`test_edge_cases.cpp`)

Boundary condition and error handling tests.

### Fragment Ordering Tests (`test_fragment_ordering.cpp`)

Tests for out-of-order fragment reassembly.

### Integration Tests (`test_integration.cpp`)

End-to-end integration tests combining sender and receiver.

### Lifecycle Tests (`test_lifecycle.cpp`)

Tests for start/stop, restart, and cleanup scenarios.

### Packet Loss Tests (`test_packet_loss.cpp`)

Tests for handling dropped/lost packets.

### Receiver Tests (`test_receiver.cpp`)

Receiver-specific functionality tests.

### Sender Tests (`test_sender.cpp`)

Sender-specific functionality tests.

### Stress Tests (`test_stress.cpp`)

High-volume and performance tests.

### Embedded Data Tests (`test_embedded_data.cpp`)

Tests for inline/embedded payload functionality.

### C API Tests (`test_c_api.cpp`)

Tests for the C language API wrapper.

## Running Tests

### Build and Run All Tests

```bash
cd build
cmake -DEFP_BUILD_TESTS=ON ..
make
ctest --output-on-failure
```

### Run Specific Test Suite

```bash
# Run only Bandwidth Manager tests
./efp_tests -ts="Bandwidth Manager"

# Run only NACK tests
./efp_tests -ts="NACK and Retransmission"

# Run with verbose output
./efp_tests -s
```

### Run Single Test Case

```bash
./efp_tests -tc="BandwidthManager can be instantiated"
```

## Test Framework

Tests use [doctest](https://github.com/doctest/doctest) v2.4.11, a lightweight C++ testing framework with:

- `TEST_SUITE` for grouping related tests
- `TEST_CASE` for individual test cases
- `CHECK` for non-fatal assertions
- `REQUIRE` for fatal assertions
- `doctest::Approx` for floating-point comparisons

## Adding New Tests

When adding new tests:

1. Create test cases in the appropriate test file or create a new file
2. Add the file to `CMakeLists.txt` in the `efp_tests` target
3. Document the tests in this file (`test_doc.md`)
4. Ensure tests follow the project's code style (see `codestyle.txt`)

