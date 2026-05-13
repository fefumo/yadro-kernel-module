#ifndef RACE_TRACKER_H
#define RACE_TRACKER_H

#include <linux/blk_types.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct race_io {
  struct rb_node node;
  sector_t start;
  sector_t end;
  u64 seq;
  bool is_write;
  bool tracked;
};

struct race_tracker {
  struct rb_root root;
  spinlock_t lock;
  u64 next_seq;
  u64 active;
  u64 races;
};

void race_tracker_init(struct race_tracker *tracker);

void race_tracker_track(struct race_tracker *tracker, struct race_io *io,
                        sector_t start, sector_t sectors, bool is_write,
                        const char *new_op, const char *dev_name);

void race_tracker_untrack(struct race_tracker *tracker, struct race_io *io);

void race_tracker_snapshot(struct race_tracker *tracker, u64 *active,
                           u64 *races);

#endif
