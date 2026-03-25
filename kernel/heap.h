/* Copyright (C) 2025 Ahmed Gheith and contributors.
 *
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef HEAP_H
#define HEAP_H

// initialize the heap allocator over one writable memory range
void heap_init(void* start, unsigned size);

// allocate at least size bytes, or panic on allocator failure
void* malloc(unsigned size);

// allocate memory and count it as intentionally leaked for leak reporting
void* leak(unsigned size);

// free one allocation returned by malloc/leak; NULL is ignored
void free(void* p);

// print the allocator leak summary
void check_leaks(void);

#endif // HEAP_H
