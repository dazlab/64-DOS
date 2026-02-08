#include <stdint.h>

typedef struct DosApi {
    void (*print)(const char* s);
} DosApi;

int com64_main(DosApi* api, int argc, const char** argv) {
    (void)argc; (void)argv;
    api->print("HELLO FROM C (not ASM)\r\n");
    return 0;
}
