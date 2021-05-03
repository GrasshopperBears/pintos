/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <hash.h>
#include "threads/mmu.h"
#include "threads/synch.h"

struct lock hash_lock;

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	lock_init(&hash_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
// static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;
	bool (*initializer)(struct page *, enum vm_type, void *);
	struct page* page;
	bool succ;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		page = malloc(sizeof(struct page));

		// TODO: malloc fail 시 처리 이게 맞나??
		if (page == NULL)
			goto err;

		initializer = (VM_TYPE(type) == VM_ANON) ? anon_initializer : file_backed_initializer;
		uninit_new (page, upage, init, type, aux, initializer);
		page->writable = writable;

		/* TODO: Insert the page into the spt. */
		succ = spt_insert_page(spt, page);
		// printf("inserted: %p\n", upage);
		if (!succ)
			goto err;
		if (init != NULL)
			init(page, aux);
		return succ;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function. */
	struct page p;
  struct hash_elem *h_el;

	p.va = va;
	h_el = hash_find(&spt->hash, &p.spt_hash_elem);
	if (h_el != NULL)
		page = hash_entry(h_el, struct page, spt_hash_elem);
	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	/* TODO: Fill this function. */
	struct hash_elem* result;

	// if (spt_find_page(spt, page->va))
	// 	return false;
	result = hash_insert(&spt->hash, &page->spt_hash_elem);
	if (result == NULL)
		succ = true;
	
	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	void* newpage = palloc_get_page(PAL_USER);

	if (newpage == NULL)
		PANIC("todo");	// TODO: user pool 다 찼을 경우 evict 구현
		// return vm_evict_frame();

	frame = malloc(sizeof(struct frame));
	frame->kva = newpage;
	frame->page = NULL;

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	// spt 에서 주소에 해당하는 page가 존재하는지 찾기
	page = spt_find_page (&thread_current()->spt, pg_round_down(addr));

	if (page == NULL) {
		// printf("err1-addr: %p\n", addr);
		return false;
	}
	if (is_kernel_vaddr(addr) || is_kernel_vaddr(page->va)) {
		// printf("err 2\n");
		return false;
	}
	// if (!not_present) {
	// 	printf("err 3\n");
	// 	return false;
	// }
	// printf("pass\n");
	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	page = malloc(sizeof(struct page));
	page->va = va;
	spt_insert_page(&thread_current()->spt, page);
	// printf("inserted-claim: %p\n", va);

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	bool succ;

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	// setup MMU = add the mapping from the virtual address to the physical address in the page table
	succ = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);

	// return swap_in (page, frame->kva);	// TODO: swap in 구현하면 갈아끼워야 할듯?
	return succ;
}

unsigned
spt_hash (const struct hash_elem *h_el, void *aux UNUSED) {
  const struct page *p = hash_entry (h_el, struct page, spt_hash_elem);
  return hash_bytes (&p->va, sizeof p->va);
}

bool
spt_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
  const struct page *a = hash_entry (a_, struct page, spt_hash_elem);
  const struct page *b = hash_entry (b_, struct page, spt_hash_elem);

  return a->va < b->va;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->hash, spt_hash, spt_less, NULL);
}

void
copy_spt_hash(struct hash_elem *e, void *aux) {
	struct supplemental_page_table *dst = (struct supplemental_page_table*)aux;
	struct page* page_original = hash_entry(e, struct page, spt_hash_elem);
	struct page* page_copy = malloc(sizeof(struct page));

	page_copy->va = page_original->va;
	page_copy->writable = page_original->writable;
	spt_insert_page(dst, page_copy);

	page_copy->frame = page_original->frame;
	pml4_set_page(thread_current()->pml4, page_copy->va, page_copy->frame->kva, page_copy->writable);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	src->hash.aux = dst;
	hash_apply(&src->hash, copy_spt_hash);
	src->hash.aux = NULL;
	return true;
}

void
kill_spt_hash(struct hash_elem *e, void *aux) {
	struct page* page = hash_entry(e, struct page, spt_hash_elem);
	destroy(page);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_apply(spt, kill_spt_hash);
}
