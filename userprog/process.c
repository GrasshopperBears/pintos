#include "userprog/process.h"
#include "threads/synch.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);

extern struct lock filesys_lock;

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
	current->is_process = true;
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;
	char *save_ptr;
	struct child_elem* c_el = palloc_get_page(PAL_ZERO);

	if (c_el == NULL)
		exit(-1);

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);
	thread_current()->parent = NULL;
	thread_current()->current_dir = dir_open_root();

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (strtok_r (file_name, " ", &save_ptr), PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR) {
		palloc_free_page(c_el);
		palloc_free_page (fn_copy);
	} else {
		c_el->tid = tid;
		c_el->terminated = false;
		c_el->waiting = false;
		c_el->waiting_sema = NULL;
		list_push_front(&thread_current()->children_list, &c_el->elem);
	}
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
	struct thread* current = thread_current();
	struct file_elem* f_el_stdin = malloc(sizeof(struct file_elem));
	struct file_elem* f_el_stdout = malloc(sizeof(struct file_elem));
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();
	f_el_stdin->fd = 0;
	f_el_stdin->open = true;
	f_el_stdin->file = NULL;
	f_el_stdin->reference = -1;
	f_el_stdout->fd = 1;
	f_el_stdout->open = true;
	f_el_stdout->file = NULL;
	f_el_stdout->reference = -1;
	list_push_back(&current->files_list, &f_el_stdin->elem);
	list_push_back(&current->files_list, &f_el_stdout->elem);

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

struct file_map_elem {
  struct file* original;
  struct file* copy;
};

struct file*
find_copied_file(struct file_map_elem* map, int size, struct file* find) {
	struct list_elem* el;
	struct file_map_elem* file_map_el;

	for (int i = 0; i < size; i++) {
		if (map[i].original == find)
			return map[i].copy;
	}
	return NULL;
}

bool
copy_file_list(struct thread* parent, struct thread* child) {
	struct list_elem *el;
	struct file_elem *f_el;
	struct file_elem* new_f_el;
	int size, idx = 0;
	struct file_map_elem* map;
	struct file* found_file;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	if (list_empty(&parent->files_list)) {
		return true;
	}
	size = list_size(&parent->files_list);
	map = malloc(sizeof(struct file_map_elem) * size);
	el = list_begin(&parent->files_list);
	while (el != list_end(&parent->files_list)) {
		f_el = list_entry(el, struct file_elem, elem);
		if (f_el->fd < 0) {
			free(map);
			return false;
		}
		new_f_el = malloc(sizeof(struct file_elem));
		if (new_f_el == NULL) {
			free(map);
			return false;
		}
		
		new_f_el->fd = f_el->fd;
		new_f_el->open = f_el->open;
		new_f_el->is_file = f_el->is_file;
		new_f_el->reference = f_el->reference;
		found_file = f_el->is_file ? find_copied_file(map, size, f_el->file) : NULL;
		// LAB 4 TODO: DIRECTORY DUPLICATE. ????????? ????????? pointing
		new_f_el->dir = f_el->dir;
		
		if (found_file == NULL) {
			if (!f_el->is_file) {
				new_f_el->file = NULL;
			} else if (f_el->fd > 1 && f_el->reference != 0 && f_el->reference != 1) {
				new_f_el->file = file_duplicate(f_el->file);
				if (new_f_el->file == NULL) {
					free(new_f_el);
					free(map);
					return false;
				}
				map[idx].original = f_el->file;
				map[idx].copy = new_f_el->file;
				idx++;
			}
		} else {
			new_f_el->file = found_file;
		}
		list_push_back(&child->files_list, &new_f_el->elem);
		el = el->next;
	}
	free(map);
	return true;
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED, int* parent_lock) {
	/* Clone current thread to new thread.*/
	struct parent_info* p_info = palloc_get_page(PAL_ZERO);

	if (p_info == NULL)
		exit(-1);

	p_info->t = thread_current();
	p_info->if_ = if_;
	p_info->parent_lock = parent_lock;

	return thread_create (name,
			PRI_DEFAULT, __do_fork, p_info);
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if(is_kern_pte(pte))
		return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (newpage == NULL)
		return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page (newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct parent_info* p_info = (struct parent_info *) aux;
	struct thread *parent = p_info->t;
	struct intr_frame *parent_if = p_info->if_;
	bool succ = true;
	struct child_elem* c_el;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;
	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	process_init ();
	lock_acquire(&filesys_lock);
	if (!copy_file_list(parent, current)) {
		lock_release(&filesys_lock);
		goto error;
	}
	lock_release(&filesys_lock);

	c_el = palloc_get_page(PAL_ZERO);
	if (c_el == NULL)
		exit(-1);
	lock_acquire(&filesys_lock);
	current->current_dir = dir_reopen(parent->current_dir);
	current->running_file = file_reopen(parent->running_file);
	lock_release(&filesys_lock);
	current->parent = parent;
	current->is_process = true;
	c_el->tid = current->tid;
	c_el->terminated = false;
	c_el->waiting = false;
	c_el->waiting_sema = NULL;
	list_push_front(&parent->children_list, &c_el->elem);
	*p_info->parent_lock += 1;

	/* Finally, switch to the newly created process. */
	if (succ) {
		if (parent->status == THREAD_BLOCKED)
			thread_unblock(parent);
		palloc_free_page(aux);
		// printf("fork done\n");
		do_iret (&if_);
	}
error:
	*p_info->parent_lock -= 1;
	if (parent->status == THREAD_BLOCKED)
		thread_unblock(parent);
	palloc_free_page(aux);
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	lock_acquire(&filesys_lock);
	success = load (file_name, &_if);
	lock_release(&filesys_lock);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;
	
	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	struct thread *curr = thread_current ();
	struct semaphore sema;
	struct list_elem* el;
	struct child_elem* c_el;
	int returned_status;

	if (list_empty(&curr->children_list))
		return -1;

	el = list_begin(&curr->children_list);
	while (el != list_end(&curr->children_list)) {
		c_el = list_entry(el, struct child_elem, elem);
		if (c_el->tid == child_tid) {
			if (c_el->waiting)
				return -1;
			
			if (!c_el->terminated) {
				c_el->waiting = true;
				sema_init(&sema, 0);
				c_el->waiting_sema = &sema;
				sema_down(&sema);
			}
			returned_status = c_el->exit_status;
			list_remove(&c_el->elem);
			palloc_free_page(c_el);
			return returned_status;
		}
		el = el->next;
	}
	return -1;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	struct list_elem* el;
	struct child_elem* c_el;

	if (!curr->is_process)
		return;
	printf ("%s: exit(%d)\n", curr->name, curr->exit_status);

	if (curr->parent != NULL) {
		el = list_begin(&curr->parent->children_list);
		while (el != list_end(&curr->parent->children_list)) {
			c_el = list_entry(el, struct child_elem, elem);
			if (c_el->tid == curr->tid) {
				c_el->exit_status = curr->exit_status;
				c_el->terminated = true;
				if (c_el->waiting) {
					sema_up(c_el->waiting_sema);
				}
				break;
			}
			el = el->next;
		}
	}
	if (!list_empty(&curr->children_list)) {
		el = list_begin(&curr->children_list);
		while (el != list_end(&curr->children_list)) {
			c_el = list_entry(el, struct child_elem, elem);
			el = el->next;
			palloc_free_page(c_el);
		}
	}
	if (!lock_held_by_current_thread(&filesys_lock))
		lock_acquire(&filesys_lock);
	if (curr->running_file != NULL)
		file_close(curr->running_file);
	if (lock_held_by_current_thread(&filesys_lock))
		lock_release(&filesys_lock);
	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	unsigned int try_reload_cnt = 0;
	off_t file_ofs;
	bool success = false;
	int i, argc, arg_len, idx, word_align, PTR_SIZE = 8;
	char *token, *save_ptr, *copied_file_name = palloc_get_page(PAL_ZERO);
	int64_t* args_addr_list;

	if (copied_file_name == NULL)
		exit(-1);

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	strlcpy(copied_file_name, file_name, strlen(file_name) + 1);
	token = strtok_r (file_name, " ", &save_ptr);

	/* Open executable file. */
	// lock_acquire(&filesys_lock);
	while (file == NULL && try_reload_cnt < 10) {
		file = filesys_open (token);
		try_reload_cnt++;
	}
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		exit(-1);
	}
	t->running_file = file;
	file_deny_write(file);

	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file)) {
			goto done;
		}
		file_seek (file, file_ofs);
		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr) {
			goto done;
		}
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	argc = 0;
	while (token != NULL) {
		argc++;
		token = strtok_r (NULL, " ", &save_ptr);
	}
	args_addr_list = (int64_t *)malloc(sizeof(int64_t) * argc);

	token = strtok_r (copied_file_name, " ", &save_ptr);
	idx = 0;
	while(token != NULL) {
		arg_len = strlen(token);
		if_->rsp -= (arg_len + 1);
		memcpy(if_->rsp, token, arg_len + 1);
		args_addr_list[idx] = if_->rsp;
		token = strtok_r (NULL, " ", &save_ptr);
		idx++;
	}

	word_align = 0;
	while ((if_->rsp - word_align) % PTR_SIZE != 0) {
		word_align++;
	}
	if (word_align != 0) {
		if_->rsp -= word_align;
		memset(if_->rsp, 0, word_align);
	}

	if_->rsp -= PTR_SIZE;
	memset(if_->rsp, 0, PTR_SIZE);

	for (i = argc - 1; i >= 0; i--) {
		if_->rsp -= PTR_SIZE;
		memcpy(if_->rsp, &args_addr_list[i], PTR_SIZE);
	}

	if_->rsp -= PTR_SIZE;
	memset(if_->rsp, 0, PTR_SIZE);
	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp + PTR_SIZE;
	// hex_dump ((uintptr_t)if_->rsp, if_->rsp, USER_STACK - if_->rsp, true);

	free(args_addr_list);
	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	palloc_free_page(copied_file_name);
	if (!success) {
		t->running_file = NULL;
		file_close (file);
	}

	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	struct lazy_parameter * params = (struct lazy_parameter *)aux;
	bool acquired = false;
	
	if (params->file == NULL)
		return false;

	if (params->read_bytes > 0) {
		if (!lock_held_by_current_thread(&filesys_lock)) {
			lock_acquire(&filesys_lock);
			acquired = true;
		}
		file_read_at(params->file, params->upage, params->read_bytes, params->ofs);
		if (acquired)
			lock_release(&filesys_lock);
	}
	if (params->zero_bytes > 0)
		memset(page->va + params->read_bytes, 0, params->zero_bytes);
	file_close(params->file);
	free(aux);
	return true;
}

void *
copy_lazy_parameter(struct page* src, void* dst) {
	struct lazy_parameter *aux = malloc(sizeof(struct lazy_parameter));
	struct lazy_parameter *src_aux = (struct lazy_parameter *)src->uninit.aux;

	aux->file = file_reopen(src_aux->file);
	aux->ofs = src_aux->ofs;
	aux->read_bytes = src_aux->read_bytes;
	aux->zero_bytes = src_aux->zero_bytes;
	aux->upage = src_aux->upage;
	return aux;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);
	unsigned cnt = 0;

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = page_read_bytes < PGSIZE ? PGSIZE - page_read_bytes : 0;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct lazy_parameter *aux = malloc(sizeof(struct lazy_parameter));
		aux->file = file_reopen(file);
		aux->ofs = ofs + cnt * PGSIZE;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->upage = upage;
		aux->copy = copy_lazy_parameter;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		cnt++;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */
	success = vm_alloc_page_with_initializer(VM_ANON, stack_bottom, true, after_stack_set, NULL);

	if (success)
		if_->rsp = USER_STACK;

	return success;
}
#endif /* VM */
