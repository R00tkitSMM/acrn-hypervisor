/*
 * Copyright (C) 2018 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */


#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "usb.h"
#include "usbdi.h"
#include "usb_pmapper.h"

#undef LOG_TAG
#define LOG_TAG "USBPM: "

static struct usb_dev_sys_ctx_info g_ctx;
static uint16_t usb_dev_get_ep_maxp(struct usb_dev *udev, int pid, int epnum);

static bool
usb_get_native_devinfo(struct libusb_device *ldev,
		struct usb_native_devinfo *info,
		struct libusb_device_descriptor *desc)
{
	struct libusb_device_descriptor d;
	int rc;

	if (!ldev || !info)
		return false;

	memset(info, 0, sizeof(*info));
	info->speed = libusb_get_device_speed(ldev);
	info->priv_data = ldev;
	info->path.bus = libusb_get_bus_number(ldev);
	info->path.depth = libusb_get_port_numbers(ldev, info->path.path,
			USB_MAX_TIERS);

	rc = libusb_get_device_descriptor(ldev, &d);
	if (rc) {
		UPRINTF(LWRN, "fail to get descriptor for %d-%s\r\n",
				info->path.bus, usb_dev_path(&info->path));
		return false;
	}

	/* set device type */
	if (ROOTHUB_PORT(info->path) == 0)
		info->type = USB_TYPE_ROOTHUB;
	else if (d.bDeviceClass == LIBUSB_CLASS_HUB)
		info->type = USB_TYPE_EXTHUB;
	else if (info->path.path[1] == 0)
		info->type = USB_TYPE_ROOTHUB_SUBDEV;
	else
		info->type = USB_TYPE_EXTHUB_SUBDEV;

	if (info->type == USB_TYPE_EXTHUB) {
		info->maxchild = usb_get_hub_port_num(&info->path);
		if (info->maxchild < 0)
			UPRINTF(LDBG, "fail to get count of numbers of hub"
					" %d-%s\r\n", info->path.bus,
					usb_dev_path(&info->path));
	}

	info->pid = d.idProduct;
	info->vid = d.idVendor;
	info->bcd = d.bcdUSB;

	if (desc != NULL)
		*desc = d;

	return true;
}

static void
internal_scan(struct libusb_device ***list, int list_sz, int depth,
		int8_t *visit, int visit_sz)
{
	int i;
	struct libusb_device **devlist;
	struct usb_native_devinfo di;

	devlist = *list;
	if (depth >= USB_MAX_TIERS) {
		UPRINTF(LFTL, "max hub layers(7) reached, stop scan\r\n");
		return;
	}

	/* The scanning must be done according to the order from depth 1 to
	 * USB_MAX_TIERS. The reason is if hub exist in the USB device tree,
	 * the ports of hub should be assigned first, and then its child
	 * is scanned. The reason is external hub ports are dyanmically
	 * assigned.
	 */

	/* scan devices and assign ports if hub is found */
	for (i = 0; i < list_sz; i++) {
		if (usb_get_native_devinfo(devlist[i], &di, NULL) == false)
			continue;

		if (!visit[i] && di.path.depth == depth &&
				ROOTHUB_PORT(di.path)) {
			visit[i] = 1;
			if (g_ctx.conn_cb)
				g_ctx.conn_cb(g_ctx.hci_data, &di);
		}
	}

	/* do the scanning in deeper depth */
	for (i = 0; i < list_sz; i++) {
		if (usb_get_native_devinfo(devlist[i], &di, NULL) == false)
			continue;

		if (!visit[i] && di.path.depth > depth && ROOTHUB_PORT(di.path))
			internal_scan(list, list_sz, depth + 1, visit,
					visit_sz);
	}
}

static int
usb_dev_scan_dev(struct libusb_device ***devlist)
{
	int num_devs;
	int8_t visit[USB_MAX_DEVICES];

	if (!g_ctx.libusb_ctx)
		return -1;

	num_devs = libusb_get_device_list(g_ctx.libusb_ctx, devlist);
	if (num_devs < 0) {
		*devlist = NULL;
		return -1;
	}

	memset(visit, 0, sizeof(visit));
	internal_scan(devlist, num_devs, 1, visit, USB_MAX_DEVICES);
	return num_devs;
}

static int
libusb_speed_to_usb_speed(int libusb_speed)
{
	int speed = LIBUSB_SPEED_UNKNOWN;

	switch (libusb_speed) {
	case LIBUSB_SPEED_LOW:
		speed = USB_SPEED_LOW;
		break;
	case LIBUSB_SPEED_FULL:
		speed = USB_SPEED_FULL;
		break;
	case LIBUSB_SPEED_HIGH:
		speed = USB_SPEED_HIGH;
		break;
	case LIBUSB_SPEED_SUPER:
		speed = USB_SPEED_SUPER;
		break;
	default:
		UPRINTF(LWRN, "%s unexpect speed %d\r\n", __func__,
				libusb_speed);
	}
	return speed;
}

static void
usb_dev_comp_cb(struct libusb_transfer *trn)
{
	struct usb_dev_req *r;
	struct usb_xfer *xfer;
	struct usb_block *block;
	struct usb_native_devinfo *info;
	int do_intr = 0;
	int i, idx, buf_idx, done;
	int is_stalled = 0;
	int framelen = 0;
	uint16_t maxp;
	uint8_t *buf;

	/* async request */
	r = trn->user_data;
	if (!r) {
		UPRINTF(LFTL, "error: user context data not found on USB transfer\r\n");
		goto free_transfer;
	}
	info = &r->udev->info;

	/* async transfer */
	xfer = r->xfer;

	maxp = usb_dev_get_ep_maxp(r->udev, r->in, xfer->epid / 2);
	if (trn->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
		/* got the isoc frame length */
		framelen = USB_EP_MAXP_SZ(maxp) * (1 + USB_EP_MAXP_MT(maxp));
		UPRINTF(LDBG, "iso maxp %u framelen %d\r\n", maxp, framelen);
	}
	UPRINTF(LDBG, "%s: %d-%s: actlen %d ep%d-xfr [%d-%d %d] rq-%d "
			"[%d-%d %d] st %d\r\n", __func__, info->path.bus,
			usb_dev_path(&info->path), trn->actual_length,
			xfer->epid, xfer->head, xfer->tail, xfer->ndata,
			r->seq, r->blk_head, r->blk_tail, r->buf_size,
			trn->status);

	/* lock for protecting the transfer */
	xfer->status = USB_ERR_NORMAL_COMPLETION;

	switch (trn->status) {
	case LIBUSB_TRANSFER_STALL:
		xfer->status = USB_ERR_STALLED;
		is_stalled = 1;
		goto stall_out;
	case LIBUSB_TRANSFER_NO_DEVICE:
		/* avoid short packet warnings when devices are plugged out. */
		xfer->status = USB_ERR_SHORT_XFER;
		goto out;
	case LIBUSB_TRANSFER_ERROR:
		/*
		 * If this error happened due to device disconnecting, there is
		 * nothing should do and just wait usb_dev_native_sys_disconn_cb
		 * to do the 'unplugging process'.
		 */
		if (usb_native_is_device_existed(&info->path) == 0)
			goto cancel_out;

		is_stalled = 1;
		xfer->status = USB_ERR_STALLED;
		goto stall_out;
	case LIBUSB_TRANSFER_CANCELLED:
		xfer->status = USB_ERR_IOERROR;
		goto cancel_out;
	case LIBUSB_TRANSFER_TIMED_OUT:
		xfer->status = USB_ERR_TIMEOUT;
		goto out;
	case LIBUSB_TRANSFER_OVERFLOW:
		xfer->status = USB_ERR_BAD_BUFSIZE;
		goto out;
	case LIBUSB_TRANSFER_COMPLETED:
		break;
	default:
		UPRINTF(LWRN, "unknown failure: %x\r\n", trn->status);
		break;
	}

	g_ctx.lock_ep_cb(xfer->dev, &xfer->epid);
	for (i = 0; i < trn->num_iso_packets; i++)
		UPRINTF(LDBG, "iso_frame %d len %u act_len %u\n", i,
				trn->iso_packet_desc[i].length,
				trn->iso_packet_desc[i].actual_length);

	/* handle the blocks belong to this request */
	i = 0;
	buf = r->buffer;
	idx = r->blk_head;
	buf_idx = 0;
	done = trn->actual_length;

	while (index_valid(r->blk_head, r->blk_tail, xfer->max_blk_cnt, idx)) {
		if (trn->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
			buf_idx = 0;
			buf = libusb_get_iso_packet_buffer_simple(trn, i);
			done = trn->iso_packet_desc[i].actual_length;
			i++;
		}
		do {
			int d;

			block = &xfer->data[idx];
			if (block->stat == USB_BLOCK_FREE &&
					block->type != USB_DATA_NONE)
				UPRINTF(LFTL, "error: found free block\r\n");

			d = done;
			if (d > block->blen)
				d = block->blen;

			if (block->type == USB_DATA_PART ||
					block->type == USB_DATA_FULL) {
				if (r->in == TOKEN_IN) {
					memcpy(block->buf, buf + buf_idx, d);
					buf_idx += d;
				}
			} else {
				/* Link TRB */
				i--;
			}

			done -= d;
			block->blen -= d;
			block->bdone = d;
			block->stat = USB_BLOCK_HANDLED;
			idx = index_inc(idx, xfer->max_blk_cnt);

		} while (block->type == USB_DATA_PART);
	}

stall_out:
	if (is_stalled) {
		idx = r->blk_head;
		while (index_valid(r->blk_head, r->blk_tail,
					xfer->max_blk_cnt, idx)) {
			block = &xfer->data[idx];
			block->stat = USB_BLOCK_HANDLED;
			idx = index_inc(idx, xfer->max_blk_cnt);
		}
	}

out:
	/* notify the USB core this transfer is over */
	if (g_ctx.notify_cb)
		do_intr = g_ctx.notify_cb(xfer->dev, xfer);

	/* if a interrupt is needed, send it to guest */
	if (do_intr && g_ctx.intr_cb)
		g_ctx.intr_cb(xfer->dev, NULL);

cancel_out:
	/* unlock and release memory */
	g_ctx.unlock_ep_cb(xfer->dev, &xfer->epid);

	if (r && r->buffer)
		free(r->buffer);

	xfer->reqs[r->blk_head] = NULL;
	free(r);
free_transfer:
	libusb_free_transfer(trn);
}

static struct usb_dev_req *
usb_dev_alloc_req(struct usb_dev *udev, struct usb_xfer *xfer, int in,
		size_t size, size_t count)
{
	struct usb_dev_req *req;
	static int seq = 1;

	if (!udev || !xfer || count < 0)
		return NULL;

	req = calloc(1, sizeof(*req));
	if (!req)
		return NULL;

	req->udev = udev;
	req->in = in;
	req->xfer = xfer;
	req->seq = seq++;
	req->trn = libusb_alloc_transfer(count);
	if (!req->trn)
		goto errout;

	if (size)
		req->buffer = malloc(size);

	if (!req->buffer)
		goto errout;

	return req;

errout:
	if (req && req->buffer)
		free(req->buffer);
	if (req && req->trn)
		libusb_free_transfer(req->trn);
	if (req)
		free(req);
	return NULL;
}

static int
usb_dev_prepare_xfer(struct usb_xfer *xfer, int *head, int *tail)
{
	int i, idx, size, first;
	struct usb_block *block = NULL;

	idx = xfer->head;
	first = -1;
	size = 0;
	if (idx < 0 || idx >= xfer->max_blk_cnt)
		return -1;

	for (i = 0; i < xfer->ndata;
		i++, idx = index_inc(idx, xfer->max_blk_cnt)) {
		block = &xfer->data[idx];
		if (block->stat == USB_BLOCK_HANDLED ||
				block->stat == USB_BLOCK_HANDLING)
			continue;

		first = (first < 0) ? idx : first;

		switch (block->type) {
		case USB_DATA_PART:
		case USB_DATA_FULL:
			size += block->blen;
			block->stat = USB_BLOCK_HANDLING;
			break;
		case USB_DATA_NONE:
			block->stat = USB_BLOCK_HANDLED;
			continue;
		default:
			UPRINTF(LFTL, "%s error stat %d\r\n",
					__func__, block->type);
		}
	}

	*head = first;
	*tail = xfer->tail;
	return size;
}

static inline int
usb_dev_err_convert(int err)
{
	switch (err) {
	case LIBUSB_ERROR_TIMEOUT: return USB_ERR_TIMEOUT;
	case LIBUSB_ERROR_PIPE: return USB_ERR_STALLED;
	case LIBUSB_ERROR_NO_DEVICE: return USB_ERR_IOERROR;
	case LIBUSB_ERROR_BUSY: return USB_ERR_IN_USE;
	case LIBUSB_ERROR_OVERFLOW: return USB_ERR_BAD_BUFSIZE;
	case LIBUSB_ERROR_IO: return USB_ERR_IOERROR;
	default:
		break; /* add more when required */
	}
	return USB_ERR_IOERROR;
}

static inline struct usb_dev_ep *
usb_dev_get_ep(struct usb_dev *udev, int pid, int ep)
{
	if (ep < 0 || ep >= USB_NUM_ENDPOINT) {
		UPRINTF(LWRN, "invalid ep %d\r\n", ep);
		return NULL;
	}

	if (ep == 0)
		return &udev->epc;

	if (pid == TOKEN_IN)
		return udev->epi + ep - 1;
	else
		return udev->epo + ep - 1;
}

static inline void
usb_dev_set_ep_type(struct usb_dev *udev, int pid, int epnum,
		uint8_t type)
{
	struct usb_dev_ep *ep;

	ep = usb_dev_get_ep(udev, pid, epnum);
	if (ep)
		ep->type = type;
}

static inline uint8_t
usb_dev_get_ep_type(struct usb_dev *udev, int pid, int epnum)
{
	struct usb_dev_ep *ep;

	ep = usb_dev_get_ep(udev, pid, epnum);
	if (!ep)
		return USB_EP_ERR_TYPE;
	else
		return ep->type;
}

static inline void
usb_dev_set_ep_maxp(struct usb_dev *udev, int pid, int epnum, uint16_t maxp)
{
	struct usb_dev_ep *ep;

	ep = usb_dev_get_ep(udev, pid, epnum);
	if (ep)
		ep->maxp = maxp;
}

static inline uint16_t
usb_dev_get_ep_maxp(struct usb_dev *udev, int pid, int epnum)
{
	struct usb_dev_ep *ep;

	ep = usb_dev_get_ep(udev, pid, epnum);
	if (!ep)
		return 0;
	else
		return ep->maxp;
}

static void
usb_dev_reset_ep(struct usb_dev *udev)
{
	int ep;

	udev->epc.type = USB_ENDPOINT_CONTROL;
	for (ep = 0; ep < USB_NUM_ENDPOINT; ep++) {
		udev->epi[ep].pid = TOKEN_IN;
		udev->epo[ep].pid = TOKEN_OUT;
		udev->epi[ep].type = USB_ENDPOINT_INVALID;
		udev->epo[ep].type = USB_ENDPOINT_INVALID;
	}
}

static void
usb_dev_update_ep(struct usb_dev *udev)
{
	struct libusb_config_descriptor *cfg;
	const struct libusb_interface_descriptor *_if;
	const struct libusb_endpoint_descriptor *desc;
	int i, j;

	if (libusb_get_active_config_descriptor(udev->info.priv_data, &cfg))
		return;

	for (i = 0; i < cfg->bNumInterfaces; i++) {
		_if = &cfg->interface[i].altsetting[udev->alts[i]];

		for (j = 0; j < _if->bNumEndpoints; j++) {
			desc = &_if->endpoint[j];
			usb_dev_set_ep_type(udev,
					USB_EP_PID(desc),
					USB_EP_NR(desc),
					USB_EP_TYPE(desc));
			usb_dev_set_ep_maxp(udev,
					USB_EP_PID(desc),
					USB_EP_NR(desc),
					USB_EP_MAXP(desc));
		}
	}
	libusb_free_config_descriptor(cfg);
}

static int
usb_dev_native_toggle_if(struct usb_dev *udev, int claim)
{
	struct libusb_config_descriptor *config;
	struct usb_devpath *path;
	uint8_t c, i;
	int rc = 0, r;

	path = &udev->info.path;
	r = libusb_get_active_config_descriptor(udev->info.priv_data, &config);
	if (r) {
		UPRINTF(LWRN, "%d-%s: can't get config\r\n", path->bus,
				usb_dev_path(path));
		return -1;
	}

	c = config->bConfigurationValue;
	for (i = 0; i < config->bNumInterfaces; i++) {
		if (claim == 1)
			r = libusb_claim_interface(udev->handle, i);
		else {
			r = libusb_release_interface(udev->handle, i);
			/* according to libusb, if libusb_release_interface
			 * return LIBUSB_ERROR_NOT_FOUND, it means that this
			 * interface is not claimed before. This case should
			 * not be considered as an error here.
			 */
			if (r == LIBUSB_ERROR_NOT_FOUND)
				r = 0;
		}
		if (r) {
			rc = -1;
			UPRINTF(LWRN, "%d-%s:%d.%d can't %s if, r %d\r\n",
					path->bus, usb_dev_path(path), c, i,
					claim == 1 ? "claim" : "release", r);
		}
	}
	if (rc)
		UPRINTF(LWRN, "%d-%s fail to %s rc %d\r\n", path->bus,
				usb_dev_path(path), claim == 1 ? "claim" :
				"release", rc);

	libusb_free_config_descriptor(config);
	return rc;
}

static int
usb_dev_native_toggle_if_drivers(struct usb_dev *udev, int attach)
{
	struct libusb_config_descriptor *config;
	struct usb_devpath *path;
	uint8_t c, i;
	int rc = 0, r;

	path = &udev->info.path;
	r = libusb_get_active_config_descriptor(udev->info.priv_data, &config);
	if (r) {
		UPRINTF(LWRN, "%d-%s: can't get config\r\n", path->bus,
				usb_dev_path(path));
		return -1;
	}

	UPRINTF(LDBG, "%s driver\r\n", attach == 1 ?  "attach" : "detach");

	c = config->bConfigurationValue;
	for (i = 0; i < config->bNumInterfaces; i++) {
		if (attach == 1)
			r = libusb_attach_kernel_driver(udev->handle, i);
		else {
			if (libusb_kernel_driver_active(udev->handle, i) == 1)
				r = libusb_detach_kernel_driver(udev->handle,
						i);
		}

		if (r) {
			rc = -1;
			UPRINTF(LWRN, "%d-%s:%d.%d can't %stach if driver, r %d"
					"\r\n", path->bus, usb_dev_path(path),
					c, i, attach == 1 ? "at" : "de", r);
		}
	}
	if (rc)
		UPRINTF(LWRN, "%d-%s fail to %s rc %d\r\n", path->bus,
				usb_dev_path(path), attach == 1 ? "attach" :
				"detach", rc);

	libusb_free_config_descriptor(config);
	return rc;
}

static void
usb_dev_set_config(struct usb_dev *udev, struct usb_xfer *xfer, int config)
{
	int rc = 0;
	struct libusb_config_descriptor *cfg;

	/*
	 * set configuration
	 * according to the libusb doc, the detach and release work
	 * should be done before set configuration.
	 */
	usb_dev_native_toggle_if_drivers(udev, 0);
	usb_dev_native_toggle_if(udev, 0);

	rc = libusb_set_configuration(udev->handle, config);
	if (rc) {
		UPRINTF(LWRN, "fail to set config rc %d\r\n", rc);
		goto err2;
	}

	/* claim all the interfaces of this configuration */
	rc = libusb_get_active_config_descriptor(udev->info.priv_data, &cfg);
	if (rc) {
		UPRINTF(LWRN, "fail to get config rc %d\r\n", rc);
		goto err2;
	}

	rc = usb_dev_native_toggle_if(udev, 1);
	if (rc) {
		UPRINTF(LWRN, "fail to claim if, rc %d\r\n", rc);
		goto err1;
	}

	udev->if_num = cfg->bNumInterfaces;
	udev->configuration = config;

	usb_dev_reset_ep(udev);
	usb_dev_update_ep(udev);
	libusb_free_config_descriptor(cfg);
	return;

err1:
	usb_dev_native_toggle_if(udev, 0);
	libusb_free_config_descriptor(cfg);
err2:
	UPRINTF(LWRN, "%d-%s: fail to set config\r\n", udev->info.path.bus,
			usb_dev_path(&udev->info.path));
	xfer->status = USB_ERR_STALLED;
}

static void
usb_dev_set_if(struct usb_dev *udev, int iface, int alt, struct usb_xfer
		*xfer)
{
	if (iface >= USB_NUM_INTERFACE)
		goto errout;

	UPRINTF(LDBG, "%d-%s set if, iface %d alt %d\r\n", udev->info.path.bus,
			usb_dev_path(&udev->info.path), iface, alt);

	if (libusb_set_interface_alt_setting(udev->handle, iface, alt))
		goto errout;

	udev->alts[iface] = alt;
	/*
	 * FIXME: Only support single interface USB device first. Need fix in
	 * future to support composite USB device.
	 */
	usb_dev_reset_ep(udev);
	usb_dev_update_ep(udev);
	return;

errout:
	xfer->status = USB_ERR_STALLED;
	UPRINTF(LDBG, "%d-%s fail to set if, iface %d alt %d\r\n",
			udev->info.path.bus,
			usb_dev_path(&udev->info.path),
			iface,
			alt);
}

static struct usb_block *
usb_dev_prepare_ctrl_xfer(struct usb_xfer *xfer)
{
	int i, idx;
	struct usb_block *ret = NULL;
	struct usb_block *blk = NULL;

	idx = xfer->head;

	if (idx < 0 || idx >= xfer->max_blk_cnt)
		return NULL;

	for (i = 0; i < xfer->ndata; i++) {
		/*
		 * find out the data block and set every
		 * block to be USB_BLOCK_HANDLED
		 */
		blk = &xfer->data[idx];
		if (blk->blen > 0 && !ret)
			ret = blk;

		blk->stat = USB_BLOCK_HANDLED;
		idx = index_inc(idx, xfer->max_blk_cnt);
	}
	return ret;
}

int
usb_dev_reset(void *pdata)
{
	struct usb_dev *udev;
	int rc = 0;

	udev = pdata;

	UPRINTF(LDBG, "reset endpoints\n");
	libusb_reset_device(udev->handle);
	usb_dev_reset_ep(udev);
	usb_dev_update_ep(udev);
	rc = libusb_reset_device(udev->handle);
	if (!rc) {
		usb_dev_reset_ep(udev);
		usb_dev_update_ep(udev);
	}
	return rc;
}

int
usb_dev_data(void *pdata, struct usb_xfer *xfer, int dir, int epctx)
{
	struct usb_dev *udev;
	struct usb_dev_req *r;
	struct usb_native_devinfo *info;
	int rc = 0, epid;
	uint8_t type;
	int i, idx, buf_idx, head, tail, size;
	struct usb_block *b;
	static const char * const type_str[] = {"CTRL", "ISO", "BULK", "INT"};
	static const char * const dir_str[] = {"OUT", "IN"};
	int framelen = 0, framecnt = 0;
	uint16_t maxp;

	udev = pdata;
	info = &udev->info;
	xfer->status = USB_ERR_NORMAL_COMPLETION;
	size = usb_dev_prepare_xfer(xfer, &head, &tail);
	if (size <= 0)
		goto done;

	type = usb_dev_get_ep_type(udev, dir ? TOKEN_IN : TOKEN_OUT, epctx);
	if (type > USB_ENDPOINT_INT) {
		xfer->status = USB_ERR_IOERROR;
		goto done;
	}

	epid = dir ? (0x80 | epctx) : epctx;
	if (!(dir == USB_XFER_IN || dir == USB_XFER_OUT)) {
		xfer->status = USB_ERR_IOERROR;
		goto done;
	}

	maxp = usb_dev_get_ep_maxp(udev, dir, epctx);
	if (type == USB_ENDPOINT_ISOC) {
		/* need to double check it, there might be some non-spec
		 * compatible usb devices in the market.
		 */
		framelen = USB_EP_MAXP_SZ(maxp) * (1 + USB_EP_MAXP_MT(maxp));
		UPRINTF(LDBG, "iso maxp %u framelen %d\r\n", maxp, framelen);

		for (idx = head;
			index_valid(head, tail, xfer->max_blk_cnt, idx);
			idx = index_inc(idx, xfer->max_blk_cnt)) {

			if (xfer->data[idx].blen > framelen)
				UPRINTF(LFTL, "err framelen %d\r\n", framelen);

			if (xfer->data[idx].type == USB_DATA_NONE ||
					xfer->data[idx].type == USB_DATA_PART)
				continue;
			else if (xfer->data[idx].type == USB_DATA_FULL)
				framecnt++;
			else
				UPRINTF(LFTL, "%s:%d error\r\n", __func__, __LINE__);
		}
		UPRINTF(LDBG, "iso maxp %u framelen %d, framecnt %d\r\n", maxp,
				framelen, framecnt);
	}

	r = usb_dev_alloc_req(udev, xfer, dir, size, type ==
			USB_ENDPOINT_ISOC ? framecnt : 0);
	if (!r) {
		xfer->status = USB_ERR_IOERROR;
		goto done;
	}

	r->buf_size = size;
	r->blk_head = head;
	r->blk_tail = tail;
	xfer->reqs[head] = r;
	UPRINTF(LDBG, "%s: %d-%s: explen %d ep%d-xfr [%d-%d %d] rq-%d "
			"[%d-%d %d] dir %s type %s\r\n", __func__,
			info->path.bus, usb_dev_path(&info->path), size, epctx,
			xfer->head, xfer->tail, xfer->ndata, r->seq,
			r->blk_head, r->blk_tail, r->buf_size, dir_str[dir],
			type_str[type]);

	if (!dir) {
		for (idx = head, buf_idx = 0;
				index_valid(head, tail, xfer->max_blk_cnt, idx);
				idx = index_inc(idx, xfer->max_blk_cnt)) {
			b = &xfer->data[idx];
			if (b->type == USB_DATA_PART ||
					b->type == USB_DATA_FULL) {
				memcpy(&r->buffer[buf_idx], b->buf, b->blen);
				buf_idx += b->blen;
			}
		}
	}

	if (type == USB_ENDPOINT_ISOC) {
		for (i = 0, idx = head;
				index_valid(head, tail, xfer->max_blk_cnt, idx);
				idx = index_inc(idx, xfer->max_blk_cnt)) {
			int len = xfer->data[idx].blen;

			if (xfer->data[idx].type == USB_DATA_NONE) {
				continue;
			} else if (xfer->data[idx].type == USB_DATA_PART) {
				r->trn->iso_packet_desc[i].length += len;
				continue;
			} else if (xfer->data[idx].type == USB_DATA_FULL) {
				r->trn->iso_packet_desc[i].length += len;
			} else {
				UPRINTF(LFTL, "%s:%d error\r\n",
						__func__, __LINE__);
			}

			UPRINTF(LDBG, "desc[%d].length %d\r\n", i,
					r->trn->iso_packet_desc[i].length);
			i++;
		}
	}

	if (type == USB_ENDPOINT_BULK) {
		libusb_fill_bulk_transfer(r->trn, udev->handle, epid,
				r->buffer, size, usb_dev_comp_cb, r, 0);

	} else if (type == USB_ENDPOINT_INT) {
		libusb_fill_interrupt_transfer(r->trn, udev->handle, epid,
				r->buffer, size, usb_dev_comp_cb, r, 0);

	} else if (type == USB_ENDPOINT_ISOC) {
		libusb_fill_iso_transfer(r->trn, udev->handle, epid,
				r->buffer, size, framecnt,
				usb_dev_comp_cb, r, 0);

	} else {
		UPRINTF(LFTL, "%s: wrong endpoint type %d\r\n", __func__, type);
		if (r->buffer)
			free(r->buffer);
		if (r->trn)
			libusb_free_transfer(r->trn);
		free(r);
		xfer->status = USB_ERR_INVAL;
	}

	rc = libusb_submit_transfer(r->trn);
	if (rc) {
		xfer->status = USB_ERR_IOERROR;
		UPRINTF(LDBG, "libusb_submit_transfer fail: %d\n", rc);
	}
done:
	return xfer->status;
}

static void
clear_uas_desc(struct usb_dev *udev, uint8_t *data, int len)
{
	struct usb_devpath *path;
	int32_t i;

	/* only process configuration descriptor */
	if (len < 2 || data[1] != 0x2)
		return;

	i = 0;
	path = &udev->info.path;
	while (i < len) {
		/* When UAS protocol is found in interface descriptor, set it
		 * to invalid value.
		 *
		 * According to USB3 spec 9.6.5, Standard Interface Descriptor,
		 * data[i+0] => bLength
		 * data[i+1] => bDescriptorType
		 * data[i+7] => bInterfaceProtocol
		 */
		if (data[i] == 9 && data[i+1] == 0x4 && data[i+7] == 0x62) {
			UPRINTF(LFTL, "%d-%s: clear uas protocol\r\n",
					path->bus, usb_dev_path(path));
			data[i+7] = 0;
		}
		i = i + data[i];
	}
}

int
usb_dev_request(void *pdata, struct usb_xfer *xfer)
{
	struct usb_dev *udev;
	uint8_t  request_type;
	uint8_t  request;
	uint16_t value;
	uint16_t index;
	uint16_t len;
	struct usb_block *blk;
	uint8_t *data;
	int rc;
	bool need_chk_uas = false;

	udev = pdata;
	xfer->status = USB_ERR_NORMAL_COMPLETION;
	if (!udev->info.priv_data || !xfer->ureq) {
		UPRINTF(LWRN, "invalid request\r\n");
		xfer->status = USB_ERR_IOERROR;
		goto out;
	}

	request_type = xfer->ureq->bmRequestType;
	request      = xfer->ureq->bRequest;
	value        = xfer->ureq->wValue;
	index        = xfer->ureq->wIndex;
	len          = xfer->ureq->wLength;

	blk = usb_dev_prepare_ctrl_xfer(xfer);
	data = blk ? blk->buf : NULL;

	UPRINTF(LDBG, "%d-%s: urb: type 0x%x req 0x%x val 0x%x idx %d len %d "
			"data %d\r\n", udev->info.path.bus,
			usb_dev_path(&udev->info.path), request_type, request,
			value, index, len, blk ? blk->blen : 0);

	/*
	 * according to usb spec, control transfer may have no
	 * DATA STAGE, so the valid situations are:
	 *   a. with DATA STAGE: blk != NULL && len > 0
	 *   b. without DATA STAGE: blk == NULL && len == 0
	 * any other situations, just skip process
	 */
	if ((!blk && len > 0) || (blk && len <= 0))
		goto out;

	switch (UREQ(request, request_type)) {
	case UREQ(UR_SET_ADDRESS, UT_WRITE_DEVICE):
		UPRINTF(LDBG, "UR_SET_ADDRESS\n");
		udev->addr = value;
		goto out;
	case UREQ(UR_SET_CONFIG, UT_WRITE_DEVICE):
		UPRINTF(LDBG, "UR_SET_CONFIG\n");
		usb_dev_set_config(udev, xfer, value & 0xff);
		goto out;
	case UREQ(UR_SET_INTERFACE, UT_WRITE_INTERFACE):
		UPRINTF(LDBG, "UR_SET_INTERFACE\n");
		usb_dev_set_if(udev, index, value, xfer);
		goto out;
	case UREQ(UR_GET_DESCRIPTOR, UT_READ):
		if (value == 0x0200)
			need_chk_uas = true;
		break;
	case UREQ(UR_CLEAR_FEATURE, UT_WRITE_ENDPOINT):
		if (value) {
			/* according to usb spec (ch9), this is impossible */
			UPRINTF(LWRN, "Clear Feature request with non-zero "
					"value %d\r\n", value);
			break;
		}

		UPRINTF(LDBG, "UR_CLEAR_HALT\n");
		rc = libusb_clear_halt(udev->handle, index);
		if (rc)
			UPRINTF(LWRN, "fail to clear halted ep, rc %d\r\n", rc);
		goto out;

	}

	/* (data == NULL && len == 0) is okey to libusb_control_transfer */
	if (data == NULL && len) {
		xfer->status = USB_ERR_IOERROR;
		UPRINTF(LFTL, "%s unexpected NULL data\r\n", __func__);
		goto out;
	}

	/* send it to physical device */
	/* FIXME: In the process of implementation of USB isochronouse transfer,
	 * the timeout time is not enough for Plantronics headset. So this
	 * issue should be investigated detailly, and at worst situation, the
	 * control transfer should be also changed to async operation.
	 */
	rc = libusb_control_transfer(udev->handle, request_type, request,
			value, index, data, len, 300);

	if (rc < 0) {
		xfer->status = usb_dev_err_convert(rc);
		goto out;
	}
	/* TODO: Currently, the USB Attached SCSI (UAS) protocol is not
	 * supported and the following code is used as a workaround now.
	 * UAS will be implemented in future.
	 */
	if (need_chk_uas && data)
		clear_uas_desc(udev, data, rc);

	if (blk) {
		blk->blen = len - rc;
		blk->bdone += rc;
		xfer->status = blk->blen > 0 ? USB_ERR_SHORT_XFER :
			USB_ERR_NORMAL_COMPLETION;
	} else
		xfer->status = USB_ERR_NORMAL_COMPLETION;


	UPRINTF(LDBG, "%d-%s: usb rc %d, blk %p, blen %u bdon %u\n",
			udev->info.path.bus, usb_dev_path(&udev->info.path),
			rc, blk, blk ? blk->blen : 0, blk ? blk->bdone : 0);
out:
	return xfer->status;
}

void *
usb_dev_init(void *pdata, char *opt)
{
	struct usb_dev *udev = NULL;
	struct libusb_device_descriptor desc;
	struct usb_native_devinfo *di;
	int ver;

	di = pdata;

	libusb_get_device_descriptor(di->priv_data, &desc);
	UPRINTF(LINF, "Found USB device: %d-%s\r\nPID(0x%X), VID(0x%X) CLASS"
			"(0x%X) SUBCLASS(0x%X) BCD(0x%X) SPEED(%d)\r\n",
			di->path.bus, usb_dev_path(&di->path), di->pid,
			di->vid, desc.bDeviceClass, desc.bDeviceSubClass,
			di->bcd, di->speed);

	/* allocate and populate udev */
	udev = calloc(1, sizeof(struct usb_dev));
	if (!udev)
		goto errout;

	/* this is a root hub */
	if (ROOTHUB_PORT(di->path) == 0)
		goto errout;

	switch (desc.bcdUSB) {
	case 0x320:
	case 0x310:
	case 0x300:
		ver = 3; break;
	case 0x200:
	case 0x201:
	case 0x210:
	case 0x110:
		/* 0x110 is a special case.
		 * xHCI spec v1.0 was released in 2010 and USB spec v1.1 was
		 * released in 1998, anything about USB 1.x could hardly be
		 * found in xHCI spec. So here use USB 2.x to do the emulation
		 * for USB 1.x device.
		 * And one more thing, it is almost impossible to find an USB
		 * 1.x device today.
		 */
		ver = 2; break;
	default:
		goto errout;
	}

	udev->info    = *di;
	udev->version = ver;
	udev->handle  = NULL;

	/* configure physical device through libusb library */
	if (libusb_open(udev->info.priv_data, &udev->handle)) {
		UPRINTF(LWRN, "fail to open device.\r\n");
		goto errout;
	}

	if (usb_dev_native_toggle_if_drivers(udev, 0) < 0) {
		UPRINTF(LWRN, "fail to detach interface driver.\r\n");
		goto errout;
	}
	return udev;

errout:
	if (udev && udev->handle)
		libusb_close(udev->handle);

	free(udev);
	return NULL;
}

void
usb_dev_free_request(void *pdata)
{
	struct libusb_transfer *trn;

	trn = pdata;
	libusb_free_transfer(trn);
}

void
usb_dev_cancel_request(void *pdata)
{
	struct libusb_transfer *trn;

	trn = pdata;
	libusb_cancel_transfer(trn);
}

void
usb_dev_deinit(void *pdata)
{
	int rc = 0;
	struct usb_dev *udev;

	udev = pdata;
	if (udev) {
		if (udev->handle) {
			rc = usb_dev_native_toggle_if_drivers(udev, 1);
			if (rc)
				UPRINTF(LWRN, "fail to attach if drv rc:%d\r\n",
						rc);
			libusb_close(udev->handle);
		}
		free(udev);
	}
}

int
usb_dev_info(void *pdata, int type, void *value, int size)
{
	struct usb_dev *udev;
	int sz;
	void *pv;

	udev = pdata;

	switch (type) {
	case USB_INFO_VERSION:
		sz = sizeof(udev->version);
		pv = &udev->version;
		break;
	case USB_INFO_SPEED:
		sz = sizeof(udev->info.speed);
		udev->info.speed = libusb_speed_to_usb_speed(udev->info.speed);
		pv = &udev->info.speed;
		break;
	case USB_INFO_BUS:
		sz = sizeof(udev->info.path.bus);
		pv = &udev->info.path.bus;
		break;
	case USB_INFO_PORT:
		sz = sizeof(udev->info.path.path[0]);
		pv = &udev->info.path.path[0];
		break;
	case USB_INFO_VID:
		sz = sizeof(udev->info.vid);
		pv = &udev->info.vid;
		break;
	case USB_INFO_PID:
		sz = sizeof(udev->info.pid);
		pv = &udev->info.pid;
		break;
	default:
		return -1;
	}

	if (size == sz)
		memcpy(value, pv, sz);

	return sz == size ? 0 : -1;
}

static void *
usb_dev_sys_thread(void *arg)
{
	struct timeval t = {1, 0};
	int rc = 0;

	while (g_ctx.thread_exit == 0) {
		rc = libusb_handle_events_timeout(g_ctx.libusb_ctx, &t);
		if (rc < 0)
			/* TODO: maybe one second as interval is too long which
			 * may result of slower USB enumeration process.
			 */
			sleep(1);
	}

	UPRINTF(LINF, "poll thread exit\n\r");
	return NULL;
}

static int
usb_dev_native_sys_conn_cb(struct libusb_context *ctx, struct libusb_device
		*ldev, libusb_hotplug_event event, void *pdata)
{
	struct usb_native_devinfo di;
	bool ret;

	UPRINTF(LDBG, "connect event\r\n");

	if (!ctx || !ldev) {
		UPRINTF(LFTL, "connect callback fails!\n");
		return -1;
	}

	ret = usb_get_native_devinfo(ldev, &di, NULL);
	if (ret == false)
		return 0;

	if (g_ctx.conn_cb)
		g_ctx.conn_cb(g_ctx.hci_data, &di);

	return 0;
}

static int
usb_dev_native_sys_disconn_cb(struct libusb_context *ctx, struct libusb_device
		*ldev, libusb_hotplug_event event, void *pdata)
{
	struct usb_native_devinfo di;
	bool ret;

	UPRINTF(LDBG, "disconnect event\r\n");

	if (!ctx || !ldev) {
		UPRINTF(LFTL, "disconnect callback fails!\n");
		return -1;
	}

	ret = usb_get_native_devinfo(ldev, &di, NULL);
	if (ret == false)
		return 0;

	if (g_ctx.disconn_cb)
		g_ctx.disconn_cb(g_ctx.hci_data, &di);

	return 0;
}

int
usb_dev_sys_init(usb_dev_sys_cb conn_cb, usb_dev_sys_cb disconn_cb,
		usb_dev_sys_cb notify_cb, usb_dev_sys_cb intr_cb,
		usb_dev_sys_cb lock_ep_cb, usb_dev_sys_cb unlock_ep_cb,
		void *hci_data, int log_level)
{
	libusb_hotplug_event native_conn_evt;
	libusb_hotplug_event native_disconn_evt;
	libusb_hotplug_flag flags;
	libusb_hotplug_callback_handle native_conn_handle;
	libusb_hotplug_callback_handle native_disconn_handle;
	int native_pid, native_vid, native_cls, rc;
	int num_devs;

	usb_set_log_level(log_level);

	if (g_ctx.libusb_ctx) {
		UPRINTF(LFTL, "port mapper is already initialized.\r\n");
		return -1;
	}

	rc = libusb_init(&g_ctx.libusb_ctx);
	if (rc < 0) {
		UPRINTF(LFTL, "libusb_init fails, rc:%d\r\n", rc);
		return -1;
	}

	g_ctx.hci_data     = hci_data;
	g_ctx.conn_cb      = conn_cb;
	g_ctx.disconn_cb   = disconn_cb;
	g_ctx.notify_cb    = notify_cb;
	g_ctx.intr_cb      = intr_cb;
	g_ctx.lock_ep_cb   = lock_ep_cb;
	g_ctx.unlock_ep_cb = unlock_ep_cb;

	num_devs = usb_dev_scan_dev(&g_ctx.devlist);
	UPRINTF(LINF, "found %d devices before Guest OS booted\r\n", num_devs);

	native_conn_evt    = LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED;
	native_disconn_evt = LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT;
	native_pid         = LIBUSB_HOTPLUG_MATCH_ANY;
	native_vid         = LIBUSB_HOTPLUG_MATCH_ANY;
	native_cls         = LIBUSB_HOTPLUG_MATCH_ANY;
	flags              = 0;

	/* register connect callback */
	rc = libusb_hotplug_register_callback(g_ctx.libusb_ctx, native_conn_evt,
			flags, native_vid, native_pid, native_cls,
			usb_dev_native_sys_conn_cb, NULL, &native_conn_handle);
	if (rc != LIBUSB_SUCCESS)
		goto errout;

	/* register disconnect callback */
	rc = libusb_hotplug_register_callback(g_ctx.libusb_ctx,
			native_disconn_evt, flags, native_vid, native_pid,
			native_cls, usb_dev_native_sys_disconn_cb, NULL,
			&native_disconn_handle);
	if (rc != LIBUSB_SUCCESS) {
		libusb_hotplug_deregister_callback(g_ctx.libusb_ctx,
				native_conn_handle);
		goto errout;
	}

	/* this is for guest rebooting purpose */
	g_ctx.conn_handle = native_conn_handle;
	g_ctx.disconn_handle = native_disconn_handle;
	g_ctx.thread_exit = 0;

	if (pthread_create(&g_ctx.thread, NULL, usb_dev_sys_thread, NULL)) {
		libusb_hotplug_deregister_callback(g_ctx.libusb_ctx,
				native_conn_handle);
		libusb_hotplug_deregister_callback(g_ctx.libusb_ctx,
				native_disconn_handle);
		goto errout;
	}
	pthread_setname_np(g_ctx.thread, "usb_dev_sys");

	return 0;

errout:
	if (g_ctx.devlist) {
		libusb_free_device_list(g_ctx.devlist, 1);
		g_ctx.devlist = NULL;
	}

	if (g_ctx.libusb_ctx) {
		libusb_exit(g_ctx.libusb_ctx);
		g_ctx.libusb_ctx = NULL;
	}
	return -1;
}

void
usb_dev_sys_deinit(void)
{
	if (!g_ctx.libusb_ctx)
		return;

	UPRINTF(LINF, "port-mapper de-initialization\r\n");
	libusb_hotplug_deregister_callback(g_ctx.libusb_ctx, g_ctx.conn_handle);
	libusb_hotplug_deregister_callback(g_ctx.libusb_ctx,
			g_ctx.disconn_handle);

	g_ctx.thread_exit = 1;
	pthread_join(g_ctx.thread, NULL);

	if (g_ctx.devlist) {
		libusb_free_device_list(g_ctx.devlist, 1);
		g_ctx.devlist = NULL;
	}

	libusb_exit(g_ctx.libusb_ctx);
	g_ctx.libusb_ctx = NULL;
}
