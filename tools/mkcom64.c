// mkcom64.c - host tool to wrap a flat x86-64 binary into .COM64
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    char     magic[8];      // "64DOSCOM"
    uint32_t header_size;   // 64
    uint32_t flags;         // 0
    uint64_t entry_rva;     // from payload start
    uint64_t bss_size;      // bytes to zero after payload
    uint64_t reserved0;
    uint64_t reserved1;
    uint64_t reserved2;
} Com64Hdr;
#pragma pack(pop)

static void die(const char* msg) { fprintf(stderr, "%s\n", msg); exit(1); }

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <payload.bin> <out.COM64> [entry_rva] [bss_size]\n", argv[0]);
        return 2;
    }

    const char* in_path  = argv[1];
    const char* out_path = argv[2];
    uint64_t entry_rva = (argc >= 4) ? strtoull(argv[3], NULL, 0) : 0;
    uint64_t bss_size  = (argc >= 5) ? strtoull(argv[4], NULL, 0) : 0;

    FILE* in = fopen(in_path, "rb");
    if (!in) die("Failed to open input");

    if (fseek(in, 0, SEEK_END) != 0) die("fseek failed");
    long sz = ftell(in);
    if (sz < 0) die("ftell failed");
    if (fseek(in, 0, SEEK_SET) != 0) die("fseek failed");

    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) die("malloc failed");

    if (fread(buf, 1, (size_t)sz, in) != (size_t)sz) die("read failed");
    fclose(in);

    Com64Hdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "64DOSCOM", 8);
    h.header_size = 64;
    h.flags = 0;
    h.entry_rva = entry_rva;
    h.bss_size = bss_size;

    FILE* out = fopen(out_path, "wb");
    if (!out) die("Failed to open output");

    if (fwrite(&h, 1, sizeof(h), out) != sizeof(h)) die("write header failed");
    if (fwrite(buf, 1, (size_t)sz, out) != (size_t)sz) die("write payload failed");

    fclose(out);
    free(buf);
    return 0;
}
