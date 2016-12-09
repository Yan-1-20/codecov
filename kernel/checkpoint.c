#include "checkpoint.h"

struct list_head cproot;	/* all checkpoint we have probed */
rwlock_t cproot_rwlock;

void checkpoint_init(void)
{
	INIT_LIST_HEAD(&cproot);
	rwlock_init(&cproot_rwlock);
}

static int name_in_cplist(char *name)
{
	struct checkpoint *tmp;

	read_lock(&cproot_rwlock);
	list_for_each_entry(tmp, &cproot, siblings) {
		if ((strlen(name) == strlen(tmp->name)) &&
			!strcmp(tmp->name, name)) {
			read_unlock(&cproot_rwlock);
			return 1;
		}
	}
	read_unlock(&cproot_rwlock);

	return 0;
}

int (*lookup_symbol_attrs_f)(unsigned long, unsigned long *,
			     unsigned long *, char *, char *);
void (*insn_init_f)(struct insn *, const void *, int, int);
void (*insn_get_length_f)(struct insn *);
static int do_checkpoint_add(char *name, char *func, unsigned long offset);

static void do_set_jxx_probe(char *func, unsigned long base,
			     struct insn *insn, int opcs)
{
	void *addr = (char *)insn->kaddr;
	void *next_addr = (char *)addr + insn->length;
	void *addr_jmp;
	char name_tmp[KSYM_NAME_LEN];

	/*
	 * TODO: where this instruction jump to?
	 * if @opcs == 1, then Jbs
	 * if @opcs == 2, then Jvds, four bytes
	 * calculate the next address and the jump address
	 */
	signed int offset;

	if (opcs == 1) {
		offset = (signed int)(insn->immediate.bytes[0]);
		addr_jmp = (void *)((unsigned long)next_addr + offset);
	} else if (opcs == 2) {
		switch (insn->immediate.nbytes) {
		case 1:
			offset = (signed int)insn->immediate.bytes[0];
			break;
		case 2:
			offset = *(signed short *)(insn->immediate.bytes);
			break;
		case 4:
			offset = *(signed int *)(insn->immediate.bytes);
			break;
		default:
			offset = 0;
		}
		addr_jmp = (void *)((unsigned long)next_addr + offset);
	} else
		return;

	if ((next_addr <= (void *)base) || (addr_jmp <= (void *)base))
		return;

	/* now we have two addresses: next_addr, addr_jmp */
	memset(name_tmp, 0, KSYM_NAME_LEN);
	snprintf(name_tmp, KSYM_NAME_LEN, "%s#%lx", func,
			(unsigned long)next_addr-base);
	do_checkpoint_add(name_tmp, func, (unsigned long)next_addr-base);
	memset(name_tmp, 0, KSYM_NAME_LEN);
	snprintf(name_tmp, KSYM_NAME_LEN, "%s#%lx", func,
			(unsigned long)addr_jmp-base);
	do_checkpoint_add(name_tmp, func, (unsigned long)addr_jmp-base);
	return;
}

/*
 * do_auto_add: parse the @func, and add checkpoint automatic
 * @func: the function need to parse
 * we try our best to add more checkpoints
 * as of now, only support X86_64
 */
static void do_auto_add(char *func)
{
#ifdef CONFIG_X86_64
	char name[KSYM_NAME_LEN], modname[KSYM_NAME_LEN];
	unsigned long addr, size, offset, i = 0;
	struct insn insn;
	int err;

	addr = (unsigned long)kallsyms_lookup_name(func);
	if (unlikely(!addr))
		return;

	if (unlikely(!lookup_symbol_attrs_f))
		lookup_symbol_attrs_f = (void *)kallsyms_lookup_name(
						"lookup_symbol_attrs");
	if (unlikely(!lookup_symbol_attrs_f))
		return;
	if (unlikely(!insn_init_f))
		insn_init_f = (void *)kallsyms_lookup_name("insn_init");
	if (unlikely(!insn_init_f))
		return;
	if (unlikely(!insn_get_length_f))
		insn_get_length_f = (void *)kallsyms_lookup_name(
						"insn_get_length");
	if (unlikely(!insn_get_length_f))
		return;

	err = lookup_symbol_attrs_f(addr, &size, &offset, modname, name);
	if (err)
		return;

	while (i < size) {
		unsigned char opcode_bytes, opc0, opc1;

		insn_init_f(&insn, (void *)(addr+i), MAX_INSN_SIZE, 1);
		insn_get_length_f(&insn);
		if (!insn.length)
			break;

		opcode_bytes = insn.opcode.nbytes;
		opc0 = insn.opcode.bytes[0];
		opc1 = insn.opcode.bytes[1];

		if ((opcode_bytes == 1) && ((opc0 == JCXZ_OPC) ||
					((opc0 <= JG_OPC0) && (opc0 >= JO_OPC0))))
			do_set_jxx_probe(func, (unsigned long)addr, &insn, 1);
		else if ((opcode_bytes == 2) && (opc0 == TWO_OPC) &&
				((opc1 <= JG_OPC1) && (opc1 >= JO_OPC1)))
			do_set_jxx_probe(func, (unsigned long)addr, &insn, 2);
		i += insn.length;
	}
#endif
	return;
}

/*
 * alloc a new `checkpoint` and initialised with the given arguments
 * @name: the name of this checkpoint, if use offset, please name it [name]_offxxxx
 * @function: the name of the function will be probed
 * @offset: the offset of the function will be probed
 *
 * we should check if the name is already exist in the cproot;
 */
static int do_checkpoint_add(char *name, char *func, unsigned long offset)
{
	int err;
	struct checkpoint *new;
	size_t name_len;

	if (name_in_cplist(name))
		return -EEXIST;

	new = kmalloc(sizeof(struct checkpoint), GFP_KERNEL);
	if (unlikely(!new))
		return -ENOMEM;
	memset(new, 0, sizeof(struct checkpoint));

	err = -EINVAL;
	name_len = strlen(name);
	if (unlikely(name_len > NAME_LEN_MAX))
		goto err_free;
	err = -ENOMEM;
	new->name = kmalloc(name_len+1, GFP_KERNEL);
	if (unlikely(!new->name))
		goto err_free;
	memset(new->name, 0, name_len+1);
	memcpy(new->name, name, name_len);

	/*
	 * now deal with the probe.
	 * If the offset not zero, we alloc a kprobe,
	 * otherwise, we alloc a kretprobe
	 */
	if (unlikely(offset)) {
		new->this_kprobe = kmalloc(sizeof(struct kprobe), GFP_KERNEL);
		if (unlikely(!new->this_kprobe))
			goto err_free2;
		memset(new->this_kprobe, 0, sizeof(struct kprobe));

		new->this_kprobe->pre_handler = cp_default_kp_prehdl;
		new->this_kprobe->symbol_name = func;
		new->this_kprobe->offset = offset;

		if ((err = register_kprobe(new->this_kprobe)))
			goto err_free3;
	} else {
		new->this_retprobe = kmalloc(sizeof(struct kretprobe), GFP_KERNEL);
		if (unlikely(!new->this_retprobe))
			goto err_free2;
		memset(new->this_retprobe, 0, sizeof(struct kretprobe));

		new->this_retprobe->entry_handler = cp_default_ret_entryhdl;
		new->this_retprobe->maxactive = RETPROBE_MAXACTIVE;
		new->this_retprobe->handler = cp_default_ret_hdl;
		new->this_retprobe->kp.addr =
				(kprobe_opcode_t *)kallsyms_lookup_name(func);

		if ((err = register_kretprobe(new->this_retprobe)))
			goto err_free3;
	}

	rwlock_init(&new->caller_rwlock);
	INIT_LIST_HEAD(&new->caller);

	write_lock(&cproot_rwlock);
	list_add_tail(&new->siblings, &cproot);
	write_unlock(&cproot_rwlock);
	return 0;

err_free3:
	kfree(new->this_retprobe);
	kfree(new->this_kprobe);
err_free2:
	kfree(new->name);
err_free:
	kfree(new);
	return err;
}

int checkpoint_add(char *name, char *func, unsigned long offset)
{
	int err = do_checkpoint_add(name, func, offset);
	if (!err && !offset)
		do_auto_add(func);
	return err;
}

static void checkpoint_caller_cleanup(struct checkpoint *cp)
{
	struct checkpoint_caller *tmp, *next;

	write_lock(&cp->caller_rwlock);
	list_for_each_entry_safe(tmp, next, &cp->caller, caller_list) {
		list_del(&tmp->caller_list);
		kfree(tmp);
	}
	INIT_LIST_HEAD(&cp->caller);
	write_unlock(&cp->caller_rwlock);
}

void checkpoint_del(char *name)
{
	struct checkpoint *tmp;

	/*
	 * XXX:here we do not need list_for_each_entry_safe,
	 * because when we find the target checkpoint, we jump out the loop
	 */
	write_lock(&cproot_rwlock);
	list_for_each_entry(tmp, &cproot, siblings) {
		if ((strlen(name) == strlen(tmp->name)) &&
			!strcmp(tmp->name, name)) {
			list_del(&tmp->siblings);
			checkpoint_caller_cleanup(tmp);

			if (unlikely(tmp->this_kprobe)) {
				unregister_kprobe(tmp->this_kprobe);
				kfree(tmp->this_kprobe);
			} else {
				unregister_kretprobe(tmp->this_retprobe);
				kfree(tmp->this_retprobe);
			}
			kfree(tmp->name);
			kfree(tmp);
			break;
		}
	}
	write_unlock(&cproot_rwlock);
}

void checkpoint_restart(void)
{
	struct checkpoint *tmp, *next;

	/* TODO: need to check cphit_root */

	write_lock(&cproot_rwlock);
	list_for_each_entry_safe(tmp, next, &cproot, siblings) {
		list_del(&tmp->siblings);
		checkpoint_caller_cleanup(tmp);
		if (unlikely(tmp->this_kprobe)) {
			unregister_kprobe(tmp->this_kprobe);
			kfree(tmp->this_kprobe);
		} else {
			unregister_kretprobe(tmp->this_retprobe);
			kfree(tmp->this_retprobe);
		}
		kfree(tmp->name);
		kfree(tmp);
	}
	INIT_LIST_HEAD(&cproot);
	write_unlock(&cproot_rwlock);
}

/* this function actually returns the number of functions been hit */
unsigned long checkpoint_get_numhit(void)
{
	unsigned long num = 0;
	struct checkpoint *tmp;

	read_lock(&cproot_rwlock);
	list_for_each_entry(tmp, &cproot, siblings) {
		if (tmp->hit)
			num++;
	}
	read_unlock(&cproot_rwlock);

	return num;
}

unsigned long checkpoint_count(void)
{
	unsigned long num = 0;
	struct checkpoint *tmp;

	read_lock(&cproot_rwlock);
	list_for_each_entry(tmp, &cproot, siblings)
		num++;
	read_unlock(&cproot_rwlock);

	return num;
}

unsigned long path_count(void)
{
	unsigned long num = 0;
	struct checkpoint *tmp;
	struct checkpoint_caller *tmp_caller;

	read_lock(&cproot_rwlock);
	list_for_each_entry(tmp, &cproot, siblings) {
		read_lock(&tmp->caller_rwlock);
		list_for_each_entry(tmp_caller, &tmp->caller, caller_list)
			num++;
		read_unlock(&tmp->caller_rwlock);
	}
	read_unlock(&cproot_rwlock);

	return num;
}

/*
 * get_next_unhit_func, get_next_unhit_cp, get_path_map
 * SHOULD check if current process is the only process running now.
 * and hold the task_list_rwlock until the functions return
 */
int get_next_unhit_func(char __user *buf, size_t len)
{
	int err, num = 0, found = 0;
	size_t name_len;
	struct cov_thread *ct;
	struct checkpoint *cp;

	err = -EBUSY;
	read_lock(&task_list_rwlock);
	list_for_each_entry(ct, &task_list_root, list) {
		num++;
		if (num > 1)
			goto unlock_ret;
	}
	err = -EINVAL;
	if (!num)
		goto unlock_ret;

	read_lock(&cproot_rwlock);
	list_for_each_entry(cp, &cproot, siblings) {
		if (!cp->hit)
			if (!strchr(cp->name, '#')) {
				found = 1;
				break;
			}
	}
	read_unlock(&cproot_rwlock);
	err = -ENOENT;
	if (!found)
		goto unlock_ret;

	err = -EINVAL;
	name_len = strlen(cp->name) + 1;
	if (len < name_len)
		goto unlock_ret;

	err = 0;
	if (copy_to_user(buf, cp->name, name_len))
		err = -EFAULT;

unlock_ret:
	read_unlock(&task_list_rwlock);
	return err;
}

int get_next_unhit_cp(char __user *buf, size_t len)
{
	int err, num = 0, found = 0;
	size_t name_len;
	struct cov_thread *ct;
	struct checkpoint *cp;

	err = -EBUSY;
	read_lock(&task_list_rwlock);
	list_for_each_entry(ct, &task_list_root, list) {
		num++;
		if (num > 1)
			goto unlock_ret;
	}
	err = -EINVAL;
	if (!num)
		goto unlock_ret;

	read_lock(&cproot_rwlock);
	list_for_each_entry(cp, &cproot, siblings) {
		if (!cp->hit) {
			found = 1;
			break;
		}
	}
	read_unlock(&cproot_rwlock);
	err = -ENOENT;
	if (!found)
		goto unlock_ret;

	err = -EINVAL;
	name_len = strlen(cp->name) + 1;
	if (len < name_len)
		goto unlock_ret;

	err = 0;
	if (copy_to_user(buf, cp->name, name_len))
		err = -EFAULT;

unlock_ret:
	read_unlock(&task_list_rwlock);
	return err;
}

/*
 * if *len is not enough, we return -ERETY, userspace should check the new len
 */
int get_path_map(char __user *buf, size_t __user *len)
{
	size_t len_need = 0, len_tmp, copid = 0;
	int err, num = 0;
	struct cov_thread *ct;
	struct checkpoint *cp;
	char *buf_tmp = NULL;

	err = -EBUSY;
	read_lock(&task_list_rwlock);
	list_for_each_entry(ct, &task_list_root, list) {
		num++;
		if (num > 1)
			goto unlock_ret;
	}
	err = -EINVAL;
	if (!num)
		goto unlock_ret;

	err = -EFAULT;
	if (copy_from_user(&len_tmp, len, sizeof(size_t)))
		goto unlock_ret;

	/* use +: as the seperator, cp->name+caller_name+caller_addr+:NEXT */
	read_lock(&cproot_rwlock);
	list_for_each_entry(cp, &cproot, siblings) {
		struct checkpoint_caller *cpcaller;

		if (cp->name)
			len_need += strlen(cp->name);
		else
			len_need += strlen("NULL");

		read_lock(&cp->caller_rwlock);
		list_for_each_entry(cpcaller, &cp->caller, caller_list) {
			len_need += 1;		/* for + */
			if (cpcaller->name[0])
				len_need += strlen(cpcaller->name);
			else
				len_need += strlen("NULL");
			len_need += 16 + 2;	/* for 0x */
		}
		read_unlock(&cp->caller_rwlock);
		len_need += 1;	/* for : */
	}
	read_unlock(&cproot_rwlock);
	len_need += 1;	/* for \0 */

	err = -EAGAIN;
	if (len_tmp < len_need) {
		if (copy_to_user(len, &len_need, sizeof(size_t)))
			err = -EFAULT;
		goto unlock_ret;
	}

	err = -ENOMEM;
	buf_tmp = kmalloc(len_need, GFP_KERNEL);
	if (!buf_tmp)
		goto unlock_ret;
	memset(buf_tmp, 0, len_need);

	read_lock(&cproot_rwlock);
	list_for_each_entry(cp, &cproot, siblings) {
		struct checkpoint_caller *cpcaller;

		if (cp->name) {
			memcpy(buf_tmp+copid, cp->name, strlen(cp->name));
			copid += strlen(cp->name);
		} else {
			memcpy(buf_tmp,"NULL", 4);
			copid += 4;
		}

		read_lock(&cp->caller_rwlock);
		list_for_each_entry(cpcaller, &cp->caller, caller_list) {
			memcpy(buf_tmp+copid, "+", 1);
			copid += 1;

			if (cpcaller->name[0]) {
				memcpy(buf_tmp+copid, cpcaller->name,
						strlen(cpcaller->name));
				copid += strlen(cpcaller->name);
			} else {
				memcpy(buf_tmp+copid, "NULL", 4);
				copid += 4;
			}
			sprintf(buf_tmp+copid, "0x%016lx", cpcaller->address);
			copid += 16 + 2;
		}
		read_unlock(&cp->caller_rwlock);
		memcpy(buf_tmp+copid, ":", 1);
		copid += 1;
	}
	read_unlock(&cproot_rwlock);

	err = 0;
	if (copy_to_user(buf, buf_tmp, copid+1))
		err = -EFAULT;

unlock_ret:
	read_unlock(&task_list_rwlock);
	kfree(buf_tmp);
	return err;
}

void checkpoint_exit(void)
{
	checkpoint_restart();
}
