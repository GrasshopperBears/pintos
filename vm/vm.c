/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include <hash.h>
#include "threads/mmu.h"
#include "threads/synch.h"
#include <string.h>
#include "userprog/process.h"

struct lock hash_lock;
struct list frames_list;
extern struct lock filesys_lock;
struct lock cow_lock;
struct lock handle_fault_lock;

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
	lock_init(&cow_lock);
	list_init(&frames_list);
	lock_init(&handle_fault_lock);
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
static bool vm_do_claim_page (struct page *page);
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
		page->uninit.copy = (VM_TYPE(type) == VM_ANON) ? copy_lazy_parameter : copy_mmap_parameter;
		page->swapped_out = false;
		page->owner = thread_current();

		/* TODO: Insert the page into the spt. */
		succ = spt_insert_page(spt, page);
		if (!succ) {
			printf("spt insert error\n");
			goto err;
		}
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
	/* TODO: Fill this function. */
	struct hash_elem* result;

	lock_acquire(&hash_lock);
	result = hash_insert(&spt->hash, &page->spt_hash_elem);
	lock_release(&hash_lock);

	if (result == NULL)
		return true;
	
	return false;
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
	return list_entry(list_pop_back(&frames_list), struct frame, elem);
}

void
init_frame_struct(struct frame* frame, void *kva) {
	frame->kva = kva;
	frame->original_kva = kva;
	frame->page = NULL;
	list_init(&frame->referers);
	list_push_front(&frames_list, &frame->elem);
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	struct page* page_to_evict;
	struct list_elem* el;
	
	/* TODO: swap out the victim and return the evicted frame. */
	el = list_front(&victim->referers);
	while (el != list_end(&victim->referers)) {
		page_to_evict = list_entry(el, struct page, referer_elem);
		swap_out(page_to_evict);
		page_to_evict->swapped_out = true;
		page_to_evict->frame = NULL;
		pml4_clear_page(thread_current()->pml4, page_to_evict->va);
		el = el->next;
		list_remove(&page_to_evict->referer_elem);
	}
	init_frame_struct(victim, victim->original_kva);

	return victim;
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

	if (newpage == NULL) {
		if (list_empty(&frames_list))
			PANIC("disk problem: nothing allocated but no frame's possible");
		return vm_evict_frame();
	}
	frame = malloc(sizeof(struct frame));
	init_frame_struct(frame, newpage);

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

void
after_stack_set(struct page *page, void *aux) {
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
}

void
clear_frame(struct frame* frame) {
	list_remove(&frame->elem);
	palloc_free_page(frame->kva);
	free(frame);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
	struct frame* original_frame = page->frame;
	struct frame* new_frame = vm_get_frame();
	bool succ;

	new_frame->page = page;
	page->frame = new_frame;
	page->cow_writable = true;
	lock_acquire(&cow_lock);
	pml4_clear_page(thread_current()->pml4, page->va);
	succ = pml4_set_page(thread_current()->pml4, page->va, new_frame->kva, page->writable);
	memcpy(new_frame->kva, original_frame->kva, PGSIZE);
	lock_release(&cow_lock);
	if (VM_TYPE(page->operations->type) == VM_FILE) {
		lock_acquire(&filesys_lock);
		page->file.file = file_reopen(page->file.file);
		lock_release(&filesys_lock);
	}
	list_remove(&page->referer_elem);
	if (list_empty(&original_frame->referers))
		clear_frame(original_frame);
	list_push_front(&new_frame->referers, &page->referer_elem);

	page->swapped_out = false;
	return succ;
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
	bool succ;

	// printf("fault handler at %p by %d\n", addr, user);
	lock_acquire(&handle_fault_lock);
	if (user && is_kernel_vaddr(addr)) {
		succ = false;
		goto done;
	}
	if ((user && f->rsp - 8 <= (uintptr_t)addr) || (!user && ptov(addr) >= MAX_STACK_ADDR)) {
		if (curr->stack_page_count >= MAX_STACK_COUNT) {
			lock_release(&handle_fault_lock);
			exit(-1);
		}
		vm_stack_growth(user ? addr : ptov(addr));
	}
	page = spt_find_page (&thread_current()->spt, pg_round_down(addr));
	if (page == NULL) {
		succ = false;
		goto done;
	}
	if ((!not_present && write)) {
		if (!not_present && !page->writable) {
			succ=false;
			goto done;
		}
		if (!not_present && !page->cow_writable) {
			succ = vm_handle_wp(page);
			goto done;
		}
	}
	if (page_get_type(page) == VM_FILE && page->file.file == NULL) {
		succ = false;
		goto done;
	}
	succ = vm_do_claim_page (page);
	done:
		lock_release(&handle_fault_lock);
		if (!succ)
			return succ;
		if (!write)
			pml4_set_dirty(curr->pml4, page->va, false);

		return succ;
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
	page->owner = thread_current();
	spt_insert_page(&thread_current()->spt, page);

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	bool succ;
	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	// setup MMU = add the mapping from the virtual address to the physical address in the page table
	succ = pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);
	if (page->operations->type == VM_UNINIT && page->uninit.init != NULL)
		page->uninit.init(page, page->uninit.aux);

	page->uninit.page_initializer(page, page_get_type(page), frame->kva);
	page->swapped_out = false;
	list_push_front(&frame->referers, &page->referer_elem);

	return swap_in (page, frame->kva);
}

unsigned
page_hash (const struct hash_elem *h_el, void *aux UNUSED) {
  const struct page *p = hash_entry (h_el, struct page, spt_hash_elem);
  return hash_bytes (&p->va, sizeof p->va);
}

bool
page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
  const struct page *a = hash_entry (a_, struct page, spt_hash_elem);
  const struct page *b = hash_entry (b_, struct page, spt_hash_elem);

  return a->va < b->va;
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	lock_acquire(&hash_lock);
	hash_init(&spt->hash, page_hash, page_less, NULL);
	lock_release(&hash_lock);
}

void
copy_page_struct(struct page* src, struct page* dst) {
	dst->va = src->va;
	dst->writable = src->writable;
	dst->cow_writable = src->cow_writable = false;
	dst->swapped_out = true;
	dst->operations = src->operations;
	dst->owner = thread_current();
}

void
copy_spt_hash(struct hash_elem *e, void *aux) {
	struct supplemental_page_table *dst = (struct supplemental_page_table*)aux;
	struct page* page_original = hash_entry(e, struct page, spt_hash_elem);
	struct frame *frame;
	struct page* page_copy;
	void *copied_aux;


	if (VM_TYPE(page_original->operations->type) == VM_UNINIT) {
		copied_aux = page_original->uninit.copy(page_original, NULL);
		vm_alloc_page_with_initializer (page_get_type(page_original), page_original->va,	
										page_original->writable, page_original->uninit.init, copied_aux);
	}	else {
		page_copy = malloc(sizeof(struct page));
		// frame = vm_get_frame();
		lock_acquire(&cow_lock);
		frame = page_original->frame;
		list_push_front(&frame->referers, &page_copy->referer_elem);
		frame->page = NULL;
		page_copy->frame = frame;
		copy_page_struct(page_original, page_copy);

		memcpy(frame->kva, page_original->frame->kva, PGSIZE);
		pml4_clear_page(thread_current()->pml4, page_copy->va);
		pml4_set_page(page_original->owner->pml4, page_original->va, frame->kva, false);
		pml4_set_page(thread_current()->pml4, page_copy->va, frame->kva, false);
		
		if (VM_TYPE(page_original->operations->type) == VM_FILE)
			copy_file_page(&page_original->file, &page_copy->file);
		lock_release(&cow_lock);
		spt_insert_page(&thread_current()->spt.hash, page_copy);
	}
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
	vm_dealloc_page(page);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	lock_acquire(&hash_lock);
	hash_clear(spt, kill_spt_hash);
	lock_release(&hash_lock);
}

void
common_clear_page(struct page *page) {
	pml4_clear_page(thread_current()->pml4, page->va);
	list_remove(&page->referer_elem);
	if (list_empty(&page->frame->referers)) 
		clear_frame(page->frame);
}
