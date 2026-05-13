#!/usr/bin/env bash

# Exit on error, on unset variables, and on errors inside pipelines.
set -euo pipefail

# Size of the virtual device in 512-byte sectors.
# Default: 2097152 sectors = 1 GiB.
SIZE=${SIZE:-2097152}

# File that will be used as a fake block device through loopback.
BACKING_FILE=${BACKING_FILE:-/tmp/dm-race-backend.img}

# Name of the device-mapper device.
# The resulting block device will be /dev/mapper/my0 by default.
DM_NAME=${DM_NAME:-my0}

cleanup() {
  # Cleanup must continue even if some commands fail.
  set +e

  # Remove the device-mapper device if it exists.
  sudo dmsetup remove "$DM_NAME" >/dev/null 2>&1

  # Detach the loop device if it was created.
  if [[ -n "${LOOP_DEV:-}" ]]; then
    sudo losetup -d "$LOOP_DEV" >/dev/null 2>&1
  fi
}

# Always run cleanup when the script exits, even after an error.
trap cleanup EXIT

# Create a 1 GiB zero-filled file that will act as backend storage.
dd if=/dev/zero of="$BACKING_FILE" bs=1M count=1024 status=none

# Attach the backend file to the first free loop device.
# Example result: /dev/loop0.
LOOP_DEV=$(sudo losetup --find --show "$BACKING_FILE")

# Load the kernel module.
# If it is already loaded, ignore the error and continue.
sudo insmod ./dm-race.ko 2>/dev/null || true

# Create /dev/mapper/$DM_NAME.
# All I/O to this virtual device will go through the mytarget DM target
# and then be remapped to the loop backend device.
sudo dmsetup create "$DM_NAME" --table "0 $SIZE mytarget $LOOP_DEV"

# Clear kernel log so test output is easier to read.
# Ignore failure because clearing dmesg may be restricted on some systems.
sudo dmesg -C || true

# Run many overlapping writes in parallel.
# This makes it much more likely that several BIOs will be in-flight at once.
for i in $(seq 1 200); do
  sudo dd oflag=direct if=/dev/urandom of="/dev/mapper/$DM_NAME" bs=32k count=1 seek=4 status=none &
  sudo dd oflag=direct if=/dev/urandom of="/dev/mapper/$DM_NAME" bs=8k count=1 seek=17 status=none &
done

wait || true

# Print device-mapper target status:
# backend device, offset, active in-flight requests, and detected race count.
sudo dmsetup status "$DM_NAME"

# Print race detector messages from the kernel log.
# Ignore grep failure because no race may be detected on a very fast backend.
sudo dmesg | grep -E "dm-race|race on" || true
