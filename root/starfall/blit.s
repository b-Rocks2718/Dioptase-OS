.text
.global starfall_blit_span

# Copy changed arena tile entries to the live framebuffer and remember them.
# Inputs: r1 points at the first 16-bit framebuffer entry, r2 at wanted art,
# r3 at displayed art, and r4 is the number of entries in this row span.
# Outputs: none. r1-r7 are caller-saved; r20-r27, bp, sp, and ra are untouched.
# This user-mode leaf routine requires aligned, valid buffers covering r4
# entries. The caller excludes menu cells and owns the MMIO framebuffer mapping.
starfall_blit_span:
	cmp r4, r0
	ble starfall_blit_span.done
starfall_blit_span.loop:
	lda r5, [r2]
	lda r6, [r3]
	cmp r5, r6
	bz starfall_blit_span.next
	and r7, r5, 255
	sda r7, [r1]
	sda r5, [r3]
starfall_blit_span.next:
	add r1, r1, 2
	add r2, r2, 2
	add r3, r3, 2
	add r4, r4, -1
	bnz starfall_blit_span.loop
starfall_blit_span.done:
	ret
