/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/mmu.h"
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
	return true;
}

void
copy_file_page(struct file_page* src, struct file_page* dst) {
	// lock_acquire(&filesys_lock);
	dst->file = src->file;
	// lock_release(&filesys_lock);
	dst->is_last = src->is_last;
	dst->data_bytes = src->data_bytes;
	dst->zero_bytes = src->zero_bytes;
	dst->offset = src->offset;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
	if (file_page->file == NULL)
		return false;

	lock_acquire(&filesys_lock);
	file_read_at(file_page->file, page->va, file_page->data_bytes, file_page->offset);
	lock_release(&filesys_lock);
	if (file_page->zero_bytes > 0)
		memset(page->va + file_page->data_bytes, 0, file_page->zero_bytes);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;

	if (file_page->file == NULL)
		return false;

	lock_acquire(&filesys_lock);
	file_write_at(file_page->file, page->va, file_page->data_bytes, file_page->offset);
	lock_release(&filesys_lock);
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;

	if (!page->swapped_out) {
		file_backed_swap_out(page);
	}
	if (page->frame != NULL)
		common_clear_page(page);
}

void
mmap_set_page(struct page *page, void *aux) {
	struct mmap_parameter *params = (struct mmap_parameter *)aux;

	page->file.data_bytes = params->data_bytes;
	page->file.file = params->file;
	page->file.offset = params->offset;
	page->file.zero_bytes = params->zero_bytes;
	page->file.is_last = params->is_last;
	free(aux);
}

void *
copy_mmap_parameter(struct page* src, void* dst) {
	struct mmap_parameter* aux = malloc(sizeof(struct mmap_parameter));
	struct mmap_parameter *src_aux = (struct mmap_parameter *)src->uninit.aux;

	aux->file = src_aux->file;
	aux->offset = src_aux->offset;
	aux->data_bytes = src_aux->data_bytes;
	aux->zero_bytes = src_aux->zero_bytes;
	aux->is_last = src_aux->is_last;

	return aux;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	size_t page_number = length / PGSIZE + (length % PGSIZE == 0 ? 0 : 1);
	size_t left_size = length;
	struct mmap_parameter *aux;

	if (file_length(file) <= offset)	// ????????
		return NULL;

	for (int i = 0; i < page_number; i++) {
		if (addr + PGSIZE * i == NULL || is_kernel_vaddr(addr + PGSIZE * i))
			return NULL;
		if (spt_find_page(&thread_current()->spt, addr + PGSIZE * i) != NULL)
			return NULL;
	}

	for (int i = 0; i < page_number; i++) {
		aux = malloc(sizeof(struct mmap_parameter));
		lock_acquire(&filesys_lock);
		aux->file = file_reopen(file);
		lock_release(&filesys_lock);
		aux->offset = offset + PGSIZE * i;
		if (left_size >= PGSIZE) {
			aux->data_bytes = PGSIZE;
			aux->zero_bytes = 0;
			aux->is_last = left_size == PGSIZE ? true : false;
		} else {
			aux->data_bytes = left_size;
			aux->zero_bytes = PGSIZE - left_size;
			aux->is_last = true;
		}
		if (vm_alloc_page_with_initializer(VM_FILE, addr + PGSIZE * i, writable, mmap_set_page, aux) == NULL)
			return NULL;
		left_size -= PGSIZE;
	}
	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct page* page;
	struct file_page* fp;
	void* curr_addr = addr;

	while (true) {
		page = spt_find_page(&thread_current()->spt, curr_addr);
		if (page == NULL || page_get_type(page) == VM_ANON)
			exit(-1);

		fp = &page->file;
		if (VM_TYPE(page->operations->type) == VM_FILE && pml4_is_dirty(thread_current()->pml4, page->va)) {
			lock_acquire(&filesys_lock);
			file_write_at(fp->file, page->va, fp->data_bytes, fp->offset);
			lock_release(&filesys_lock);
		} 
		if (VM_TYPE(page->operations->type) == VM_FILE && fp->file != NULL) {
			lock_acquire(&filesys_lock);
			file_close(fp->file);
			lock_release(&filesys_lock);
		}
		fp->file = NULL;
		
		if (fp->is_last)
			break;
		curr_addr += PGSIZE;
	}
}
