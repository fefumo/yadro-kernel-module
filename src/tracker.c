#include <linux/printk.h>

#include "race_tracker.h"

static bool ranges_overlap(sector_t a_start, sector_t a_end, sector_t b_start,
                           sector_t b_end) {
  return a_start <= b_end && b_start <= a_end;
}

static const char *rw_name(bool is_write) {
  return is_write ? "write" : "read";
}

void race_tracker_init(struct race_tracker *tracker) {
  tracker->root = RB_ROOT;
  spin_lock_init(&tracker->lock);
  tracker->next_seq = 0;
  tracker->active = 0;
  tracker->races = 0;
}

void race_tracker_track(struct race_tracker *tracker, struct race_io *io,
                        sector_t start, sector_t sectors, bool is_write,
                        const char *new_op, const char *dev_name) {
  struct rb_node **link;
  struct rb_node *parent;
  struct rb_node *node;
  unsigned long flags;

  io->tracked = false;

  if (!sectors)
    return;

  io->start = start;
  io->end = start + sectors - 1;
  if (io->end < io->start)
    io->end = (sector_t)-1;
  io->is_write = is_write;

  spin_lock_irqsave(&tracker->lock, flags);

  io->seq = ++tracker->next_seq;

  for (node = rb_first(&tracker->root); node; node = rb_next(node)) {
    struct race_io *old = rb_entry(node, struct race_io, node);

    if (!ranges_overlap(io->start, io->end, old->start, old->end))
      continue;

    if (!io->is_write && !old->is_write)
      continue;

    tracker->races++;

    pr_warn("dm-race: race on %s: new %s [%llu..%llu] conflicts with in-flight "
            "%s [%llu..%llu]\n",
            dev_name, new_op, (unsigned long long)io->start,
            (unsigned long long)io->end, rw_name(old->is_write),
            (unsigned long long)old->start, (unsigned long long)old->end);
  }

  link = &tracker->root.rb_node;
  parent = NULL;

  while (*link) {
    struct race_io *cur;

    parent = *link;
    cur = rb_entry(parent, struct race_io, node);

    if (io->start < cur->start)
      link = &parent->rb_left;
    else if (io->start > cur->start)
      link = &parent->rb_right;
    else if (io->seq < cur->seq)
      link = &parent->rb_left;
    else
      link = &parent->rb_right;
  }

  rb_link_node(&io->node, parent, link);
  rb_insert_color(&io->node, &tracker->root);

  io->tracked = true;
  tracker->active++;

  spin_unlock_irqrestore(&tracker->lock, flags);
}

void race_tracker_untrack(struct race_tracker *tracker, struct race_io *io) {
  unsigned long flags;

  spin_lock_irqsave(&tracker->lock, flags);

  if (io->tracked) {
    rb_erase(&io->node, &tracker->root);
    io->tracked = false;
    tracker->active--;
  }

  spin_unlock_irqrestore(&tracker->lock, flags);
}

void race_tracker_snapshot(struct race_tracker *tracker, u64 *active,
                           u64 *races) {
  unsigned long flags;

  spin_lock_irqsave(&tracker->lock, flags);
  *active = tracker->active;
  *races = tracker->races;
  spin_unlock_irqrestore(&tracker->lock, flags);
}
