#include <stdio.h>
#include <stdlib.h>
#include <linux/perf_event.h>
#include <string.h>
#include <asm/unistd.h>		// __NR_perf_event_open
#include <locale.h>
#include <errno.h>

/*
 * TODO: Hard coded for now.  These bit masks are exposed in the files
 * 	'domain', 'starting_index' 'offset', 'lpar' in the directory:
 *
 * 	 /sys/bus/event_source/devices/hv_24x7/format/
 *
 */
#define	DOMAIN_MASK		0xF		/* domain: config:0-3 */
#define DOMAIN_SHIFT		0

#define START_INDEX_MASK	0xFFFF		/* start index: config:16-31 */
#define	START_INDEX_SHIFT	16

#define OFFSET_MASK		0xFFFFFFFF	/* offset: config:32-63 */
#define OFFSET_SHIFT		32

#define LPAR_MASK		0xFF		/* lpar: config1: 0-15 */
#define LPAR_SHIFT		0

typedef unsigned long long u64;


#if 0
#define NUM_CORES		4
#else
#define NUM_CORES		16
#endif

init_attr(struct perf_event_attr *attr, int domain, unsigned long offset,
			int start, int lpar)
{
	memset(attr, 0, sizeof(struct perf_event_attr));

	/*
	 * TODO Hard code type for now. We should read from the file:
	 * 	/sys/bus/event_source/devices/hv_24x7/type
	 */
	attr->type = 6;
	attr->size = sizeof(struct perf_event_attr);

	attr->config = attr->config1 = attr->config2 = (u64)0;
	attr->config |= ((domain & DOMAIN_MASK) << DOMAIN_SHIFT);
	attr->config |= ((start & START_INDEX_MASK) << START_INDEX_SHIFT);
	attr->config |= ((offset & OFFSET_MASK) << OFFSET_SHIFT);

	attr->config1 |= (lpar & LPAR_MASK) << LPAR_SHIFT;

	attr->read_format |= PERF_FORMAT_GROUP;
	attr->read_format |= PERF_FORMAT_TOTAL_TIME_ENABLED;
	attr->read_format |= PERF_FORMAT_TOTAL_TIME_RUNNING;
	attr->read_format |= PERF_FORMAT_ID;

	return;
}

int compute_read_size(struct perf_event_attr *attr, int total_events)
{
	int n;
	
	n = 1;	// number of siblings
	if (attr->read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
		n++;
	if (attr->read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
		n++;

	n += total_events;
	if (attr->read_format & PERF_FORMAT_ID)
		n += total_events;

	n *= sizeof(u64);

	printf("read_size %d\n", n);

	return n;
}
int sys_perf_event_open(struct perf_event_attr *attr,
				int pid, int cpu, int group, long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu, group, flags);
}


/*
 * TODO: Hard code event name and offset for now. We should actually
 * 	 read them from the catalog:
 *
 * 	 	/sys/bus/event_source/devices/hv_24x7/interface/catalog
 *
 *	Ignore the "__PHYS_CORE" suffix that we need when specifying
 *	event to perf tool command line.
 *
 *	For now check each event's offset from its file in sysfs and update
 *	this table manually. Eg:
 *
 *	$ cat /sys/bus/event_source/devices/hv_24x7/events/HPM_CCYC__PHYS_CORE
 *	domain=0x2,offset=0x98,core=?,lpar=0x0
 *	           ^^^^^^^^^^^
 *	Remember to add the __pHYS_CORE suffix to the test event, when finding
 *	the offset.
 */
struct event_desc {
	int   offset;
	char *name;
} test_events[] = {
	{ 0x1b58, "HPM_CS_FROM_L2_IFETCH_KERNEL" },
	{ 0x1920, "HPM_CS_PURR_KERNEL" },
	{ 0x98,   "HPM_CCYC" },
	{ 0x20,   "HPM_INST" },
};
	
/*
 * Map event ids assigned by perf_event_open() to the event name.
 */
struct event_idmap {
	u64 id;
	char *name;
	int core;
};

static void set_idmap(struct event_idmap *idmap, int nmaps, int te, int core,
			u64 id)
{
	int i;

	for (i = 0; i < nmaps; i++) {
		if (idmap[i].id == 0ULL) {
			idmap[i].id = id;
			idmap[i].name = test_events[te].name;
			idmap[i].core = core;
			return;
		}
	}
}

static struct event_idmap *get_idmap(struct event_idmap *idmap, int nmaps,
			u64 id)
{
	int i;

	for (i = 0; i < nmaps; i++) {
		if (idmap[i].id == id)
			return &idmap[i];
	}
	return NULL;

}

main(int argc, char *argv[])
{
	int domain, offset, core, lpar;
	int i, j, k, n, fd[NUM_CORES], pid, cpu, group;
	int n_test_events, total_events, nmaps, read_size;

	u64 *counters;
	u64 read_format, count, id;

	unsigned long flags;
	struct perf_event_attr attr;
	struct event_idmap *idmap;

	cpu = 1;			// monitor specified CPU
	if (argc > 1)
		cpu = atoi(argv[1]);

	setlocale(LC_NUMERIC, "");	// print big numbers with commas
	pid = -1;			// Must be -1 - cannot monitor a task
	group = -1;			// -1 for group leader event
	flags = 0UL;

	/*
	 * Hard code domain and lpar for now. We could get them from
	 * command line
	 */
	domain = 2;
	lpar = 0;

	n_test_events = sizeof(test_events) / sizeof(struct event_desc);
	total_events = n_test_events * NUM_CORES;
	nmaps = sizeof(struct event_idmap) * total_events;

	idmap = malloc(nmaps);
	if (!idmap) {
		printf("malloc() idmap failed\n");
		_Exit(1);
	}
	memset(idmap, 0, nmaps);

	/*
	 * Prepare to monitor each test event on all cores that
	 * we are interested in.
	 */
	for (k = 0; k < n_test_events; k++) {
		offset = test_events[k].offset;
		for (core = 0; core < NUM_CORES; core++) {
			init_attr(&attr, domain, offset, core, lpar);

			fd[core] = sys_perf_event_open(&attr, pid, cpu, group,
							flags);
			if (fd[core] < 0) {
				perror("perf_event_open()");
				_Exit(1);
			}

			if (group == -1)
				group = fd[core];

			/* Enable event id so we can retreive event name */
			if (ioctl(fd[core], PERF_EVENT_IOC_ID, &id) < 0) {
				printf("Error getting EVENT ID\n", id);
				_Exit(1);
			}
			set_idmap(idmap, nmaps, k, core, id);
		}
	}

	read_size = compute_read_size(&attr, total_events);
	counters = malloc(read_size);
	printf("Read size is %d, total_events %d\n", read_size, total_events);
	if (!counters) {
		printf("Unable to allocate %d bytes for counters\n", read_size);
		_Exit(1);
	}

	while(1) {
		sleep(2);

		memset(counters, 0, read_size);
		errno = 0;
		n = read(fd[0], counters, read_size);
		if (n < (int)read_size) {
			printf("Read returned %d, error %s\n", n,
							strerror(errno));
			continue;
		}
		j = 0;
		if (counters[j++] != total_events) {
			printf("Incorrect # of counters?  Exp %d, got %d\n",
					total_events, (int)counters[j]);
			_Exit(-1);
		}
		/*
		 * counters[] layout:
		 * 0: nr_siblings
		 * 1: enabled time (if PERF_FORMAT_TOTAL_TIME_ENABLED set)
		 * 2: running time (if PERF_FORMAT_TOTAL_TIME_RUNNING set)
		 * 3: leader's counter value
		 * 4: leader's id if PERF_FORMAT_ID is set)
		 *
		 * 5: sibling 1 counter value (incl its children)
		 * 6: event ID (if PERF_FORMAT_ID is set)
		 *
		 * ...
		 *
		 * X: sibling N counter value (incl its children)
		 * Y: event ID (if PERF_FORMAT_ID is set)
		 */
		printf("===== ");
		if (attr.read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
			printf("Enabled: %llu, ", counters[j++]);
		if (attr.read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
			printf("Running: %llu", counters[j++]);

		printf("\n-------------------------------------------------\n");
		printf("\tCounter");
		printf("\t\t[Event, Core]");
		printf("\n-------------------------------------------------\n");

		count = counters[j++];

		if (attr.read_format & PERF_FORMAT_ID)
			id = counters[j++];

		if (count) {
			struct event_idmap *map;

			map = get_idmap(idmap, nmaps, id);
			printf("\t%'llu", count);
			printf("\t\t[%s, %d]", map->name, map->core);
			printf("\n");
		}

		/* done processing leader, now process sibling events */
		for (i = 1; i < total_events; i++) {
			count = counters[j++];
			if (attr.read_format & PERF_FORMAT_ID)
				id = counters[j++];

			if (count) {
				struct event_idmap *map;

				map = get_idmap(idmap, nmaps, id);
				printf("\t%'llu", count);
				printf("\t\t[%s, %d]", map->name, map->core);
				printf("\n");
			}
		}
	}
}
