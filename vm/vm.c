/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <hash.h>
#include "threads/mmu.h"
#include "threads/synch.h"
#include <string.h>

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
		if (page == NULL) {
			printf("malloc error\n");
			goto err;	
		}
		initializer = (VM_TYPE(type) == VM_ANON) ? anon_initializer : file_backed_initializer;
		uninit_new (page, upage, init, type, aux, initializer);
		page->writable = writable;
		page->is_stack = false;

		/* TODO: Insert the page into the spt. */
		succ = spt_insert_page(spt, page);
		if (!succ) {
			printf("spt insert error\n");
			goto err;
		}
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
	void* newpage = palloc_get_page(PAL_USER | PAL_ZERO);

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

void
after_stack_set(struct page *page, void *aux) {
	page->is_stack = true;
	thread_current()->stack_page_count++;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
	void *stack_end = pg_round_down(addr);
	void* currPtr = (void *) (((uint8_t *) USER_STACK) - PGSIZE);
	bool success = true;

	while (currPtr >= stack_end) {
		if (spt_find_page(&thread_current()->spt, currPtr) == NULL) {
			if (!vm_alloc_page_with_initializer(VM_ANON, currPtr, true, after_stack_set, NULL)) {
				success = false;
				break;
			}
		}
		currPtr = (void *) (((uint8_t *) currPtr) - PGSIZE);
	}
	if (success)
		thread_current()->tf.rsp = addr;
	// else
	// 	exit(-1);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct thread* curr = thread_current();
	struct supplemental_page_table *spt UNUSED = &curr->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	int MAX_STACK_COUNT = 256;
	int MAX_STACK_ADDR = USER_STACK - 1 << 20;	// limit stack size to 1mb

	if (user && is_kernel_vaddr(addr))
		return false;

	if ((user && f->rsp - 8 <= (uintptr_t)addr) || (!user && ptov(addr) >= MAX_STACK_ADDR)) {
		if (curr->stack_page_count >= MAX_STACK_COUNT) {
			exit(-1);
		}
		vm_stack_growth(user ? addr : ptov(addr));
	}

	// spt 에서 주소에 해당하는 page가 존재하는지 찾기
	page = spt_find_page (&thread_current()->spt, pg_round_down(addr));

	if (page == NULL) {
		// printf("err1-addr: %p %p\n", addr, pg_round_down(addr));
		return false;
	}
	if ((!not_present && write) || (!not_present && !page->writable)) {
		// printf("%d %d %d\n", write, page->writable, not_present);
		return false;
	}
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
	page->writable = true;
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
	page->uninit.page_initializer(page, page_get_type(page), frame->kva);

	return swap_in (page, frame->kva);
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
	struct frame *frame = vm_get_frame();
	struct page* page_copy = malloc(sizeof(struct page));

	frame->page = page_copy;
	page_copy->frame = frame;
	page_copy->va = page_original->va;
	page_copy->writable = page_original->writable;
	page_copy->operations = page_original->operations;
	memcpy(frame->kva, page_original->frame->kva, PGSIZE);

	spt_insert_page(dst, page_copy);
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
	free(page);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_clear(spt, kill_spt_hash);
}

void
common_clear_page(struct page *page) {
	pml4_clear_page(thread_current()->pml4, page->va);
	palloc_free_page(page->frame->kva);
	free(page->frame);
}
