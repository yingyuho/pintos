#include "threads/loader.h"

#### Kernel loader.

#### This code should be stored in the first sector of a hard disk.
#### When the BIOS runs, it loads this code at physical address
#### 0x7c00-0x7e00 (512 bytes) and jumps to the beginning of it,
#### in real mode.  The loader loads the kernel into memory and jumps
#### to its entry point, which is the start function in start.S.
####
#### The BIOS passes in the drive that the loader was read from as
#### DL, with floppy drives numbered 0x00, 0x01, ... and hard drives
#### numbered 0x80, 0x81, ...  We want to support booting a kernel on
#### a different drive from the loader, so we don't take advantage of
#### this.

# Runs in real mode, which is a 16-bit segment.
	.code16

# Set up segment registers.
# Set stack to grow downward from 60 kB (after boot, the kernel
# continues to use this stack for its initial thread).

	sub %ax, %ax
	mov %ax, %ds
	mov %ax, %ss
	mov $0xf000, %esp

# Configure serial port so we can report progress without connected VGA.
# See [IntrList] for details.
	sub %dx, %dx			# Serial port 0.
	mov $0xe3, %al			# 9600 bps, N-8-1.
					# AH is already 0 (Initialize Port).
	int $0x14			# Destroys AX.

	call puts
	.string "PiLo"

#### TODO:  ITERATE THROUGH ALL HARD DISKS IN THE SYSTEM, LOOKING FOR A
####        BOOTABLE PINTOS PARTITION TO LOAD AND START.
####
####        Note that there are only ~300 bytes available for the entire
####        Pintos bootloader, as Pintos commands are stored in the
####        region immediately before the partition table in the sector.
####        Sooo... be concise!
####
####        If you want to create helper routines, there is a TODO toward
####        the end of this file that shows where they should go.

# Iterate through the drives. int 13h AH=42h wants the drive
# index in %dl, so we might as well put it there.
# We start %dl at 0x7F to simplify some code later.

	mov $0x42, %ah
	mov $0x7f, %dl
# Build a thing in memory (it's more or less irrelevant where we put it,
# since it's not like there's anyone else's data for us to trash)
	xor %cx, %cx
	mov %cx, %es
	mov $0x7e00, %si
	
# The struct starts at 0x7E00 and takes up 16 bytes.
# The first two bytes should be 10h and 00h respectively
	movb $0x10, 0x7e00
	movb $0, 0x7e01
# Next two bytes are the number of sectors (so 1 here)
	movw $0x0001, 0x7e02
# Next four bytes are segment:offset for the place to read into
# We might as well just put it at 0x7E10
	movl $0x00007e10, 0x7e04

# And the rest are just zero because we want to start at the first
	movl $0, 0x7e08
	movl $0, 0x7e0c

# We can reuse this struct for every hard disk of course
# Since we're doing the same thing to every disk
	
loop:	inc %dl
#	call puts
#	.string "\r Checking hard drive "
#	mov %dl, %al
#	sub $0x50, %al
#	call putc
	
	int $0x13

# Check for error. If there is an error, we are out of hard drives
# Error is signalled on the carry flag.
	jc end
# Otherwise check whether the drive is bootable (ends with 0x55 0xAA)
# Since the sector starts at 0x7E10, we need to look at 0x800E and 0x800F
# But in the other order (endianness yay)
	
	testb $0xaa, (0x800e)
	jne loop
	testb $0x55, (0x800f)
	jne loop

# Drive is bootable. Print a message to that effect
	call puts
	.string "\r\rBootable drive found\r"

# Do something here (find a bootable partition, load a kernel, etc)

	

end:	call puts
	.string "\rNo bootable drives found\r"
# Print some sort of error message maybe?
	int $0x18

	# Stop the processor.
	hlt

#### TODO:  LOADING CODE ENDS HERE!

#### Transfer control to the kernel that we loaded.  We read the start
#### address out of the ELF header (see [ELF1]) and convert it from a
#### 32-bit linear address into a 16:16 segment:offset address for
#### real mode, then jump to the converted address.  The 80x86 doesn't
#### have an instruction to jump to an absolute segment:offset kept in
#### registers, so in fact we store the address in a temporary memory
#### location, then jump indirectly through that location.  To save 4
#### bytes in the loader, we reuse 4 bytes of the loader's code for
#### this temporary pointer.

	mov $0x2000, %ax
	mov %ax, %es
	mov %es:0x18, %dx
	mov %dx, start
	movw $0x2000, start + 2
	ljmp *start

#### The following label is a placeholder for the preceding code to use
#### to set up for the jump to the Pintos kernel.  Of course, the start
#### label also points to the puts function; when the previous code runs,
#### it will clobber a few of the bytes at the start of the puts function,
#### but hey we're done bootloading, so we don't need it anymore.

start:

#### Print string subroutine.  To save space in the loader, this
#### subroutine takes its null-terminated string argument from the
#### code stream just after the call, and then returns to the byte
#### just after the terminating null.  This subroutine preserves all
#### general-purpose registers.

puts:	xchg %si, %ss:(%esp)
	push %ax
next_char:
	mov %cs:(%si), %al
	inc %si
	test %al, %al
	jz 1f
	call putc
	jmp next_char
1:	pop %ax
	xchg %si, %ss:(%esp)
	ret

#### Character output subroutine.  Prints the character in AL to the
#### VGA display and serial port 0, using BIOS services (see
#### [IntrList]).  Preserves all general-purpose registers.
####
#### If called upon to output a carriage return, this subroutine
#### automatically supplies the following line feed.

putc:	pusha

1:	sub %bh, %bh			# Page 0.
	mov $0x0e, %ah			# Teletype output service.
	int $0x10

	mov $0x01, %ah			# Serial port output service.
	sub %dx, %dx			# Serial port 0.
2:	int $0x14			# Destroys AH.
	test $0x80, %ah			# Output timed out?
	jz 3f
	movw $0x9090, 2b		# Turn "int $0x14" above into NOPs.

3:
	cmp $'\r', %al
	jne popa_ret
	mov $'\n', %al
	jmp 1b

popa_ret:
	popa
	ret				# Error code still in CF


#### TODO:  YOU CAN PUT HELPER FUNCTIONS HERE IF YOU WISH.


#### Command-line arguments and their count.
#### This is written by the `pintos' utility and read by the kernel.
#### The loader itself does not do anything with the command line.
	.org LOADER_ARG_CNT - LOADER_BASE
	.fill LOADER_ARG_CNT_LEN, 1, 0

	.org LOADER_ARGS - LOADER_BASE
	.fill LOADER_ARGS_LEN, 1, 0

#### Partition table.
	.org LOADER_PARTS - LOADER_BASE
	.fill LOADER_PARTS_LEN, 1, 0

#### Boot-sector signature for BIOS inspection.
	.org LOADER_SIG - LOADER_BASE
	.word 0xaa55

