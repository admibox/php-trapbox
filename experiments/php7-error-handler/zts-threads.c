/* Test-only embedded SAPI: one engine, concurrent requests in eight OS threads.
 * Main keeps its bootstrap request alive; hooks are initialized before threads.
 */
#include <sapi/embed/php_embed.h>
#include <Zend/zend_signal.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define THREAD_COUNT 8
#define REQUEST_COUNT 100
static pthread_barrier_t barrier;
static size_t discard_output(const char *data, size_t length) { return length; }

static void *run_thread(void *argument)
{
    long index = (long)(intptr_t)argument;
    ts_resource(0);
    ZEND_TSRMLS_CACHE_UPDATE();
    for (int iteration = 0; iteration < REQUEST_COUNT; iteration++) {
        pthread_barrier_wait(&barrier);
        if (php_request_startup() == FAILURE) abort();
        SG(headers_sent) = 1;
        SG(request_info).no_headers = 1;
        SG(options) |= SAPI_OPTION_NO_CHDIR;
        char thread[32], request[32];
        snprintf(thread, sizeof(thread), "%ld", index);
        snprintf(request, sizeof(request), "%d", iteration);
        zval value;
        ZVAL_STRING(&value, thread);
        zend_hash_str_update(&EG(symbol_table), "TRAPBOX_THREAD", sizeof("TRAPBOX_THREAD") - 1, &value);
        ZVAL_STRING(&value, request);
        zend_hash_str_update(&EG(symbol_table), "TRAPBOX_ITERATION", sizeof("TRAPBOX_ITERATION") - 1, &value);
        zend_first_try {
            zend_file_handle file = {0};
            file.type = ZEND_HANDLE_FILENAME;
            file.filename = "/ext/zts-thread-target.php";
            php_execute_script(&file);
        } zend_catch {
            /* exit/fatal terminate this request, not the hosting thread. */
        } zend_end_try();
        php_request_shutdown(NULL);
    }
    ts_free_thread();
    return NULL;
}

int main(int argc, char **argv)
{
#ifndef ZTS
#error This test requires a ZTS PHP build.
#endif
    php_embed_module.ub_write = discard_output;
    int cold = getenv("TRAPBOX_COLD_START") != NULL;
    if (cold) {
        /* Match a threaded SAPI: module startup on the main thread, but the
         * first RINIT occurs concurrently in workers. No bootstrap request. */
#if PHP_VERSION_ID >= 70400
        php_tsrm_startup();
#else
        tsrm_startup(1, 1, 0, NULL);
        (void)ts_resource(0);
        ZEND_TSRMLS_CACHE_UPDATE();
#endif
        zend_signal_startup();
        sapi_startup(&php_embed_module);
        php_embed_module.executable_location = argv[0];
        if (php_embed_module.startup(&php_embed_module) == FAILURE) return 1;
    } else if (php_embed_init(argc, argv) == FAILURE) return 1;
    pthread_t threads[THREAD_COUNT];
    if (pthread_barrier_init(&barrier, NULL, THREAD_COUNT) != 0) return 2;
    for (long i = 0; i < THREAD_COUNT; i++)
        if (pthread_create(&threads[i], NULL, run_thread, (void *)(intptr_t)i) != 0) abort();
    for (int i = 0; i < THREAD_COUNT; i++) pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&barrier);
    if (cold) {
        php_module_shutdown();
        sapi_shutdown();
        tsrm_shutdown();
    } else php_embed_shutdown();
    printf("Completed %d requests on %d concurrent threads\n", THREAD_COUNT * REQUEST_COUNT, THREAD_COUNT);
    return 0;
}
