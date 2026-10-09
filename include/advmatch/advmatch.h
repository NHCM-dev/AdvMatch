#ifndef ADVMATCH_ADVMATCH_H
#define ADVMATCH_ADVMATCH_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(advmatch_EXPORTS)
#    define ADVMATCH_API __declspec(dllexport)
#  else
#    define ADVMATCH_API __declspec(dllimport)
#  endif
#else
#  define ADVMATCH_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ABI 4 extends fee/risk views, audit reads and persistence configuration.
 * Binding packets use schema 1; persisted operation codes are append-only.
 * Every export catches C++ exceptions. Error messages are UTF-8 and thread-local.
 * Handles are validated IDs, never addresses. Status: 0=success, 1=pending,
 * -1=error, -2=cancelled, -3=queue full (command was not executed).
 * Copy last_error before another call on this thread.
 * poll/slot_load return malloc-owned buffers; free each with buffer_free.
 * A successful poll does not release its ticket. Release it after copying.
 * destroy drains accepted commands; callers must stop their pollers first.
 */
ADVMATCH_API int advmatch_abi_version(void);
ADVMATCH_API const char* advmatch_last_error(void);
ADVMATCH_API int advmatch_exchange_create(const uint8_t* config, size_t size, uint64_t* exchange);
ADVMATCH_API int advmatch_exchange_submit(uint64_t exchange, const uint8_t* command, size_t size, uint64_t* request);
/* Batch packet: u32 count, repeated u32 size + schema-1 command bytes.
 * count is 1..1024. Validation is all-or-none; execution is ordered, not atomic.
 * requests capacity must equal count. Queue rejection completes every ticket.
 */
ADVMATCH_API int advmatch_exchange_submit_batch(uint64_t exchange, const uint8_t* commands, size_t size, uint64_t* requests, size_t count);
/* Removes completed non-close requests after building the entire output.
 * Packet: u32 count, repeated u64 ticket, i32 status, u32 payload size, payload.
 * Payload is a schema-1 response on success, UTF-8 error bytes on failure.
 * Do not mix drain and per-ticket poll/release on the same session.
 * Empty queue returns 1. buffer_free owns successful output as with poll.
 */
ADVMATCH_API int advmatch_exchange_drain(uint64_t exchange, size_t limit, uint8_t** data, size_t* size);
/* Waits for a non-close completion; 0=ready, 1=timeout. timeout is 0..100 ms.
 * Does not consume results. Stop waiting before destroying the session.
 */
ADVMATCH_API int advmatch_exchange_await(uint64_t exchange, unsigned int timeoutMillis);
ADVMATCH_API int advmatch_exchange_close(uint64_t exchange, uint64_t* request);
ADVMATCH_API int advmatch_exchange_poll(uint64_t exchange, uint64_t request, uint8_t** data, size_t* size);
ADVMATCH_API int advmatch_exchange_cancel(uint64_t exchange, uint64_t request, int* cancelled);
ADVMATCH_API int advmatch_request_release(uint64_t exchange, uint64_t request);
ADVMATCH_API int advmatch_slot_load(uint64_t exchange, uint64_t slot, uint8_t** data, size_t* size);
ADVMATCH_API int advmatch_slot_release(uint64_t exchange, uint64_t slot);
ADVMATCH_API int advmatch_exchange_destroy(uint64_t exchange);
ADVMATCH_API void advmatch_buffer_free(void* buffer);

#ifdef __cplusplus
}
#endif

#endif
