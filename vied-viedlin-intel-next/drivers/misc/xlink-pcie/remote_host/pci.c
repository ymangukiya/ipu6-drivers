// SPDX-License-Identifier: GPL-2.0-only
/*****************************************************************************
 *
 * Intel Keem Bay XLink PCIe Driver
 *
 * Copyright (C) 2020 Intel Corporation
 *
 ****************************************************************************/

#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "pci.h"

#include "../common/core.h"
#include "../common/util.h"

extern int intel_xpcie_pci_setup_recovery_sysfs(struct xpcie_dev *xdev);
extern void intel_xpcie_pci_cleanup_recovery_sysfs(struct xpcie_dev *xdev);
enum xpcie_stage intel_xpcie_check_magic(struct xpcie_dev *xdev);

static int aspm_enable;
module_param(aspm_enable, int, 0664);
MODULE_PARM_DESC(aspm_enable, "enable ASPM");

static LIST_HEAD(dev_list);
static DEFINE_MUTEX(dev_list_mutex);

struct xpcie_dev *intel_xpcie_get_device_by_id(u32 id)
{
	struct xpcie_dev *xdev;

	mutex_lock(&dev_list_mutex);

	if (list_empty(&dev_list)) {
		mutex_unlock(&dev_list_mutex);
		return NULL;
	}

	list_for_each_entry(xdev, &dev_list, list) {
		if (xdev->devid == id) {
			mutex_unlock(&dev_list_mutex);
			return xdev;
		}
	}

	mutex_unlock(&dev_list_mutex);

	return NULL;
}

struct xpcie_dev *intel_xpcie_create_device(u32 sw_device_id,
					    struct pci_dev *pdev)
{
	struct xpcie_dev *xdev = kzalloc(sizeof(*xdev), GFP_KERNEL);

	if (!xdev)
		return NULL;

	xdev->devid = sw_device_id;
	snprintf(xdev->name, XPCIE_MAX_NAME_LEN, "%02x:%02x.%x",
		 pdev->bus->number,
		 PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));

	mutex_init(&xdev->lock);

	return xdev;
}

void intel_xpcie_remove_device(struct xpcie_dev *xdev)
{
	mutex_destroy(&xdev->lock);
	kfree(xdev);
}

void intel_xpcie_list_add_device(struct xpcie_dev *xdev)
{
	mutex_lock(&dev_list_mutex);

	list_add_tail(&xdev->list, &dev_list);

	mutex_unlock(&dev_list_mutex);
}

void intel_xpcie_list_del_device(struct xpcie_dev *xdev)
{
	mutex_lock(&dev_list_mutex);

	list_del(&xdev->list);

	mutex_unlock(&dev_list_mutex);
}

static void intel_xpcie_pci_set_aspm(struct xpcie_dev *xdev, int aspm)
{
	u16 link_control;
	u8 cap_exp;

	cap_exp = pci_find_capability(xdev->pci, PCI_CAP_ID_EXP);
	if (!cap_exp) {
		dev_err(&xdev->pci->dev, "failed to find pcie capability\n");
		return;
	}

	pci_read_config_word(xdev->pci, cap_exp + PCI_EXP_LNKCTL,
			     &link_control);
	link_control &= ~(PCI_EXP_LNKCTL_ASPMC);
	link_control |= (aspm & PCI_EXP_LNKCTL_ASPMC);
	pci_write_config_word(xdev->pci, cap_exp + PCI_EXP_LNKCTL,
			      link_control);
}

static void intel_xpcie_pci_unmap_bar(struct xpcie_dev *xdev)
{
	if (xdev->xpcie.bar0) {
		iounmap((void __iomem *)xdev->xpcie.bar0);
		xdev->xpcie.bar0 = NULL;
	}

	if (xdev->xpcie.io_comm) {
		iounmap(xdev->xpcie.io_comm);
		xdev->xpcie.io_comm = NULL;
	}

	if (xdev->xpcie.mmio) {
		xdev->xpcie.mmio = NULL;
	}

	if (xdev->xpcie.bar4) {
		iounmap((void __iomem *)xdev->xpcie.bar4);
		xdev->xpcie.bar4 = NULL;
	}
}

static int intel_xpcie_pci_map_bar(struct xpcie_dev *xdev)
{
	if (pci_resource_len(xdev->pci, 2) < XPCIE_IO_COMM_SIZE) {
		dev_err(&xdev->pci->dev, "device BAR region is too small\n");
		return -EIO;
	}

	xdev->xpcie.bar0 = (void __force *)pci_ioremap_bar(xdev->pci, 0);
	if (!xdev->xpcie.bar0) {
		dev_err(&xdev->pci->dev, "failed to ioremap BAR0\n");
		goto bar_error;
	}

	xdev->xpcie.io_comm = (void __force *)pci_ioremap_bar(xdev->pci, 2);
	if (!xdev->xpcie.io_comm) {
		dev_err(&xdev->pci->dev, "failed to ioremap BAR2\n");
		goto bar_error;
	}

	xdev->xpcie.mmio = (void __force *)
			   (xdev->xpcie.io_comm + XPCIE_MMIO_OFFSET);
	if (!xdev->xpcie.mmio) {
		dev_err(&xdev->pci->dev, "failed to ioremap BAR2\n");
		goto bar_error;
	}

	xdev->xpcie.bar4 = (void __force *)pci_ioremap_wc_bar(xdev->pci, 4);
	if (!xdev->xpcie.bar4) {
		dev_err(&xdev->pci->dev, "failed to ioremap BAR4\n");
		goto bar_error;
	}

	return 0;

bar_error:
	intel_xpcie_pci_unmap_bar(xdev);
	return -EIO;
}

static irqreturn_t intel_xpcie_core_interrupt(int irq, void *args)
{
	struct xpcie_dev *xdev = args;
	enum xpcie_stage stage;
	u8 event;

	stage = intel_xpcie_check_magic(xdev);
	if (stage == STAGE_ROM) {
		xdev->xpcie.status = XPCIE_STATUS_BOOT_FW;
		wake_up_interruptible(&xdev->waitqueue);
		return IRQ_HANDLED;
	} else if (stage == STAGE_UBOOT) {
		xdev->xpcie.status = XPCIE_STATUS_BOOT_OS;
		wake_up_interruptible(&xdev->waitqueue);
		return IRQ_HANDLED;
	} else if (stage == STAGE_RECOV) {
		xdev->xpcie.status = XPCIE_STATUS_RECOVERY;
		wake_up_interruptible(&xdev->waitqueue);
		return IRQ_HANDLED;
	} else if (stage == STAGE_OS) {
		if ((xdev->xpcie.status != XPCIE_STATUS_READY)
		     && (xdev->xpcie.status != XPCIE_STATUS_RUN)) {
			xdev->xpcie.status = XPCIE_STATUS_READY;
			intel_xpcie_set_host_status(&xdev->xpcie,
						    XPCIE_STATUS_READY);
			wake_up_interruptible(&xdev->waitqueue);
			return IRQ_HANDLED;
		}
	}

	event = intel_xpcie_get_doorbell(&xdev->xpcie, FROM_DEVICE, DEV_EVENT);
	if (event == DEV_SHUTDOWN || event == 0xFF) {
		schedule_delayed_work(&xdev->shutdown_event, 0);
		dev_warn(&xdev->pci->dev,
			 "Device shutting down, evnt rx'ed %x\n", event);
		return IRQ_HANDLED;
	}

	if (likely(xdev->core_irq_callback))
		return xdev->core_irq_callback(irq, args);

	return IRQ_HANDLED;
}

static void intel_xpcie_pci_irq_cleanup(struct xpcie_dev *xdev)
{
	int irq = pci_irq_vector(xdev->pci, 0);

	if (irq < 0)
		return;

	synchronize_irq(irq);
	free_irq(irq, xdev);
	pci_free_irq_vectors(xdev->pci);
}

static int intel_xpcie_pci_irq_init(struct xpcie_dev *xdev)
{
	int rc, irq;

	rc = pci_alloc_irq_vectors(xdev->pci, 1, 1, PCI_IRQ_MSI);
	if (rc < 0) {
		dev_err(&xdev->pci->dev,
			"failed to allocate %d MSI vectors\n", 1);
		return rc;
	}

	irq = pci_irq_vector(xdev->pci, 0);
	if (irq < 0) {
		dev_err(&xdev->pci->dev, "failed to get irq\n");
		rc = irq;
		goto error_irq;
	}
	rc = request_irq(irq, &intel_xpcie_core_interrupt, 0,
			 XPCIE_DRIVER_NAME, xdev);
	if (rc) {
		dev_err(&xdev->pci->dev, "failed to request irq\n");
		goto error_irq;
	}

	return 0;

error_irq:
	pci_free_irq_vectors(xdev->pci);
	return rc;
}

static void xpcie_device_poll(struct work_struct *work)
{
	struct xpcie_dev *xdev = container_of(work, struct xpcie_dev,
					      wait_event.work);

	if (!xdev->delay_wa_bar2_init) {
	    xdev->delay_wa_bar2_init = true;
	    schedule_delayed_work(&xdev->wait_event, msecs_to_jiffies(110));

	    return;
	}

	xdev->delay_wa_xlink_connect = true;

	return;
}

static int intel_xpcie_pci_prepare_dev_reset(struct xpcie_dev *xdev,
					     bool notify)
{
	if (mutex_lock_interruptible(&xdev->lock))
		return -EINTR;

	if (xdev->core_irq_callback) {
		xdev->core_irq_callback = NULL;
		intel_xpcie_core_cleanup(&xdev->xpcie);
	}
	xdev->xpcie.status = XPCIE_STATUS_OFF;
	if (notify)
		intel_xpcie_pci_raise_irq(xdev, DEV_EVENT, REQUEST_RESET);

	mutex_unlock(&xdev->lock);

	return 0;
}

static void xpcie_device_shutdown(struct work_struct *work)
{
	struct xpcie_dev *xdev = container_of(work, struct xpcie_dev,
					      shutdown_event.work);

	intel_xpcie_pci_prepare_dev_reset(xdev, false);
}

static int xpcie_device_init(struct xpcie_dev *xdev)
{
	int rc;

	INIT_DELAYED_WORK(&xdev->wait_event, xpcie_device_poll);
	INIT_DELAYED_WORK(&xdev->shutdown_event, xpcie_device_shutdown);

	xdev->delay_wa_bar2_init = false;
	xdev->delay_wa_xlink_connect = false;

	rc = intel_xpcie_pci_irq_init(xdev);
	if (rc)
		return rc;

	pci_set_master(xdev->pci);

	xdev->xpcie.status = XPCIE_STATUS_UNINIT;

	init_waitqueue_head(&xdev->waitqueue);
	schedule_delayed_work(&xdev->wait_event, 0);

	return 0;
}

int intel_xpcie_pci_init(struct xpcie_dev *xdev, struct pci_dev *pdev)
{
	int rc;

	if (mutex_lock_interruptible(&xdev->lock))
		return -EINTR;

	xdev->pci = pdev;
	pci_set_drvdata(pdev, xdev);

	rc = pci_enable_device_mem(xdev->pci);
	if (rc) {
		dev_err(&pdev->dev, "failed to enable pci device\n");
		goto error_exit;
	}

	rc = pci_request_regions(xdev->pci, XPCIE_DRIVER_NAME);
	if (rc) {
		dev_err(&pdev->dev, "failed to request mmio regions\n");
		goto error_req_mem;
	}

	rc = intel_xpcie_pci_map_bar(xdev);
	if (rc)
		goto error_map;

	rc = dma_set_mask_and_coherent(&xdev->pci->dev, DMA_BIT_MASK(64));
	if (rc) {
		dev_err(&pdev->dev, "failed to set dma mask\n");
		goto error_dma_mask;
	}

	intel_xpcie_pci_set_aspm(xdev, aspm_enable);

	rc = intel_xpcie_pci_setup_recovery_sysfs(xdev);
	if (rc) {
		dev_err(&pdev->dev,
			"failed to setup recovery sysfs facilities\n");
		goto error_dma_mask;
	}

	intel_xpcie_init_debug(&xdev->xpcie, &xdev->pci->dev);

	rc = xpcie_device_init(xdev);
	if (!rc)
		goto init_exit;

error_dma_mask:
	intel_xpcie_pci_unmap_bar(xdev);

error_map:
	pci_release_regions(xdev->pci);

error_req_mem:
	pci_disable_device(xdev->pci);

error_exit:
	xdev->xpcie.status = XPCIE_STATUS_ERROR;

init_exit:
	mutex_unlock(&xdev->lock);
	if (rc)
		mutex_destroy(&xdev->lock);
	return rc;
}

int intel_xpcie_pci_cleanup(struct xpcie_dev *xdev)
{
	if (mutex_lock_interruptible(&xdev->lock))
		return -EINTR;

	xdev->delay_wa_bar2_init = false;
	xdev->delay_wa_xlink_connect = false;

	intel_xpcie_pci_cleanup_recovery_sysfs(xdev);
	cancel_delayed_work(&xdev->wait_event);
	cancel_delayed_work(&xdev->shutdown_event);
	xdev->core_irq_callback = NULL;
	intel_xpcie_pci_irq_cleanup(xdev);

	intel_xpcie_core_cleanup(&xdev->xpcie);
	intel_xpcie_set_host_status(&xdev->xpcie, XPCIE_STATUS_OFF);
	intel_xpcie_pci_unmap_bar(xdev);
	pci_release_regions(xdev->pci);
	pci_disable_device(xdev->pci);
	pci_set_drvdata(xdev->pci, NULL);
	xdev->xpcie.status = XPCIE_STATUS_OFF;
	xdev->irq_enabled = false;

	mutex_unlock(&xdev->lock);

	return 0;
}

int intel_xpcie_pci_register_irq(struct xpcie_dev *xdev,
				 irq_handler_t irq_handler)
{
	if (xdev->xpcie.status != XPCIE_STATUS_READY) {
		return -EINVAL;
	}
	xdev->core_irq_callback = irq_handler;

	return 0;
}

int intel_xpcie_pci_raise_irq(struct xpcie_dev *xdev,
			      enum xpcie_doorbell_type type,
			      u8 value)
{
	u16 pci_status;

	intel_xpcie_set_doorbell(&xdev->xpcie, TO_DEVICE, type, value);
	pci_read_config_word(xdev->pci, PCI_STATUS, &pci_status);

	return 0;
}

u32 intel_xpcie_get_device_num(u32 *id_list)
{
	struct xpcie_dev *p;
	u32 num = 0;

	mutex_lock(&dev_list_mutex);

	if (list_empty(&dev_list)) {
		mutex_unlock(&dev_list_mutex);
		return 0;
	}

	list_for_each_entry(p, &dev_list, list) {
		*id_list++ = p->devid;
		num++;
	}
	mutex_unlock(&dev_list_mutex);

	return num;
}

int intel_xpcie_get_device_name_by_id(u32 id,
				      char *device_name, size_t name_size)
{
	struct xpcie_dev *xdev;
	size_t size;

	xdev = intel_xpcie_get_device_by_id(id);
	if (!xdev)
		return -ENODEV;

	mutex_lock(&xdev->lock);

	size = (name_size > XPCIE_MAX_NAME_LEN) ?
		XPCIE_MAX_NAME_LEN : name_size;
	memcpy(device_name, xdev->name, size);

	mutex_unlock(&xdev->lock);

	return 0;
}

int intel_xpcie_get_device_status_by_id(u32 id, u32 *status)
{
	struct xpcie_dev *xdev = intel_xpcie_get_device_by_id(id);

	if (!xdev)
		return -ENODEV;

	mutex_lock(&xdev->lock);
	*status = xdev->xpcie.status;
	mutex_unlock(&xdev->lock);

	return 0;
}

int intel_xpcie_pci_connect_device(u32 id)
{
	struct xpcie_dev *xdev;
	int rc = 0;

	xdev = intel_xpcie_get_device_by_id(id);
	if (!xdev)
		return -ENODEV;

	if (!xdev->delay_wa_xlink_connect)
		return -ENODEV;

	if (mutex_lock_interruptible(&xdev->lock))
		return -EINTR;

	if (xdev->xpcie.status == XPCIE_STATUS_RUN)
		goto connect_cleanup;

	if (xdev->xpcie.status == XPCIE_STATUS_OFF) {
		rc = -ENODEV;
		goto connect_cleanup;
	}

	if (xdev->xpcie.status != XPCIE_STATUS_READY) {
		rc = -EBUSY;
		goto connect_cleanup;
	}

	rc = intel_xpcie_core_init(&xdev->xpcie);
	if (rc < 0) {
		dev_err(&xdev->pci->dev, "failed to sync with device\n");
		goto connect_cleanup;
	}

connect_cleanup:
	mutex_unlock(&xdev->lock);
	return rc;
}

int intel_xpcie_pci_read(u32 id, void *data, size_t *size, u32 timeout)
{
	struct xpcie_dev *xdev = intel_xpcie_get_device_by_id(id);

	if (!xdev)
		return -ENODEV;

	return intel_xpcie_core_read(&xdev->xpcie, data, size, timeout);
}

int intel_xpcie_pci_write(u32 id, void *data, size_t *size, u32 timeout)
{
	struct xpcie_dev *xdev = intel_xpcie_get_device_by_id(id);

	if (!xdev)
		return -ENODEV;

	return intel_xpcie_core_write(&xdev->xpcie, data, size, timeout);
}

int intel_xpcie_pci_reset_device(u32 id)
{
	struct xpcie_dev *xdev = intel_xpcie_get_device_by_id(id);

	if (!xdev)
		return -ENOMEM;

	return intel_xpcie_pci_prepare_dev_reset(xdev, true);
}

int intel_xpcie_pci_register_device_event(u32 sw_device_id,
					  xlink_device_event event_notif_fn)
{
	struct xpcie_dev *xdev = intel_xpcie_get_device_by_id(sw_device_id);

	if (!xdev)
		return -ENOMEM;

	xdev->event_fn = event_notif_fn;

	return 0;
}

int intel_xpcie_pci_unregister_device_event(u32 sw_device_id)
{
	struct xpcie_dev *xdev = intel_xpcie_get_device_by_id(sw_device_id);

	if (!xdev)
		return -ENOMEM;

	xdev->event_fn = NULL;

	return 0;
}

void intel_xpcie_pci_notify_event(struct xpcie_dev *xdev,
				  enum xlink_device_event_type event_type)
{
	if (event_type >= NUM_EVENT_TYPE)
		return;

	if (xdev->event_fn)
		xdev->event_fn(xdev->devid, event_type);
}