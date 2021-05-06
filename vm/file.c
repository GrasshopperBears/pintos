/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

extern struct lock filesys_lock;

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;

	// TODO: reopen 처리(file_reopen)
	if (file_page->file == NULL)
		return false;
	lock_acquire(&filesys_lock);
	file_read_at(file_page->file, page->va, file_page->data_bytes, file_page->offset);
	lock_release(&filesys_lock);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

void
mmap_set_page(struct page *page, void *aux) {
	struct mmap_parameter *params = (struct mmap_parameter *)aux;

	page->file.data_bytes = params->data_bytes;
	page->file.file = file_reopen(params->file);
	page->file.offset = params->offset;
	page->file.zero_bytes = params->zero_bytes;
	free(aux);
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	int page_number = length / PGSIZE + (length % PGSIZE == 0 ? 0 : 1);
	size_t left_size = length;
	struct mmap_parameter *aux;

	for (int i = 0; i < page_number; i++) {
		aux = malloc(sizeof(struct mmap_parameter));
		aux->file = file;
		aux->offset = offset + PGSIZE * i;
		if (left_size >= PGSIZE) {
			aux->data_bytes = PGSIZE;
			aux->zero_bytes = 0;
		} else {
			aux->data_bytes = left_size;
			aux->zero_bytes = PGSIZE - left_size;
		}
		if (vm_alloc_page_with_initializer(VM_FILE, addr + PGSIZE * i, writable, mmap_set_page, aux) == NULL)
			return NULL;
		left_size -= PGSIZE;
	}
	for (int i = 0; i < page_number; i++) {
		if (!vm_alloc_page(VM_FILE, addr + PGSIZE * i, writable))
			return NULL;
	}
	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
}
