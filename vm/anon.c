/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/mmu.h"
#include "threads/synch.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

static unsigned int SEC_PER_PAGE = PGSIZE / DISK_SECTOR_SIZE;
struct swap_disk_info {
	struct hash swapped_page_hash;
	unsigned int max_number;
	unsigned int current_using;
	struct list using_sectors;
};

struct swap_disk_info swap_disk_info;
struct lock swap_disk_lock;

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	lock_init(&swap_disk_lock);
	swap_disk = disk_get (1, 1);
	hash_init(&swap_disk_info.swapped_page_hash, page_hash, page_less, NULL);
	list_init(&swap_disk_info.using_sectors);
	swap_disk_info.max_number = disk_size(swap_disk) / SEC_PER_PAGE;
	swap_disk_info.current_using = 0;
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;

	page->swapped_out = false;

	if (hash_find(&swap_disk_info.swapped_page_hash, &page->swapped_disk_hash_elem) == NULL)
		return true;
	
	lock_acquire(&swap_disk_lock);
	for (int i = 0; i < SEC_PER_PAGE; i++) {
		disk_read(swap_disk, SEC_PER_PAGE * page->anon.page_sec_idx + i, page->va + DISK_SECTOR_SIZE * i);
	}
	hash_delete(&swap_disk_info.swapped_page_hash, &page->swapped_disk_hash_elem);
	list_remove(&page->anon.elem);
	swap_disk_info.current_using--;
	lock_release(&swap_disk_lock);

	return true;
}

bool
compare_anon_page(const struct list_elem *e1, const struct list_elem *e2, void *aux UNUSED) {
	struct anon_page *ap1 = list_entry(e1, struct anon_page, elem);
	struct anon_page *ap2 = list_entry(e2, struct anon_page, elem);
	
	int sec_no1 = ap1->page_sec_idx;
	int sec_no2 = ap2->page_sec_idx;
	
	if (sec_no1 < sec_no2)
		return true;
	else
		return false;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	unsigned int sec_idx = 0;
	struct list_elem* el;
	struct anon_page* ap;

	if (swap_disk_info.max_number <= swap_disk_info.current_using)
		PANIC("No more swap disk slot, max: %d, curr: %d\n", swap_disk_info.max_number, swap_disk_info.current_using);

	if (list_size(&swap_disk_info.using_sectors) > 0) {
		el = list_begin(&swap_disk_info.using_sectors);
		while (el != list_end(&swap_disk_info.using_sectors)) {
			ap = list_entry(el, struct anon_page, elem);
			if (sec_idx != ap->page_sec_idx)
				break;
			sec_idx++;
			el = el->next;
		}
	}

	lock_acquire(&swap_disk_lock);
	for (int i = 0; i < SEC_PER_PAGE; i++) {
		disk_write(swap_disk, SEC_PER_PAGE * sec_idx + i, page->va + DISK_SECTOR_SIZE * i);
	}
	page->anon.page_sec_idx = sec_idx;
	list_insert_ordered(&swap_disk_info.using_sectors, &page->anon.elem, compare_anon_page, NULL);
	hash_insert(&swap_disk_info.swapped_page_hash, &page->swapped_disk_hash_elem);
	swap_disk_info.current_using++;
	page->swapped_out = true;
	lock_release(&swap_disk_lock);

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	if (page->frame != NULL)
		common_clear_page(page);
	else {
		lock_acquire(&swap_disk_lock);
		hash_delete(&swap_disk_info.swapped_page_hash, &page->swapped_disk_hash_elem);
		list_remove(&page->anon.elem);
		swap_disk_info.current_using--;
		lock_release(&swap_disk_lock);
	}
}
