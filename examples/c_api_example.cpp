/*
 * EFP C API Example
 *
 * Demonstrates using EFP from C code.
 * This example matches the functionality of the old efp/efp_c_api/main.c
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

#define TEST_MTU 1456
#define TEST_DATA_SIZE 10000

/* Global receiver handle for loopback */
static efp_receiver_t g_receiver = NULL;

/* Context structure */
typedef struct {
    int frames_received;
    int embedded_received;
    int broken_frames;
    const char* name;
} my_context_t;

/* Send callback - forwards fragments to receiver */
static void on_send(const uint8_t* data, size_t size, uint8_t stream_id, void* ctx) {
    auto* c = (const my_context_t*)ctx;
    (void)c;
    (void)stream_id;

    if (g_receiver) {
        const int16_t result = efp_receiver_receive(g_receiver, data, size, 0);
        if (result < 0) {
            printf("Error receiving fragment: %d\n", result);
        }
    }
}

/* Receive callback - called when a superframe is assembled */
static void on_receive(uint8_t* data, size_t size,
                       uint8_t payload_type, uint8_t broken,
                       uint64_t pts, uint64_t dts,
                       uint32_t payload_code, uint8_t stream_id,
                       uint8_t source_id, uint8_t flags,
                       void* ctx) {
    auto* c = (my_context_t*)ctx;
    c->frames_received++;

    if (broken) {
        c->broken_frames++;
        printf("[%s] Received BROKEN frame (size=%zu, pts=%llu)\n",
               c->name, size, (unsigned long long)pts);
        return;
    }

    printf("[%s] Received frame:\n", c->name);
    printf("  Size: %zu bytes\n", size);
    printf("  Payload Type: 0x%02X\n", payload_type);
    printf("  PTS: %llu\n", (unsigned long long)pts);
    printf("  DTS: %llu\n", (unsigned long long)dts);
    printf("  Code: 0x%08X (%c%c%c%c)\n", payload_code,
           (char)(payload_code >> 24), (char)(payload_code >> 16),
           (char)(payload_code >> 8), (char)payload_code);
    printf("  Stream ID: %d\n", stream_id);
    printf("  Source ID: %d\n", source_id);
    printf("  Flags: 0x%02X\n", flags);

    /* Verify data integrity */
    int errors = 0;
    for (size_t i = 0; i < size && i < TEST_DATA_SIZE; i++) {
        if (data[i] != (uint8_t)i) {
            errors++;
        }
    }

    if (errors == 0) {
        printf("  Data integrity: OK\n");
    } else {
        printf("  Data integrity: %d errors!\n", errors);
    }

    (void)source_id;
}

/* Embedded data callback */
static void on_embedded(uint8_t* data, size_t size, uint8_t data_type,
                        uint64_t pts, void* ctx) {
    auto* c = (my_context_t*)ctx;
    c->embedded_received++;

    printf("[%s] Received embedded data:\n", c->name);
    printf("  Size: %zu bytes\n", size);
    printf("  Type: %d\n", data_type);
    printf("  PTS: %llu\n", (unsigned long long)pts);

    /* If it looks like a string, print it */
    if (size > 0 && data[size-1] == '\0') {
        printf("  Content: \"%s\"\n", (char*)data);
    }
}

int main(void) {
    printf("==============================================\n");
    printf("EFP C API Example\n");
    printf("==============================================\n");
    printf("Version: %d.%d\n\n", efp_version() >> 8, efp_version() & 0xFF);

    /* Create context */
    my_context_t ctx = {
        .frames_received = 0,
        .embedded_received = 0,
        .broken_frames = 0,
        .name = "Example"
    };

    /* ------------------------------------------------------------------ */
    /* Example 1: Basic send/receive using new API                        */
    /* ------------------------------------------------------------------ */
    printf("\n--- Example 1: Basic Send/Receive ---\n\n");

    /* Create sender and receiver */
    efp_sender_t sender = efp_sender_create(TEST_MTU);
    g_receiver = efp_receiver_create(100, 0, EFP_MODE_THREADED);

    if (!sender || !g_receiver) {
        printf("Failed to create sender or receiver!\n");
        return 1;
    }

    /* Set callbacks */
    efp_sender_set_callback(sender, on_send, &ctx);
    efp_receiver_set_callback(g_receiver, on_receive, &ctx);
    efp_receiver_set_embedded_callback(g_receiver, on_embedded, &ctx);

    /* Prepare test data */
    auto* test_data = (uint8_t*)malloc(TEST_DATA_SIZE);
    for (size_t i = 0; i < TEST_DATA_SIZE; i++) {
        test_data[i] = (uint8_t)i;
    }

    /* Send plain data */
    printf("Sending %d bytes of test data...\n", TEST_DATA_SIZE);

    int16_t result = efp_sender_send(sender, test_data, TEST_DATA_SIZE,
                                     0x83,  /* H.264 */
                                     1000,  /* PTS */
                                     1000,  /* DTS */
                                     EFP_CODE('A', 'N', 'X', 'B'),  /* Annex B */
                                     1,     /* Stream ID */
                                     EFP_FLAG_NONE);

    if (result != EFP_OK) {
        printf("Send failed with error: %d\n", result);
    }

    /* Wait for processing */
    SLEEP_MS(100);

    /* ------------------------------------------------------------------ */
    /* Example 2: Send with embedded data                                 */
    /* ------------------------------------------------------------------ */
    printf("\n--- Example 2: Embedded Data ---\n\n");

    const char* metadata = "This is embedded metadata!";
    const size_t metadata_len = strlen(metadata) + 1;

    /* Calculate buffer size needed */
    const size_t buffer_size = efp_add_embedded_data(NULL, (uint8_t*)metadata, test_data,
                                                metadata_len, TEST_DATA_SIZE,
                                                EFP_EMBEDDED_PRIVATE_DATA, 1);

    printf("Total buffer size with embedded data: %zu bytes\n", buffer_size);

    /* Allocate and build buffer */
    auto* send_buffer = (uint8_t*)malloc(buffer_size);
    efp_add_embedded_data(send_buffer, (uint8_t*)metadata, test_data,
                          metadata_len, TEST_DATA_SIZE,
                          EFP_EMBEDDED_PRIVATE_DATA, 1);

    /* Send with inline payload flag */
    printf("Sending data with embedded metadata...\n");

    result = efp_sender_send(sender, send_buffer, buffer_size,
                             0x83,
                             2000,
                             2000,
                             EFP_CODE('A', 'N', 'X', 'B'),
                             1,
                             EFP_FLAG_INLINE_PAYLOAD);

    if (result != EFP_OK) {
        printf("Send failed with error: %d\n", result);
    }

    /* Wait for processing */
    SLEEP_MS(100);

    /* ------------------------------------------------------------------ */
    /* Example 3: Multiple streams                                        */
    /* ------------------------------------------------------------------ */
    printf("\n--- Example 3: Multiple Streams ---\n\n");

    /* Send video on stream 1 */
    printf("Sending video on stream 1...\n");
    (void)efp_sender_send(sender, test_data, 5000,
                             0x83, 3000, 3000,
                             EFP_CODE('A', 'N', 'X', 'B'),
                             1, EFP_FLAG_NONE);

    /* Send audio on stream 2 */
    printf("Sending audio on stream 2...\n");
    (void)efp_sender_send(sender, test_data, 1000,
                             0x88, 3000, 3000,
                             EFP_CODE('A', 'D', 'T', 'S'),
                             2, EFP_FLAG_NONE);

    /* Send metadata on stream 3 */
    printf("Sending metadata on stream 3...\n");
    (void)efp_sender_send(sender, test_data, 100,
                             0x0a, 3000, 3000, 0,
                             3, EFP_FLAG_NONE);

    /* Wait for processing */
    SLEEP_MS(100);

    /* ------------------------------------------------------------------ */
    /* Cleanup                                                            */
    /* ------------------------------------------------------------------ */
    printf("\n--- Cleanup ---\n\n");

    efp_sender_destroy(sender);
    efp_receiver_destroy(g_receiver);
    g_receiver = NULL;

    free(test_data);
    free(send_buffer);

    /* ------------------------------------------------------------------ */
    /* Results                                                            */
    /* ------------------------------------------------------------------ */
    printf("==============================================\n");
    printf("Results:\n");
    printf("  Frames received: %d\n", ctx.frames_received);
    printf("  Embedded data received: %d\n", ctx.embedded_received);
    printf("  Broken frames: %d\n", ctx.broken_frames);
    printf("==============================================\n");

    return 0;
}

