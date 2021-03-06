#ifndef CODECOV_H
#define CODECOV_H

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/user.h>
#include <stdio.h>
#include <stdlib.h>

#define COV_IOC_MAGIC		'z'
#define COV_COUNT_HIT		_IOWR(COV_IOC_MAGIC, 0, unsigned long)
#define COV_COUNT_CP		_IOWR(COV_IOC_MAGIC, 1, unsigned long)
#define COV_ADD_CP		_IOWR(COV_IOC_MAGIC, 2, unsigned long)
#define COV_DEL_CP		_IOWR(COV_IOC_MAGIC, 3, unsigned long)
#define COV_RESTART_CP		_IOWR(COV_IOC_MAGIC, 4, unsigned long)
#define COV_REGISTER		_IOWR(COV_IOC_MAGIC, 5, unsigned long)
#define COV_UNREGISTER		_IOWR(COV_IOC_MAGIC, 6, unsigned long)
#define COV_GET_BUFFER		_IOWR(COV_IOC_MAGIC, 7, unsigned long)
#define COV_PATH_COUNT		_IOWR(COV_IOC_MAGIC, 8, unsigned long)
#define COV_NEXT_UNHIT_FUNC	_IOWR(COV_IOC_MAGIC, 9, unsigned long)
#define COV_NEXT_UNHIT_CP	_IOWR(COV_IOC_MAGIC, 10, unsigned long)
#define COV_PATH_MAP		_IOWR(COV_IOC_MAGIC, 11, unsigned long)
#define COV_CHECK		_IOWR(COV_IOC_MAGIC, 12, unsigned long)
#define COV_GET_CP_STATUS	_IOWR(COV_IOC_MAGIC, 13, unsigned long)
#define COV_CP_XSTATE		_IOWR(COV_IOC_MAGIC, 14, unsigned long)
#define COV_THREAD_EFFECTIVE	_IOWR(COV_IOC_MAGIC, 15, unsigned long)

struct checkpoint {
	size_t name_len;
	char *name;
	size_t func_len;
	char *func;
	unsigned long offset;
	unsigned long level;
	int _auto;
};

#define THREAD_BUFFER_SIZE	PAGE_SIZE*0x10
#define	FUNC_IN_STR		">>>"
#define	FUNC_OUT_STR		"<<<"
#define	INFUNC_IN_STR		"-->"
#define	TAB_STR			"--- "
#define	TAB_IN_STR		"->> "
#define	TAB_OUT_STR		"<<- "
#define	TAB_INFUNC_STR		"=>> "
struct buffer_user {
	char *buffer;
	size_t len;
};

enum status_opt { STATUS_HIT, STATUS_LEVEL, STATUS_ENABLED, };

extern int checkpoint_add(char *name, char *func, unsigned long offset,
			  unsigned long level, int _auto);
extern int checkpoint_del(char *name);
extern int get_numhit(unsigned long *num_hit);
extern int get_numtotal(unsigned long *num_total);
extern int get_coverage(double *percent);
extern int checkpoint_restart(void);
extern int cov_register(unsigned long id, int is_test_case);
extern int cov_check(void);
extern int cov_unregister(void);
extern int cov_get_buffer(char *buffer, size_t len);
extern int cov_buffer_print(void);
extern int cov_buffer_print_pretty(void);
extern int cov_buffer_clear(void);
extern int cov_path_count(unsigned long *count);
extern int get_cp_status(char *name, enum status_opt option, unsigned long *value);
extern int get_next_unhit_func(char *buf, size_t len, size_t skip,
			       unsigned long level);
extern int get_next_unhit_cp(char *buf, size_t len, size_t skip,
			     unsigned long level);
extern int get_path_map(char *buf, size_t *len);
extern void output_path_map(char *buf, size_t len);
extern int checkpoint_xstate(char *name, size_t len, unsigned long enable,
			     unsigned long subpath);
extern int checkpoint_xstate_all(unsigned long enable);
extern int cov_thread_effective(void);

#endif
