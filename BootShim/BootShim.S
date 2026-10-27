/*
 * Android boot shim: move the appended UEFI firmware volume to its link address
 * and jump to it.
 *
 * The bootloader loads the kernel (this shim plus the volume) at the address
 * the boot image names, which is not where the volume was built to live, so
 * the first thing this code does is copy the volume into place.
 *
 * BAO_PROBE=1 turns the shim into a boot-stage reporter. A retail phone has no
 * serial console, so each stage paints the framebuffer a solid colour before
 * running the next one. The framebuffer is whatever the bootloader left on
 * screen (the splash region from the device tree), written as pixels:
 *
 *   red    shim entered
 *   green  volume copied, about to jump into it
 *
 * No colour at all means the bootloader never reached the shim.
 */
  .equ PROBE_COLOUR_ENTER, 0x00ff0000
  .equ PROBE_COLOUR_JUMP,  0x0000ff00
  .equ PROBE_FB_BASE,      0xa0000000     /* cont_splash_region (device tree) */
  .equ PROBE_PIXELS,       0x00280000     /* 1080*2400 pixels, rounded up */

_Head:
  adr   x1, _Payload    /* Set _Entry address */
  b     _Start          /* Jump to the real code */

#if REQUIRES_KERNEL_HEADER == 1
_StackBase:
  .quad FD_BASE         /* Text Offset */

_StackSize:
  .quad FD_SIZE         /* Image Size */
  .quad 0               /* Flags */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .ascii "ARM\x64"      /* ARM64 Magic */
  .long 0               /* Reserved */
#endif

_Start:
  mov   x4, x1

  //
  // Hand the ramdisk's address to the firmware in x0.
  //
  // Not the boot image's base: that was tried, the arithmetic checked out against the
  // measured _Payload (base 0xA247F000), and the first word there was 0xFFCDFFDD rather than
  // "ANDRO". ABL decompresses the kernel and never leaves the original image behind, so the
  // Android header is not in memory at all and cannot be found this way.
  //
  // What is known is where the kernel ends. The kernel region is this shim followed by the
  // firmware volume - one contiguous blob, one gzip stream - so after decompression the
  // ramdisk, which the boot image puts after the kernel, follows the volume. _Payload marks
  // the volume's first byte, FD_SIZE is its length, and the sum is the first byte past it.
  //
  // Whether ABL actually leaves the ramdisk there is the question this answers: the firmware
  // reads the candidate and reports what it found.
  //
  // ABL puts a device tree pointer in x0 and every stage so far has found that pointer
  // unreadable, so nothing is lost by replacing it.
  //
  mov   x19, x1              /* x19 = _Payload: first byte past the shim */
  ldr   x9,  =FD_SIZE
  add   x19, x19, x9         /* x19 = first byte past the kernel         */
#if BAO_PROBE == 1
  ldr   x7, =PROBE_COLOUR_ENTER
  bl    _Paint
#endif
  ldr   x5, =FD_BASE
  cmp   x4, x5
  beq   _Entry
  ldr   x6,  =FD_SIZE

_CopyLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyLoop
  ldr   x5, =FD_BASE

_Entry:
#if BAO_PROBE == 1
  ldr   x7, =PROBE_COLOUR_JUMP
  bl    _Paint
#endif
  mov   x0, x19              /* boot image base, in place of ABL's device tree pointer */
  br    x5

_Dead:
  b     _Dead

#if BAO_PROBE == 1
_Paint:
  ldr   x8, =PROBE_FB_BASE
  ldr   x9, =PROBE_PIXELS
1:
  str   w7, [x8], #4
  subs  x9, x9, #1
  b.ne  1b
  ret
#endif

.text
.align 4

_Payload: