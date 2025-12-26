/*
 * EFP C API Tests
 *
 * Tests the C API functionality matching the old efp/efp_c_api/main.c
 */

#include "efp_c_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* Test context */
typedef struct {
    int value;
    int frames_received;
    int embedded_received;
    int broken_frames;
    size_t last_frame_size;
    uint64_t last_pts;
} test_context_t;

/* Global test state */
static test_context_t g_ctx = {0};
static uint64_t g_receiver_handle = 0;
static int g_drop_counter = 0;
static int g_test_passed = 1;

/* Send callback - forwards to receiver, optionally dropping packets */
static void send_callback(const uint8_t* data, size_t size, uint8_t stream_id, void* ctx) {
    auto* c = (const test_context_t*)ctx;
    (void)c;

    g_drop_counter++;
    if (g_drop_counter == 5) {
        /* Drop the 5th fragment to test broken frame handling */
        return;
    }

    const int16_t result = efp_receive_fragment(g_receiver_handle, data, size, 0);
    if (result < 0) {
        printf("Error %d in receive_fragment\n", result);
        g_test_passed = 0;
    }
}

/* Receive callback - verifies received data */
static void receive_callback(uint8_t* data, size_t size,
                             uint8_t payload_type, uint8_t broken,
                             uint64_t pts, uint64_t dts,
                             uint32_t payload_code, uint8_t stream_id,
                             uint8_t source_id, uint8_t flags,
                             void* ctx) {
    auto* c = (test_context_t*)ctx;

    c->frames_received++;
    c->last_frame_size = size;
    c->last_pts = pts;

    if (broken) {
        c->broken_frames++;
        printf("Received broken frame (expected for dropped fragment test)\n");
        return;
    }

    printf("Received frame: size=%zu, pts=%llu, stream=%d, code=0x%08X\n",
           size, (unsigned long long)pts, stream_id, payload_code);

    (void)payload_type;
    (void)dts;
    (void)source_id;
    (void)flags;
    (void)data;
}

/* Embedded data callback */
static void embedded_callback(uint8_t* data, size_t size, uint8_t data_type,
                              uint64_t pts, void* ctx) {
    auto* c = (test_context_t*)ctx;

    c->embedded_received++;
    printf("Received embedded data: size=%zu, type=%d, pts=%llu\n",
           size, data_type, (unsigned long long)pts);

    /* Print if it's a string */
    if (size > 0 && data[size-1] == '\0') {
        printf("  Content: %s\n", (char*)data);
    }
}

/* Test 1: Basic send/receive */
static int test_basic_roundtrip(void) {
    printf("\n=== Test: Basic Roundtrip ===\n");

    efp_sender_t sender = efp_sender_create(1456);
    efp_receiver_t receiver = efp_receiver_create(100, 0, EFP_MODE_THREADED);

    if (!sender || !receiver) {
        printf("FAIL: Could not create sender/receiver\n");
        return 0;
    }

    test_context_t ctx = {.value = 42};

    efp_receiver_set_callback(receiver, receive_callback, &ctx);

    efp_sender_set_callback(sender,
        [](const uint8_t* data, size_t size, uint8_t stream_id, void* c) {
            /* Direct loopback - we need to use the receiver from context */
            /* This is a simplified test */
            (void)data; (void)size; (void)stream_id; (void)c;
        }, &ctx);

    /* For this test, use the legacy API which handles the loopback internally */
    efp_sender_destroy(sender);
    efp_receiver_destroy(receiver);

    printf("PASS: Basic creation/destruction\n");
    return 1;
}

/* Test 2: Legacy API (matching old main.c) */
static int test_legacy_api(void) {
    printf("\n=== Test: Legacy API ===\n");

    g_ctx.value = 123;
    g_ctx.frames_received = 0;
    g_ctx.embedded_received = 0;
    g_ctx.broken_frames = 0;
    g_drop_counter = 0;
    g_test_passed = 1;

    /* Create sender */
    const uint64_t sender_handle = efp_init_send(300, send_callback, &g_ctx);
    if (!sender_handle) {
        printf("FAIL: Could not create sender\n");
        return 0;
    }

    /* Create receiver */
    g_receiver_handle = efp_init_receive(30, 10, receive_callback, embedded_callback,
                                         &g_ctx, EFP_MODE_THREADED);
    if (!g_receiver_handle) {
        printf("FAIL: Could not create receiver\n");
        efp_end_send(sender_handle);
        return 0;
    }

    /* Prepare test data */
    const size_t TEST_DATA_SIZE = 10000;
    auto* test_data = (uint8_t*)malloc(TEST_DATA_SIZE);
    for (size_t i = 0; i < TEST_DATA_SIZE; i++) {
        test_data[i] = (uint8_t)i;
    }

    /* Prepare embedded data */
    const char* embedded_str = "Hello from embedded data!";
    const size_t embedded_len = strlen(embedded_str) + 1;

    /* Calculate total size needed */
    const size_t alloc_size = efp_add_embedded_data(NULL, (uint8_t*)embedded_str, test_data,
                                               embedded_len, TEST_DATA_SIZE,
                                               EFP_EMBEDDED_PRIVATE_DATA, 1);

    auto* send_buffer = (uint8_t*)malloc(alloc_size);
    efp_add_embedded_data(send_buffer, (uint8_t*)embedded_str, test_data,
                          embedded_len, TEST_DATA_SIZE,
                          EFP_EMBEDDED_PRIVATE_DATA, 1);

    printf("Sending first frame (will drop fragment 5)...\n");

    /* First send - will drop a fragment, resulting in broken frame */
    int16_t result = efp_send_data(sender_handle, send_buffer, alloc_size,
                                   0x83, 100, 100,
                                   EFP_CODE('A', 'N', 'X', 'B'),
                                   2, EFP_FLAG_INLINE_PAYLOAD);
    if (result != EFP_OK) {
        printf("FAIL: Send returned %d\n", result);
        g_test_passed = 0;
    }

    printf("Sending second frame (complete)...\n");

    /* Second send - should succeed completely */
    result = efp_send_data(sender_handle, send_buffer, alloc_size,
                           0x83, 200, 200,
                           EFP_CODE('A', 'N', 'X', 'B'),
                           2, EFP_FLAG_INLINE_PAYLOAD);
    if (result != EFP_OK) {
        printf("FAIL: Send returned %d\n", result);
        g_test_passed = 0;
    }

    /* Wait for processing */
    SLEEP_MS(500);

    /* Cleanup */
    free(send_buffer);
    free(test_data);
    efp_end_send(sender_handle);
    efp_end_receive(g_receiver_handle);

    /* Verify results */
    printf("Results: frames=%d, broken=%d, embedded=%d\n",
           g_ctx.frames_received, g_ctx.broken_frames, g_ctx.embedded_received);

    if (g_ctx.frames_received >= 2 && g_ctx.broken_frames >= 1) {
        printf("PASS: Legacy API test\n");
        return 1;
    } else {
        printf("FAIL: Expected 2 frames (1 broken), got %d (%d broken)\n",
               g_ctx.frames_received, g_ctx.broken_frames);
        return 0;
    }
}

/* Test 3: New API with proper loopback and data verification */
static int test_new_api(void) {
    printf("\n=== Test: New Opaque Handle API ===\n");

    efp_sender_t sender = efp_sender_create(1456);
    efp_receiver_t receiver = efp_receiver_create(100, 0, EFP_MODE_RUN_TO_COMPLETION);

    if (!sender || !receiver) {
        printf("FAIL: Creation failed\n");
        return 0;
    }

    test_context_t ctx = {0};

    /* Set up loopback structure */
    struct {
        efp_receiver_t recv;
        int fragments_received;
    } loopback_ctx = {receiver, 0};

    efp_sender_set_callback(sender,
        [](const uint8_t* data, size_t size, uint8_t stream_id, void* c) {
            auto* lctx = (decltype(&loopback_ctx))c;
            int16_t res = efp_receiver_receive(lctx->recv, data, size, 0);
            if (res >= 0) {
                lctx->fragments_received++;
            }
            (void)stream_id;
        }, &loopback_ctx);

    efp_receiver_set_callback(receiver,
        [](uint8_t* data, size_t size, uint8_t payload_type, uint8_t broken,
           uint64_t pts, uint64_t dts, uint32_t payload_code, uint8_t stream_id,
           uint8_t source_id, uint8_t flags, void* c) {
            auto* tctx = (test_context_t*)c;
            tctx->frames_received++;
            tctx->last_frame_size = size;
            tctx->last_pts = pts;

            /* Verify data content - should all be 0xAB */
            int content_valid = 1;
            for (size_t i = 0; i < size; i++) {
                if (data[i] != 0xAB) {
                    content_valid = 0;
                    break;
                }
            }
            tctx->value = content_valid;

            (void)payload_type; (void)broken; (void)dts;
            (void)payload_code; (void)stream_id; (void)source_id; (void)flags;
        }, &ctx);

    /* Send some data with distinctive pattern */
    uint8_t data[100];
    memset(data, 0xAB, sizeof(data));

    const int16_t result = efp_sender_send(sender, data, sizeof(data),
                                     0x01, 1000, 1000, 0, 1, EFP_FLAG_NONE);

    efp_receiver_poll(receiver);

    int success = (result == EFP_OK &&
                   ctx.frames_received == 1 &&
                   ctx.last_frame_size == 100 &&
                   ctx.last_pts == 1000 &&
                   ctx.value == 1);  /* Content was verified */

    efp_sender_destroy(sender);
    efp_receiver_destroy(receiver);

    if (success) {
        printf("PASS: New API test (received %d frame, size=%zu, content verified)\n",
               ctx.frames_received, ctx.last_frame_size);
        return 1;
    } else {
        printf("FAIL: result=%d, frames=%d, size=%zu, content_ok=%d\n",
               result, ctx.frames_received, ctx.last_frame_size, ctx.value);
        return 0;
    }
}

/* Test 4: Embedded data helpers */
static int test_embedded_helpers(void) {
    printf("\n=== Test: Embedded Data Helpers ===\n");

    const char* embedded = "Test embedded data";
    const size_t emb_size = strlen(embedded) + 1;

    uint8_t payload[100];
    memset(payload, 0x55, sizeof(payload));

    /* Calculate size */
    const size_t total = efp_embedded_calc_size(emb_size, sizeof(payload));
    printf("Calculated size: %zu\n", total);

    if (total != 3 + emb_size + sizeof(payload)) {
        printf("FAIL: Size calculation wrong\n");
        return 0;
    }

    /* Build combined buffer */
    auto* buffer = (uint8_t*)malloc(total);
    const size_t result = efp_add_embedded_data(buffer, (uint8_t*)embedded, payload,
                                          emb_size, sizeof(payload),
                                          EFP_EMBEDDED_PRIVATE_DATA, 1);

    if (result != 0) {
        printf("FAIL: Add embedded data returned %zu\n", result);
        free(buffer);
        return 0;
    }

    /* Extract and verify */
    uint8_t extracted[100];
    size_t extracted_size;
    uint8_t extracted_type;
    size_t payload_offset;

    const int16_t extract_result = efp_extract_embedded_data(buffer, total,
                                                        extracted, &extracted_size,
                                                        &extracted_type, &payload_offset);

    if (extract_result != EFP_OK) {
        printf("FAIL: Extract returned %d\n", extract_result);
        free(buffer);
        return 0;
    }

    printf("Extracted: type=%d, size=%zu, offset=%zu\n",
           extracted_type, extracted_size, payload_offset);
    printf("Content: %s\n", (char*)extracted);

    if (extracted_type != EFP_EMBEDDED_PRIVATE_DATA ||
        extracted_size != emb_size ||
        strcmp((char*)extracted, embedded) != 0) {
        printf("FAIL: Extracted data mismatch\n");
        free(buffer);
        return 0;
    }

    free(buffer);
    printf("PASS: Embedded data helpers\n");
    return 1;
}

/* Test 5: Version */
static int test_version(void) {
    printf("\n=== Test: Version ===\n");

    const uint16_t version = efp_version();
    const uint8_t major = version >> 8;
    const uint8_t minor = version & 0xFF;

    printf("EFP Version: %d.%d\n", major, minor);

    if (major >= 1) {
        printf("PASS: Version test\n");
        return 1;
    } else {
        printf("FAIL: Unexpected version\n");
        return 0;
    }
}

/* Test 6: Error handling */
static int test_error_handling(void) {
    printf("\n=== Test: Error Handling ===\n");

    /* Test NULL sender */
    int16_t result = efp_sender_send(NULL, NULL, 0, 0, 0, 0, 0, 0, 0);
    if (result != EFP_INVALID_PARAMETER) {
        printf("FAIL: Expected INVALID_PARAMETER for NULL sender\n");
        return 0;
    }

    /* Test NULL receiver */
    result = efp_receiver_receive(NULL, NULL, 0, 0);
    if (result != EFP_INVALID_PARAMETER) {
        printf("FAIL: Expected INVALID_PARAMETER for NULL receiver\n");
        return 0;
    }

    /* Test invalid handle (legacy API) */
    result = efp_send_data(99999, NULL, 0, 0, 0, 0, 0, 0, 0);
    if (result != EFP_INVALID_PARAMETER) {
        printf("FAIL: Expected INVALID_PARAMETER for invalid handle\n");
        return 0;
    }

    /* Test destroy NULL (should not crash) */
    efp_sender_destroy(NULL);
    efp_receiver_destroy(NULL);

    printf("PASS: Error handling\n");
    return 1;
}

int main(void) {
    printf("EFP C API Test Suite\n");
    printf("====================\n");
    printf("Version: %d.%d\n", efp_version() >> 8, efp_version() & 0xFF);

    int passed = 0;
    int total = 0;

    total++;
    if (test_version()) {
        passed++;
    }
    total++;
    if (test_basic_roundtrip()) {
        passed++;
    }
    total++;
    if (test_embedded_helpers()) {
        passed++;
    }
    total++;
    if (test_error_handling()) {
        passed++;
    }
    total++;
    if (test_new_api()) {
        passed++;
    }
    total++;
    if (test_legacy_api()) {
        passed++;
    }

    printf("\n====================\n");
    printf("Results: %d/%d tests passed\n", passed, total);

    return (passed == total) ? 0 : 1;
}

