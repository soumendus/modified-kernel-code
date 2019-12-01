
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This is a test "dust" device, which fails reads on specified
 * sectors, emulating the behavior of a hard disk drive sending
 * a "Read Medium Error" sense.
 *
 * 
 *
 *
 * Soumendu Sekhar Satapathy email satapathy.soumendu@gmail.com
 * 30th Nov 2019
 * I  have modified  the  ~/drivers/md/dm-dust.c  file present in the 
 * linux kernel stable ver 5.4.1 (version as of writing this comment) 
 * and have added functionality for emulating disk with "write errors" 
 * also.  I  needed "write error"  functionality  for my  test  suite, 
 * so I have added  this  functionality to  the existing code.  
 *
 * Soumendu Sekhar Satapathy email satapathy.soumendu@gmail.com
 * 01 Dec 2019
 * Did some code cleanups
 *
 */

#include <linux/device-mapper.h>
#include <linux/module.h>
#include <linux/rbtree.h>

#define DM_MSG_PREFIX "dust"

#define RD false
#define WR true

struct badblock {
	struct rb_node node;
	sector_t bb;
	unsigned char wr_fail_cnt;
};

struct dust_device {
	struct dm_dev *dev;
	struct rb_root badblocklist_read;
	struct rb_root badblocklist_write;
	unsigned long long badblock_count_read;
	unsigned long long badblock_count_write;
	spinlock_t dust_lock;
	unsigned int blksz;
	int sect_per_block_shift;
	unsigned int sect_per_block;
	sector_t start;
	bool fail_write_on_bb:1;
	bool fail_read_on_bb:1;
	bool quiet_mode:1;
};

static struct badblock *dust_rb_search(struct rb_root *root, sector_t blk)
{
	struct rb_node *node = root->rb_node;

	while (node) {
		struct badblock *bblk = rb_entry(node, struct badblock, node);

		if (bblk->bb > blk)
			node = node->rb_left;
		else if (bblk->bb < blk)
			node = node->rb_right;
		else
			return bblk;
	}

	return NULL;
}


static bool dust_rb_insert(struct rb_root *root, struct badblock *new)
{
	struct badblock *bblk;
	struct rb_node **link = &root->rb_node, *parent = NULL;
	sector_t value = new->bb;

	while (*link) {
		parent = *link;
		bblk = rb_entry(parent, struct badblock, node);

		if (bblk->bb > value)
			link = &(*link)->rb_left;
		else if (bblk->bb < value)
			link = &(*link)->rb_right;
		else
			return false;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, root);

	return true;
}

static int dust_remove_block(struct dust_device *dd, unsigned long long block, bool mode)
{
	struct badblock *bblock;
	unsigned long flags;

	spin_lock_irqsave(&dd->dust_lock, flags);
	if(mode == RD)
		bblock = dust_rb_search(&dd->badblocklist_read, block);
	else
		bblock = dust_rb_search(&dd->badblocklist_write, block);

	if (bblock == NULL) {
		if (!dd->quiet_mode) {
			DMERR("%s: block %llu not found in badblocklist_read",
			      __func__, block);
		}
		spin_unlock_irqrestore(&dd->dust_lock, flags);
		return -EINVAL;
	}

	if(mode == RD)
		rb_erase(&bblock->node, &dd->badblocklist_read);
	else
		rb_erase(&bblock->node, &dd->badblocklist_write);
	if(mode == RD)
		dd->badblock_count_read--;
	else
		dd->badblock_count_write--;
	if (!dd->quiet_mode)
		DMINFO("%s: badblock removed at block %llu", __func__, block);
	kfree(bblock);
	spin_unlock_irqrestore(&dd->dust_lock, flags);

	return 0;
}

static int dust_add_block(struct dust_device *dd, unsigned long long block,
			  unsigned char wr_fail_cnt, bool mode)
{
	struct badblock *bblock;
	unsigned long flags;

	bblock = kmalloc(sizeof(*bblock), GFP_KERNEL);
	if (bblock == NULL) {
		if (!dd->quiet_mode)
			DMERR("%s: badblock allocation failed", __func__);
		return -ENOMEM;
	}

	spin_lock_irqsave(&dd->dust_lock, flags);
	bblock->bb = block;
	bblock->wr_fail_cnt = wr_fail_cnt;
	if(mode == RD) {
		if (!dust_rb_insert(&dd->badblocklist_read, bblock)) {
			if (!dd->quiet_mode) {
				DMERR("%s: block %llu already in badblocklist",
			      		__func__, block);
			}
			spin_unlock_irqrestore(&dd->dust_lock, flags);
			kfree(bblock);
			return -EINVAL;
		}
	}
	else {
		if (!dust_rb_insert(&dd->badblocklist_write, bblock)) {
			if (!dd->quiet_mode) {
				DMERR("%s: block %llu already in badblocklist",
			      		__func__, block);
			}
			spin_unlock_irqrestore(&dd->dust_lock, flags);
			kfree(bblock);
			return -EINVAL;
		}
	}

	if(mode == RD)
		dd->badblock_count_read++;
	else
		dd->badblock_count_read++;

	if (!dd->quiet_mode) {
		DMINFO("%s: badblock added at block %llu with write fail count %hhu",
		       __func__, block, wr_fail_cnt);
	}
	spin_unlock_irqrestore(&dd->dust_lock, flags);

	return 0;
}


static int dust_query_block(struct dust_device *dd, unsigned long long block, bool mode)
{
	struct badblock *bblock;
	unsigned long flags;

	spin_lock_irqsave(&dd->dust_lock, flags);
	if(mode == RD)
		bblock = dust_rb_search(&dd->badblocklist_read, block);
	else
		bblock = dust_rb_search(&dd->badblocklist_write, block);
	if (bblock != NULL)
		DMINFO("%s: block %llu found in badblocklist", __func__, block);
	else
		DMINFO("%s: block %llu not found in badblocklist", __func__, block);
	spin_unlock_irqrestore(&dd->dust_lock, flags);

	return 0;
}

static int __dust_map_read(struct dust_device *dd, sector_t thisblock)
{
	struct badblock *bblk = dust_rb_search(&dd->badblocklist_read, thisblock);

	if (bblk)
		return DM_MAPIO_KILL;

	return DM_MAPIO_REMAPPED;
}

static int dust_map_read(struct dust_device *dd, sector_t thisblock,
			 bool fail_read_on_bb)
{
	unsigned long flags;
	int r = DM_MAPIO_REMAPPED;

	if (fail_read_on_bb) {
		thisblock >>= dd->sect_per_block_shift;
		spin_lock_irqsave(&dd->dust_lock, flags);
		r = __dust_map_read(dd, thisblock);
		spin_unlock_irqrestore(&dd->dust_lock, flags);
	}

	return r;
}

static int __dust_map_write(struct dust_device *dd, sector_t thisblock)
{
	struct badblock *bblk_r = dust_rb_search(&dd->badblocklist_read, thisblock);
	struct badblock *bblk_w = dust_rb_search(&dd->badblocklist_write, thisblock);

	if(dd->fail_write_on_bb) {
		if (bblk_w) 
			return DM_MAPIO_KILL;
	}

	if(dd->fail_read_on_bb) {
		if (bblk_r && bblk_r->wr_fail_cnt > 0) {
			bblk_r->wr_fail_cnt--;
			return DM_MAPIO_KILL;
		}

		if (bblk_r) {
			rb_erase(&bblk_r->node, &dd->badblocklist_read);
			dd->badblock_count_read--;
			kfree(bblk_r);
			if (!dd->quiet_mode) {
				sector_div(thisblock, dd->sect_per_block);
				DMINFO("block %llu removed from badblocklist_read by write",
		       			(unsigned long long)thisblock);
			}
		}
	}

	return DM_MAPIO_REMAPPED;
}

static int dust_map_write(struct dust_device *dd, sector_t thisblock,
			  bool fail_read_on_bb, bool fail_write_on_bb)
{
	unsigned long flags;
	int ret = DM_MAPIO_REMAPPED;

	if (fail_write_on_bb) {
		thisblock >>= dd->sect_per_block_shift;
		spin_lock_irqsave(&dd->dust_lock, flags);
		ret = __dust_map_write(dd, thisblock);
		spin_unlock_irqrestore(&dd->dust_lock, flags);
	}
	else if (fail_read_on_bb) {
		thisblock >>= dd->sect_per_block_shift;
		spin_lock_irqsave(&dd->dust_lock, flags);
		ret = __dust_map_write(dd, thisblock);
		spin_unlock_irqrestore(&dd->dust_lock, flags);
	}

	return ret;
}

static int dust_map(struct dm_target *ti, struct bio *bio)
{
	struct dust_device *dd = ti->private;
	int r;

	bio_set_dev(bio, dd->dev->bdev);
	bio->bi_iter.bi_sector = dd->start + dm_target_offset(ti, bio->bi_iter.bi_sector);

	if (bio_data_dir(bio) == READ)
		r = dust_map_read(dd, bio->bi_iter.bi_sector, dd->fail_read_on_bb);
	else
		r = dust_map_write(dd, bio->bi_iter.bi_sector, dd->fail_read_on_bb, dd->fail_write_on_bb);

	return r;
}

static bool __dust_clear_badblocks(struct rb_root *tree,
				   unsigned long long count)
{
	struct rb_node *node = NULL, *nnode = NULL;

	nnode = rb_first(tree);
	if (nnode == NULL) {
		BUG_ON(count != 0);
		return false;
	}

	while (nnode) {
		node = nnode;
		nnode = rb_next(node);
		rb_erase(node, tree);
		count--;
		kfree(node);
	}
	BUG_ON(count != 0);
	BUG_ON(tree->rb_node != NULL);

	return true;
}

static int dust_clear_badblocks(struct dust_device *dd, bool mode)
{
	unsigned long flags;
	struct rb_root badblocklist_read;
	struct rb_root badblocklist_write;
	unsigned long long badblock_count_read;
	unsigned long long badblock_count_write;

	spin_lock_irqsave(&dd->dust_lock, flags);
	if(mode == RD) {
		badblocklist_read = dd->badblocklist_read;
		badblock_count_read = dd->badblock_count_read;
		dd->badblocklist_read = RB_ROOT;
		dd->badblock_count_read = 0;
	}
	else {
		badblocklist_write = dd->badblocklist_write;
		badblock_count_write = dd->badblock_count_write;
		dd->badblocklist_write = RB_ROOT;
		dd->badblock_count_write = 0;
	}
	spin_unlock_irqrestore(&dd->dust_lock, flags);

	if(mode == RD) {
		if (!__dust_clear_badblocks(&badblocklist_read, badblock_count_read))
			DMINFO("%s: no read badblocks found", __func__);
		else
			DMINFO("%s: read badblocks cleared", __func__);
	}
	else {
		if (!__dust_clear_badblocks(&badblocklist_write, badblock_count_write))
			DMINFO("%s: no write badblocks found", __func__);
		else
			DMINFO("%s: write badblocks cleared", __func__);
	}

	return 0;
}

/*
 * Target parameters:
 *
 * <device_path> <offset> <blksz>
 *
 * device_path: path to the block device
 * offset: offset to data area from start of device_path
 * blksz: block size (minimum 512, maximum 1073741824, must be a power of 2)
 */
static int dust_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct dust_device *dd;
	unsigned long long tmp;
	char dummy;
	unsigned int blksz;
	unsigned int sect_per_block;
	sector_t DUST_MAX_BLKSZ_SECTORS = 2097152;
	sector_t max_block_sectors = min(ti->len, DUST_MAX_BLKSZ_SECTORS);

	if (argc != 3) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	if (kstrtouint(argv[2], 10, &blksz) || !blksz) {
		ti->error = "Invalid block size parameter";
		return -EINVAL;
	}

	if (blksz < 512) {
		ti->error = "Block size must be at least 512";
		return -EINVAL;
	}

	if (!is_power_of_2(blksz)) {
		ti->error = "Block size must be a power of 2";
		return -EINVAL;
	}

	if (to_sector(blksz) > max_block_sectors) {
		ti->error = "Block size is too large";
		return -EINVAL;
	}

	sect_per_block = (blksz >> SECTOR_SHIFT);

	if (sscanf(argv[1], "%llu%c", &tmp, &dummy) != 1 || tmp != (sector_t)tmp) {
		ti->error = "Invalid device offset sector";
		return -EINVAL;
	}

	dd = kzalloc(sizeof(struct dust_device), GFP_KERNEL);
	if (dd == NULL) {
		ti->error = "Cannot allocate context";
		return -ENOMEM;
	}

	if (dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &dd->dev)) {
		ti->error = "Device lookup failed";
		kfree(dd);
		return -EINVAL;
	}

	dd->sect_per_block = sect_per_block;
	dd->blksz = blksz;
	dd->start = tmp;

	dd->sect_per_block_shift = __ffs(sect_per_block);

	/*
	 * Whether to fail a read on a "bad" block.
	 * Defaults to false; enabled later by message.
	 */
	dd->fail_read_on_bb = false;

	/*
	 * Fail a write on a "bad" block.
	 * Defaults to false; enabled later by message.
	 */
	dd->fail_write_on_bb = false;

	/*
	 * Initialize bad block list rbtree.
	 */
	dd->badblocklist_read = RB_ROOT;
	dd->badblock_count_read = 0;
	dd->badblocklist_write = RB_ROOT;
	dd->badblock_count_write = 0;
	spin_lock_init(&dd->dust_lock);

	dd->quiet_mode = false;

	BUG_ON(dm_set_target_max_io_len(ti, dd->sect_per_block) != 0);

	ti->num_discard_bios = 1;
	ti->num_flush_bios = 1;
	ti->private = dd;

	return 0;
}

static void dust_dtr(struct dm_target *ti)
{
	struct dust_device *dd = ti->private;

	__dust_clear_badblocks(&dd->badblocklist_read, dd->badblock_count_read);
	__dust_clear_badblocks(&dd->badblocklist_write, dd->badblock_count_write);
	dm_put_device(ti, dd->dev);
	kfree(dd);
}

static int dust_message(struct dm_target *ti, unsigned int argc, char **argv,
			char *result_buf, unsigned int maxlen)
{
	struct dust_device *dd = ti->private;
	sector_t size = i_size_read(dd->dev->bdev->bd_inode) >> SECTOR_SHIFT;
	bool invalid_msg = false;
	int r = -EINVAL;
	unsigned long long tmp, block;
	unsigned char wr_fail_cnt;
	unsigned int tmp_ui;
	unsigned long flags;
	char dummy;

	if (argc == 1) {
		if (!strcasecmp(argv[0], "addbadblock") ||
		    !strcasecmp(argv[0], "removebadblock") ||
		    !strcasecmp(argv[0], "queryblock")) {
			DMERR("%s requires 2 additional argument", argv[0]);
		} else if (!strcasecmp(argv[0], "disable")) {
			DMERR("%s requires 1 additional argument", argv[0]);
		} else if (!strcasecmp(argv[0], "enable")) {
			DMERR("%s requires 1 additional argument", argv[0]);
		} else if (!strcasecmp(argv[0], "countbadblocks")) {
			DMERR("%s requires 1 additional argument", argv[0]);
		} else if (!strcasecmp(argv[0], "clearbadblocks")) {
			DMERR("%s requires 1 additional argument", argv[0]);
		} else if (!strcasecmp(argv[0], "quiet")) {
			if (!dd->quiet_mode)
				dd->quiet_mode = true;
			else
				dd->quiet_mode = false;
			r = 0;
		} else {
			invalid_msg = true;
		}
	} else if (argc == 2) {
		if (!strcasecmp(argv[0], "addbadblock")) {
			if (!strcasecmp(argv[1], "read"))
				DMERR("%s requires 1 additional argument", argv[0]);
			else if (!strcasecmp(argv[1], "write"))
				DMERR("%s requires 1 additional argument", argv[0]);
			else
				invalid_msg = true;
		}
		else if (!strcasecmp(argv[0], "enable")) {
			if (!strcasecmp(argv[1], "read")) {
				DMINFO("enabling read failures on bad sectors");
				dd->fail_read_on_bb = true;
				r = 0;
				invalid_msg = false;
			}
			else if (!strcasecmp(argv[1], "write")) {
				DMINFO("enabling write failures on bad sectors");
				dd->fail_write_on_bb = true;
				r = 0;
				invalid_msg = false;
			}
			else
				invalid_msg = true;
		}
		else if (!strcasecmp(argv[0], "disable")) {
			if (!strcasecmp(argv[1], "read")) {
				DMINFO("disabling read failures on bad sectors");
				dd->fail_read_on_bb = false;
				r = 0;
				invalid_msg = false;
			}
			else if (!strcasecmp(argv[1], "write")) {
				DMINFO("disabling write failures on bad sectors");
				dd->fail_write_on_bb = false;
				r = 0;
				invalid_msg = false;
			}
			else
				invalid_msg = true;
		}
		else if (!strcasecmp(argv[0], "removebadblock")) {
			if (!strcasecmp(argv[1], "read"))
				DMERR("%s requires 1 additional argument", argv[0]);
			else if (!strcasecmp(argv[1], "write"))
				DMERR("%s requires 1 additional argument", argv[0]);
			else
				invalid_msg = true;
		}
		else if (!strcasecmp(argv[0], "queryblock")) {
			if (!strcasecmp(argv[1], "read"))
				DMERR("%s requires 1 additional argument", argv[0]);
			else if (!strcasecmp(argv[1], "write"))
				DMERR("%s requires 1 additional argument", argv[0]);
			else
				invalid_msg = true;
		}
		else if (!strcasecmp(argv[0], "clearbadblocks")) {
			if (!strcasecmp(argv[1], "read")) {
				r = dust_clear_badblocks(dd,false);
				invalid_msg = false;
			}
			else if (!strcasecmp(argv[1], "write")) {
				r = dust_clear_badblocks(dd,true);
				invalid_msg = false;
			}
			else
				invalid_msg = true;
		}
		else if (!strcasecmp(argv[0], "countbadblocks")) {
			if (!strcasecmp(argv[1], "read")) {
				spin_lock_irqsave(&dd->dust_lock, flags);
				DMINFO("countbadblocks: %llu read badblock(s) found",
			       		dd->badblock_count_read);
				spin_unlock_irqrestore(&dd->dust_lock, flags);
				r = 0;
				invalid_msg = false;
			}
			else if (!strcasecmp(argv[1], "write")) {
				spin_lock_irqsave(&dd->dust_lock, flags);
				DMINFO("countbadblocks: %llu write badblock(s) found",
			       		dd->badblock_count_write);
				spin_unlock_irqrestore(&dd->dust_lock, flags);
				r = 0;
				invalid_msg = false;
			}
			else
				invalid_msg = true;
		}
		else
			invalid_msg = true;
	} else if (argc == 3) {
		if (sscanf(argv[2], "%llu%c", &tmp, &dummy) != 1)
			return r;

		block = tmp;
		sector_div(size, dd->sect_per_block);
		if (block > size) {
			DMERR("selected block value out of range");
			return r;
		}

		if (!strcasecmp(argv[0], "addbadblock")) {
			if (!strcasecmp(argv[1], "read")) {
				r = dust_add_block(dd, block, 0, false);
				invalid_msg = false;
			}
			else if (!strcasecmp(argv[1], "write")) {
				r = dust_add_block(dd, block, 0, true);
				invalid_msg = false;
			}
			else
				invalid_msg = true;
		}
		else if (!strcasecmp(argv[0], "removebadblock")) {
			if (!strcasecmp(argv[1], "read")) {
				r = dust_remove_block(dd, block, false);
				invalid_msg = false;
			}
			else if (!strcasecmp(argv[1], "write")) {
				r = dust_remove_block(dd, block, true);
				invalid_msg = false;
			}
			else
				invalid_msg = true;
		}
		else if (!strcasecmp(argv[0], "queryblock")) {
			if (!strcasecmp(argv[1], "read")) {
				r = dust_query_block(dd, block, false);
				invalid_msg = false;
			}
			else if (!strcasecmp(argv[1], "write")) {
				r = dust_query_block(dd, block, true);
				invalid_msg = false;
			}
			else
				invalid_msg = true;
		}
		else
			invalid_msg = true;
	} else if (argc == 4) {
	        if (sscanf(argv[2], "%llu%c", &tmp, &dummy) != 1)
                        return r;

                if (sscanf(argv[3], "%u%c", &tmp_ui, &dummy) != 1)
                        return r;

                block = tmp;
                if (tmp_ui > 255) {
                        DMERR("selected write fail count out of range");
                        return r;
                }
                wr_fail_cnt = tmp_ui;
                sector_div(size, dd->sect_per_block);
                if (block > size) {
                        DMERR("selected block value out of range");
                        return r;
                }

                if (!strcasecmp(argv[0], "addbadblock")) {
			if (!strcasecmp(argv[1], "read")) {
				r = dust_add_block(dd, block, wr_fail_cnt, false);
				invalid_msg = false;
			}
			else if (!strcasecmp(argv[1], "write")) {
				r = dust_add_block(dd, block, wr_fail_cnt, true);
				invalid_msg = false;
			}
			else
				invalid_msg = true;
		}
                else
                        invalid_msg = true;
	} else
		DMERR("invalid number of arguments '%d'", argc);

	if (invalid_msg)
		DMERR("unrecognized message '%s' received", argv[0]);

	return r;
}

static void dust_status(struct dm_target *ti, status_type_t type,
			unsigned int status_flags, char *result, unsigned int maxlen)
{
	struct dust_device *dd = ti->private;
	unsigned int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%s %s %s", dd->dev->name,
		       dd->fail_read_on_bb ? "fail_read_on_bad_block" : "bypass",
		       dd->quiet_mode ? "quiet" : "verbose");
		DMEMIT("\n%s %s %s", dd->dev->name,
		       dd->fail_write_on_bb ? "fail_write_on_bad_block" : "bypass",
		       dd->quiet_mode ? "quiet" : "verbose");
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%s %llu %u", dd->dev->name,
		       (unsigned long long)dd->start, dd->blksz);
		break;
	}
}

static int dust_prepare_ioctl(struct dm_target *ti, struct block_device **bdev)
{
	struct dust_device *dd = ti->private;
	struct dm_dev *dev = dd->dev;

	*bdev = dev->bdev;

	/*
	 * Only pass ioctls through if the device sizes match exactly.
	 */
	if (dd->start ||
	    ti->len != i_size_read(dev->bdev->bd_inode) >> SECTOR_SHIFT)
		return 1;

	return 0;
}

static int dust_iterate_devices(struct dm_target *ti, iterate_devices_callout_fn fn,
				void *data)
{
	struct dust_device *dd = ti->private;

	return fn(ti, dd->dev, dd->start, ti->len, data);
}

static struct target_type dust_target = {
	.name = "dust",
	.version = {1, 0, 0},
	.module = THIS_MODULE,
	.ctr = dust_ctr,
	.dtr = dust_dtr,
	.iterate_devices = dust_iterate_devices,
	.map = dust_map,
	.message = dust_message,
	.status = dust_status,
	.prepare_ioctl = dust_prepare_ioctl,
};

static int __init dm_dust_init(void)
{
	int r = dm_register_target(&dust_target);

	if (r < 0)
		DMERR("dm_register_target failed %d", r);

	return r;
}

static void __exit dm_dust_exit(void)
{
	dm_unregister_target(&dust_target);
}

module_init(dm_dust_init);
module_exit(dm_dust_exit);

MODULE_DESCRIPTION(DM_NAME " dust test target");
MODULE_AUTHOR("Bryan Gurney <dm-devel@redhat.com>");
MODULE_LICENSE("GPL");
