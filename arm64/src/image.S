/* the game's initialised data image (build/image.bin, made by gen/mkimage.py from scudrace.exe at build time) */
#ifdef __APPLE__
#define SYM(x) _##x
    .section __DATA,__const
#else
#define SYM(x) x
    .section .rodata
#endif
    .globl SYM(scud_image_blob), SYM(scud_image_blob_end)
    .p2align 4
SYM(scud_image_blob):
    .incbin IMAGE_BIN
SYM(scud_image_blob_end):
