/* libtextrel.c - a library with a text relocation (roadmap 1.1 P4): an
 * absolute address in .text, which the dynamic linker can only write by
 * making the text writable and executable at once. Linked -z notext; W^X
 * refuses it, so dlopen fails. */
int textrel_target = 1;
__asm__(".text\n.globl textrel_ptr\n.p2align 3\ntextrel_ptr:\n.quad textrel_target\n");
