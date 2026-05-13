#include <linux/device-mapper.h>
#include <linux/init.h>
#include <linux/module.h>

#include "race_target.h"

static int __init dm_race_init(void) {
  int ret = dm_register_target(&race_target);

  if (ret < 0)
    pr_err("dm-race: failed to register target: %d\n", ret);
  else
    pr_info("dm-race: target registered\n");

  return ret;
}

static void __exit dm_race_exit(void) {
  dm_unregister_target(&race_target);
  pr_info("dm-race: target unregistered\n");
}

module_init(dm_race_init);
module_exit(dm_race_exit);

MODULE_AUTHOR("Fyodor");
MODULE_DESCRIPTION("Device-mapper target for detecting block I/O data races");
MODULE_LICENSE("GPL");
