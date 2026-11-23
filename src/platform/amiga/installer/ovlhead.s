| Overlay header: makes AmigaDOS's loader treat OpenLaraV4.run as an overlay program, so it stops reading
| at the HUNK_OVERLAY block that follows the code (the archive after it is never loaded), hands the
| program its open file in the longword at offset 8, and closes that file in UnLoadSeg. Linked first,
| ahead of libnix's startup code; the branch leaves d0/a0/sp (the command line) untouched.
	.text
	.globl	_ovlhead
_ovlhead:
	bra.w	1f
	.long	0x0000ABCD		| overlay magic at offset 4
	.long	0			| 8: file handle (set by the loader)
	.long	0			| 12: overlay table
	.long	0			| 16: hunk table
	.long	0			| 20: global vector
1:	jmp	_____start
