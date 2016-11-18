#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/types.h>

#define NAME_LEN_MAX	32
#define FUNC_LEN_MAX	1024
#define RETPROBE_MAXACTIVE	16

extern struct list_head cproot;
struct checkpoint;
struct checkpoint {
	struct checkpoint *parent;	/* who called this checkpoint */
	struct list_head siblings;

	unsigned long hit;		/* numbers been hit */
	char *name;			/* checkpoint's specific name */
	struct kprobe *this_kprobe;	/* the probe this checkpoint using */
	struct kretprobe *this_retprobe;/* against to this_kprobe */
};

struct checkpoint_user {
	size_t name_len;
	char *name;
	size_t func_len;
	char *func;
	unsigned long offset;
};

extern int cp_default_kp_prehdl(struct kprobe *kp, struct pt_regs *reg);
extern int cp_default_ret_hdl(struct kretprobe_instance *ri,
			      struct pt_regs *regs);
extern int cp_default_ret_entryhdl(struct kretprobe_instance *ri,
				   struct pt_regs *regs);

extern void checkpoint_init(void);
extern int checkpoint_add(char *name, char *func, unsigned long offset);
extern void checkpoint_del(char *name);
extern void checkpoint_restart(void);
extern void checkpoint_exit(void);
extern unsigned long checkpoint_count(void);

#endif
