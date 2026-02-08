// hello.S - COM64 payload (x86-64), entry symbol: com64_main
// int com64_main(DosApi* api, int argc, const char** argv)

    .text
    .globl com64_main
    .type  com64_main, @function

com64_main:
    // rdi = api
    // rsi = argc (unused)
    // rdx = argv (unused)

    // Load api->print function pointer (first field in struct)
    mov     (%rdi), %rax

    // Call print("Hello from COM64!\r\n")
    lea     msg(%rip), %rdi
    call    *%rax

    xor     %eax, %eax
    ret

    .section .rodata
msg:
    .asciz "Hello from COM64!\r\n"
