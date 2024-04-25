struct sud_entry {
	struct rb_node node;
    struct sud_entry *next;
    pid_t tid_real;

    /* Per-task SUD data */
    unsigned long mode;
    unsigned long selector;
    unsigned long offset;
    unsigned long len;
};

extern struct sud_entry *sud_lookup(pid_t tid_real, bool create, bool mandatory);
#define sud_find_entry(tid_real) sud_lookup(tid_real, false, true)
extern int sud_collect_entry(pid_t tid_real, unsigned int mode);