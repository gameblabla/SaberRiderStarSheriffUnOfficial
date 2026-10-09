/* The supplied SDK's copy-zp-data.S reverses TII source and destination.
 * SGX promotes initialized constants into zero page; copy their ROM image
 * into console RAM before main, without modifying the resident image. */
#ifdef PCE_SGX
.section .init.200,"ax",@progbits
.global __do_copy_zp_data
__do_copy_zp_data:
    .byte 0x73
    .word __zp_data_load_start, __zp_data_start, __zp_data_size
#endif
