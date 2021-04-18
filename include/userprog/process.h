#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "filesys/file.h"

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_, int* parent_lock);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

struct parent_info {
  struct thread* t;
  struct intr_frame* if_;
  int* parent_lock;
};

struct file_elem {
  struct list_elem elem;
  struct file* file;
  unsigned int fd;
  bool open;
  int reference;
};

struct file {
	struct inode *inode;        /* File's inode. */
	off_t pos;                  /* Current position. */
	bool deny_write;            /* Has file_deny_write() been called? */
};

#endif /* userprog/process.h */
