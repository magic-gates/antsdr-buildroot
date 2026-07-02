// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * High-Performance Zero-Copy AXI DMA Engine Driver
 * Compatible with Xilinx Zynq-7000 AXI DMA (Scatter-Gather Mode Enabled)
 */

#include "linux/export.h"
#include "linux/mod_devicetable.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/completion.h>
#include <linux/mutex.h>

#define DRIVER_NAME           "magic_dma"
#define FIXED_TRANSFER_SIZE   65536  /* Fixed transfer size requirement */

/* Device Private Structure */
struct magic_dma_dev {
	struct device *dev;
	struct dma_chan *tx_chan;
	struct dma_chan *rx_chan;
	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct mutex tx_lock; /* Protects simultaneous TX operations */
	struct mutex rx_lock; /* Protects simultaneous RX operations */
};

/* DMA Complete Signal Callback */
static void magic_dma_callback(void *param)
{
	struct completion *cmp = param;
	complete(cmp);
}

/**
 * magic_dma_prepare_sg - Pins userspace memory and creates a scatterlist
 */
static int magic_dma_prepare_sg(struct dma_chan *chan, unsigned long ubuf, size_t len,
				enum dma_data_direction dir, struct page ***out_pages,
				struct scatterlist **out_sg, int *out_nr_pages)
{
	struct page **pages;
	struct scatterlist *sg;
	unsigned long first, last;
	int nr_pages, pinned, i;
	size_t total_mapped = 0;
	unsigned int gup_flags = (dir == DMA_FROM_DEVICE) ? FOLL_WRITE : 0;

	/* Calculate the number of memory pages spanned by user buffer */
	first = ubuf & PAGE_MASK;
	last = (ubuf + len - 1) & PAGE_MASK;
	nr_pages = ((last - first) >> PAGE_SHIFT) + 1;

	pages = kvmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	/* Pin userspace pages into RAM for modern long-term DMA safety */
	pinned = pin_user_pages_fast(ubuf, nr_pages, gup_flags, pages);
	if (pinned < nr_pages) {
		if (pinned > 0)
			unpin_user_pages(pages, pinned);
		kvfree(pages);
		return pinned < 0 ? pinned : -EFAULT;
	}

	/* Allocate and populate the standard scatterlist table */
	sg = kmalloc_array(nr_pages, sizeof(struct scatterlist), GFP_KERNEL);
	if (!sg) {
		unpin_user_pages(pages, nr_pages);
		kvfree(pages);
		return -ENOMEM;
	}

	sg_init_table(sg, nr_pages);
	for (i = 0; i < nr_pages; i++) {
		unsigned int offset = (i == 0) ? (ubuf & ~PAGE_MASK) : 0;
		unsigned int length = min_t(unsigned int, len - total_mapped, PAGE_SIZE - offset);

		sg_set_page(&sg[i], pages[i], length, offset);
		total_mapped += length;
	}

	*out_pages = pages;
	*out_sg = sg;
	*out_nr_pages = nr_pages;
	return 0;
}

/**
 * magic_dma_execute - Configures, maps, and fires the actual Scatter-Gather DMA engine
 */
static int magic_dma_execute(struct dma_chan *chan, struct scatterlist *sg, int nr_pages,
			     enum dma_data_direction dir)
{
	struct dma_async_tx_descriptor *txd;
	struct completion cmp;
	dma_cookie_t cookie;
	enum dma_transfer_direction trans_dir;
	enum dma_status status;
	int mapped_sg_len, ret = 0;
	unsigned long timeout;

	trans_dir = (dir == DMA_TO_DEVICE) ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM;

	/* Stream map the scatterlist (Zero-Copy Architecture Setup) */
	mapped_sg_len = dma_map_sg(chan->device->dev, sg, nr_pages, dir);
	if (mapped_sg_len <= 0) {
		dev_err(chan->device->dev, "Failed to dma_map_sg\n");
		return -EIO;
	}

	/* Request engine descriptor using the official dmaengine framework pattern */
	txd = chan->device->device_prep_slave_sg(chan, sg, mapped_sg_len, trans_dir,
						 DMA_CTRL_ACK | DMA_PREP_INTERRUPT, NULL);
	if (!txd) {
		dev_err(chan->device->dev, "Failed to prepare slave SG descriptor\n");
		ret = -EIO;
		goto err_unmap;
	}

	init_completion(&cmp);
	txd->callback = magic_dma_callback;
	txd->callback_param = &cmp;

	cookie = txd->tx_submit(txd);
	if (dma_submit_error(cookie)) {
		dev_err(chan->device->dev, "DMA descriptor submit failed\n");
		ret = -EIO;
		goto err_unmap;
	}

	/* Trigger hardware start execution */
	dma_async_issue_pending(chan);

	/* Block waiting for hardware interrupt to trigger completion */
	timeout = wait_for_completion_interruptible_timeout(&cmp, msecs_to_jiffies(5000));
	if (timeout == 0) {
		dev_err(chan->device->dev, "DMA operation timed out\n");
		dmaengine_terminate_all(chan);
		ret = -ETIMEDOUT;
	} else if (timeout == -ERESTARTSYS) {
		dev_err(chan->device->dev, "DMA interrupted by system signal\n");
		dmaengine_terminate_all(chan);
		ret = -ERESTARTSYS;
	} else {
		status = dma_async_is_tx_complete(chan, cookie, NULL, NULL);
		if (status != DMA_COMPLETE) {
			dev_err(chan->device->dev, "DMA engine reports an error state\n");
			ret = -EIO;
		}
	}

err_unmap:
	dma_unmap_sg(chan->device->dev, sg, nr_pages, dir);
	return ret;
}

/**
 * magic_dma_do_transfer - Core orchestration wrapper logic
 */
static ssize_t magic_dma_do_transfer(struct magic_dma_dev *priv, struct dma_chan *chan,
				     unsigned long ubuf, size_t len, enum dma_data_direction dir)
{
	struct page **pages = NULL;
	struct scatterlist *sg = NULL;
	int nr_pages = 0;
	int ret;

	if (len != FIXED_TRANSFER_SIZE) {
		dev_warn(priv->dev, "Invalid size! Only %d bytes are allowed\n", FIXED_TRANSFER_SIZE);
		return -EINVAL;
	}

	/* Verify DMA IP-specific byte configuration alignment alignments */
	if (ubuf & ((1 << chan->device->copy_align) - 1)) {
		dev_err(priv->dev, "Buffer address is misaligned for this DMA engine instance\n");
		return -EINVAL;
	}

	/* Pin userspace pages & construct scatterlist */
	ret = magic_dma_prepare_sg(chan, ubuf, len, dir, &pages, &sg, &nr_pages);
	if (ret < 0)
		return ret;

	/* Run hardware operations */
	ret = magic_dma_execute(chan, sg, nr_pages, dir);

	/* Clean up pinned user references and allocations */
	unpin_user_pages(pages, nr_pages);
	kvfree(pages);
	kfree(sg);

	return (ret == 0) ? len : ret;
}

/* File Operations Implementations */
static int magic_dma_open(struct inode *inode, struct file *filp)
{
	struct magic_dma_dev *priv = container_of(inode->i_cdev, struct magic_dma_dev, cdev);
	filp->private_data = priv;
	return 0;
}

static int magic_dma_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/* Device to Userspace (RX / S2MM) Data Delivery Execution */
static ssize_t magic_dma_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	struct magic_dma_dev *priv = filp->private_data;
	ssize_t ret;

	if (mutex_lock_interruptible(&priv->rx_lock))
		return -ERESTARTSYS;

	ret = magic_dma_do_transfer(priv, priv->rx_chan, (unsigned long)buf, count, DMA_FROM_DEVICE);

	mutex_unlock(&priv->rx_lock);
	return ret;
}

/* Userspace to Device (TX / MM2S) Data Delivery Execution */
static ssize_t magic_dma_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct magic_dma_dev *priv = filp->private_data;
	ssize_t ret;

	if (mutex_lock_interruptible(&priv->tx_lock))
		return -ERESTARTSYS;

	ret = magic_dma_do_transfer(priv, priv->tx_chan, (unsigned long)buf, count, DMA_TO_DEVICE);

	mutex_unlock(&priv->tx_lock);
	return ret;
}

static const struct file_operations magic_dma_fops = {
	.owner   = THIS_MODULE,
	.open    = magic_dma_open,
	.release = magic_dma_release,
	.read    = magic_dma_read,
	.write   = magic_dma_write,
};

/* Platform Subsystem Functions */
static int magic_dma_probe(struct platform_device *pdev)
{
	struct magic_dma_dev *priv;
	struct device *dev_device;
	int err;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	platform_set_drvdata(pdev, priv);

	mutex_init(&priv->tx_lock);
	mutex_init(&priv->rx_lock);

	/* Acquire standard channel mappings indexed directly out of DTS node definition definitions */
	priv->tx_chan = dma_request_chan(&pdev->dev, "tx");
	if (IS_ERR(priv->tx_chan)) {
		err = PTR_ERR(priv->tx_chan);
		if (err != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Error securing TX channel infrastructure context\n");
		return err;
	}

	priv->rx_chan = dma_request_chan(&pdev->dev, "rx");
	if (IS_ERR(priv->rx_chan)) {
		err = PTR_ERR(priv->rx_chan);
		if (err != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Error securing RX channel infrastructure context\n");
		goto err_free_tx;
	}

	/* Register Character Interface mappings for userspace application context connectivity */
	err = alloc_chrdev_region(&priv->devt, 0, 1, DRIVER_NAME);
	if (err < 0) {
		dev_err(&pdev->dev, "Failed allocating system chrdev nodes\n");
		goto err_free_rx;
	}

	cdev_init(&priv->cdev, &magic_dma_fops);
	priv->cdev.owner = THIS_MODULE;
	err = cdev_add(&priv->cdev, priv->devt, 1);
	if (err < 0) {
		dev_err(&pdev->dev, "Failed linking active operational configuration to node\n");
		goto err_unregister_region;
	}

	/* Populates udev runtime class system details nodes automatically inside /dev/magic_dma */
	priv->class = class_create(THIS_MODULE, "magic_dma_class");
	if (IS_ERR(priv->class)) {
		err = PTR_ERR(priv->class);
		goto err_del_cdev;
	}

	dev_device = device_create(priv->class, &pdev->dev, priv->devt, NULL, "magic_dma");
	if (IS_ERR(dev_device)) {
		err = PTR_ERR(dev_device);
		goto err_destroy_class;
	}

	dev_info(&pdev->dev, "Zero-Copy AXI Scatter-Gather Master Driver Initialized Successfully\n");
	return 0;

err_destroy_class:
	class_destroy(priv->class);
err_del_cdev:
	cdev_del(&priv->cdev);
err_unregister_region:
	unregister_chrdev_region(priv->devt, 1);
err_free_rx:
	dma_release_channel(priv->rx_chan);
err_free_tx:
	dma_release_channel(priv->tx_chan);
	return err;
}

static int magic_dma_remove(struct platform_device *pdev)
{
	struct magic_dma_dev *priv = platform_get_drvdata(pdev);

	device_destroy(priv->class, priv->devt);
	class_destroy(priv->class);
	cdev_del(&priv->cdev);
	unregister_chrdev_region(priv->devt, 1);

	dmaengine_terminate_all(priv->rx_chan);
	dma_release_channel(priv->rx_chan);

	dmaengine_terminate_all(priv->tx_chan);
	dma_release_channel(priv->tx_chan);

	dev_info(&pdev->dev, "Driver Unloaded and Cleaned Up\n");

	return 0;
}

static const struct of_device_id magic_dma_of_ids[] = {
	{ .compatible = "magic,dma", },
	{ }
};
MODULE_DEVICE_TABLE(of, magic_dma_of_ids);

static struct platform_driver magic_dma_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = magic_dma_of_ids,
	},
	.probe  = magic_dma_probe,
	.remove = magic_dma_remove,
};

module_platform_driver(magic_dma_driver);

MODULE_AUTHOR("Danil Karpenko");
MODULE_DESCRIPTION("Magic tricks");
MODULE_LICENSE("GPL v2");
