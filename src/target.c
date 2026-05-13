#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "race_target.h"
#include "race_tracker.h"

#define DM_MSG_PREFIX "race"

struct race_c {
  struct dm_dev *dev;
  char *dev_name;
  sector_t offset;
  struct race_tracker tracker;
};

static bool op_is_write_like(enum req_op op) {
  switch (op) {
  case REQ_OP_WRITE:
  case REQ_OP_WRITE_ZEROES:
  case REQ_OP_DISCARD:
  case REQ_OP_SECURE_ERASE:
  case REQ_OP_ZONE_APPEND:
    return true;
  default:
    return false;
  }
}

static bool op_is_tracked(enum req_op op) {
  return op == REQ_OP_READ || op_is_write_like(op);
}

static const char *op_name(enum req_op op) {
  switch (op) {
  case REQ_OP_READ:
    return "read";
  case REQ_OP_WRITE:
    return "write";
  case REQ_OP_WRITE_ZEROES:
    return "write_zeroes";
  case REQ_OP_DISCARD:
    return "discard";
  case REQ_OP_SECURE_ERASE:
    return "secure_erase";
  case REQ_OP_ZONE_APPEND:
    return "zone_append";
  default:
    return "other";
  }
}

static int parse_offset(const char *arg, sector_t *offset) {
  unsigned long long value;
  int ret = kstrtoull(arg, 10, &value);

  if (ret)
    return ret;

  *offset = (sector_t)value;
  return 0;
}

static int race_ctr(struct dm_target *ti, unsigned int argc, char **argv) {
  struct race_c *rc;
  sector_t backend_sectors;
  int ret;

  if (argc != 1 && argc != 2) {
    ti->error = "Invalid argument count";
    return -EINVAL;
  }

  rc = kzalloc(sizeof(*rc), GFP_KERNEL);
  if (!rc) {
    ti->error = "Cannot allocate target context";
    return -ENOMEM;
  }

  rc->offset = 0;
  if (argc == 2) {
    ret = parse_offset(argv[1], &rc->offset);
    if (ret) {
      ti->error = "Invalid backend offset";
      kfree(rc);
      return ret;
    }
  }

  rc->dev_name = kstrdup(argv[0], GFP_KERNEL);
  if (!rc->dev_name) {
    ti->error = "Cannot allocate device name";
    kfree(rc);
    return -ENOMEM;
  }

  ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &rc->dev);
  if (ret) {
    ti->error = "Cannot open backend device";
    kfree(rc->dev_name);
    kfree(rc);
    return ret;
  }

  backend_sectors = bdev_nr_sectors(rc->dev->bdev);
  if (rc->offset > backend_sectors || ti->len > backend_sectors - rc->offset) {
    ti->error = "Target range exceeds backend device size";
    dm_put_device(ti, rc->dev);
    kfree(rc->dev_name);
    kfree(rc);
    return -EINVAL;
  }

  race_tracker_init(&rc->tracker);

  ti->private = rc;
  ti->per_io_data_size = sizeof(struct race_io);
  ti->num_flush_bios = 1;
  ti->num_discard_bios = 1;
  ti->num_secure_erase_bios = 1;
  ti->num_write_zeroes_bios = 1;

  return 0;
}

static void race_dtr(struct dm_target *ti) {
  struct race_c *rc = ti->private;

  dm_put_device(ti, rc->dev);
  kfree(rc->dev_name);
  kfree(rc);
}

static int race_map(struct dm_target *ti, struct bio *bio) {
  struct race_c *rc = ti->private;
  struct race_io *io = dm_per_bio_data(bio, sizeof(*io));
  enum req_op op = bio_op(bio);
  sector_t local_sector = dm_target_offset(ti, bio->bi_iter.bi_sector);
  sector_t sectors = bio_sectors(bio);

  memset(io, 0, sizeof(*io));

  if (op_is_tracked(op)) {
    race_tracker_track(&rc->tracker, io, local_sector, sectors,
                       op_is_write_like(op), op_name(op), rc->dev_name);
  }

  bio_set_dev(bio, rc->dev->bdev);
  bio->bi_iter.bi_sector = rc->offset + local_sector;

  return DM_MAPIO_REMAPPED;
}

static int race_end_io(struct dm_target *ti, struct bio *bio,
                       blk_status_t *error) {
  struct race_c *rc = ti->private;
  struct race_io *io = dm_per_bio_data(bio, sizeof(*io));

  race_tracker_untrack(&rc->tracker, io);

  return DM_ENDIO_DONE;
}

static void race_status(struct dm_target *ti, status_type_t type,
                        unsigned int status_flags, char *result,
                        unsigned int maxlen) {
  struct race_c *rc = ti->private;
  unsigned int sz = 0;
  u64 active;
  u64 races;

  switch (type) {
  case STATUSTYPE_INFO:
    race_tracker_snapshot(&rc->tracker, &active, &races);
    DMEMIT("backend=%s offset=%llu active=%llu races=%llu", rc->dev_name,
           (unsigned long long)rc->offset, (unsigned long long)active,
           (unsigned long long)races);
    break;
  case STATUSTYPE_TABLE:
    if (rc->offset)
      DMEMIT("%s %llu", rc->dev_name, (unsigned long long)rc->offset);
    else
      DMEMIT("%s", rc->dev_name);
    break;
  case STATUSTYPE_IMA:
    DMEMIT("target_name=mytarget,target_version=1.0.0");
    break;
  }
}

static int race_iterate_devices(struct dm_target *ti,
                                iterate_devices_callout_fn fn, void *data) {
  struct race_c *rc = ti->private;

  return fn(ti, rc->dev, rc->offset, ti->len, data);
}

struct target_type race_target = {
    .name = "mytarget",
    .version = {1, 0, 0},
    .module = THIS_MODULE,
    .ctr = race_ctr,
    .dtr = race_dtr,
    .map = race_map,
    .end_io = race_end_io,
    .status = race_status,
    .iterate_devices = race_iterate_devices,
};
