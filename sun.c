/*
 *  linux/drivers/char/xilinx-sscu.c
 *
 *  Copyright (C) 2011 Andrew 'Necromant' Andrianov <contact@necromant.ath.cx>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
 
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
 
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
 
#include <linux/types.h>
#include <linux/cdev.h>
 
//#include "xilinx-sscu.h"
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <linux/delay.h>

#define IMX_GPIO_NR(bank, nr)		(((bank) - 1) * 32 + (nr))

#define PIN_clk      IMX_GPIO_NR(1,6)
#define PIN_sout     IMX_GPIO_NR(1,5)
#define PIN_init_b   IMX_GPIO_NR(1,8)
#define PIN_prog_b   IMX_GPIO_NR(1,9)
#define PIN_done     IMX_GPIO_NR(1,7)

#ifndef _XILINX_SSCU
#define _XILINX_SSCU
struct xsscu_data {
	char *name;
	unsigned int clk;
	unsigned int sout;
	unsigned int init_b;
	unsigned int prog_b;
	unsigned int done;
};
 
enum {
	XSSCU_STATE_IDLE,
	XSSCU_STATE_UPLOADING,
	XSSCU_STATE_UPLOAD_DONE,
	XSSCU_STATE_DISABLED,
	XSSCU_STATE_PROG_ERROR,
};
 
struct xsscu_device_data {
	struct xsscu_data *pdata;
	int open;
	int state;
	char *read_ptr;
	char msg_buffer[128];
};
#endif
 
#define DRVNAME "xilinx-sscu"
#define DEVNAME "fpga"
#define DRVVER	"0.1"
 
static int g_debug;
module_param(g_debug, int, 0);	/* and these 2 lines */
MODULE_PARM_DESC(g_debug, "Print lots of useless debug info.");
 
/* This delay is system specific. In my case (200Mhz ARM) I can safely
   define it to nothing to speed things up. But on a faster system you
   may want to define it to something, e.g. udelay(100) if the clk will
   get too fast and crew things up. I do not have a chance to check if
   it's needed on a faster system, so I left it here to be 100% sure.
   Have fun
*/
 
#define DELAY  udelay(10)
 
#define DBG(fmt, ...)	if (g_debug) \
    printk(KERN_DEBUG "%s/%s: " fmt " \n", DRVNAME, __FUNCTION__, ##__VA_ARGS__)
#define INF(fmt, ...)	printk(KERN_INFO "%s: " fmt " \n", DRVNAME, ##__VA_ARGS__)
#define ERR(fmt, ...)	printk(KERN_ERR "%s: " fmt " \n", DRVNAME, ##__VA_ARGS__)
 
static inline char *xsscu_state2char(struct xsscu_device_data *dev_data)
{
	switch (dev_data->state) {
	case XSSCU_STATE_UPLOAD_DONE:
	case XSSCU_STATE_IDLE:
		if (gpio_get_value(dev_data->pdata->done))
			return "Online";
		else
			return "Unprogrammed/Error";
	case XSSCU_STATE_DISABLED:
		return "Offline";
	case XSSCU_STATE_PROG_ERROR:
		return "Bitstream error";
	default:
		return "Bug!";
	}
}
 
static int xsscu_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc;
	struct xsscu_device_data *dev_data;
	misc = file->private_data;
	dev_data = misc->this_device->platform_data;
	if (dev_data->open)
		return -EBUSY;
	dev_data->open++;
	DBG("Device %s opened", dev_data->pdata->name);
	sprintf(dev_data->msg_buffer,
		"DEVICE:\t%s\nINIT_B:\t%d\nDONE:\t%d\nSTATE:\t%s\n",
		dev_data->pdata->name,
		gpio_get_value(dev_data->pdata->init_b),
		gpio_get_value(dev_data->pdata->done),
		xsscu_state2char(dev_data)
	    );
	dev_data->read_ptr = dev_data->msg_buffer;
	return 0;
}
 
static int send_clocks(struct xsscu_data *p, int c)
{
 
	while (c--) {
		gpio_direction_output(p->clk, 0);
		DELAY;
		gpio_direction_output(p->clk, 1);
		DELAY;
		if (1 == gpio_get_value(p->done))
			return 0;
	}
	return 1;
}
 
static inline void xsscu_dbg_state(struct xsscu_data *p)
{
	DBG("INIT_B: %d | DONE: %d",
	    gpio_get_value(p->init_b), gpio_get_value(p->done));
}
 
static int xsscu_release(struct inode *inode, struct file *file)
{
	struct miscdevice *misc;
	struct xsscu_device_data *dev_data;
	int err = 0;
	misc = file->private_data;
	dev_data = misc->this_device->platform_data;
	dev_data->open--;
	switch (dev_data->state) {
	case XSSCU_STATE_UPLOADING:
		err = send_clocks(dev_data->pdata, 10000);
		dev_data->state = XSSCU_STATE_UPLOAD_DONE;
		break;
	case XSSCU_STATE_DISABLED:
		err = 0;
		break;
	}
 
	if (err) {
		ERR("DONE not HIGH or other programming error");
		dev_data->state = XSSCU_STATE_PROG_ERROR;
	}
	xsscu_dbg_state(dev_data->pdata);
	DBG("Device closed");
	/* We must still close the device, hence return ok */
	return 0;
}
 
static ssize_t xsscu_read(struct file *filp, char *buffer,
			  size_t length,
			  loff_t *offset)
{
	struct miscdevice *misc;
	struct xsscu_device_data *dev_data;
	int bytes_read = 0;
	misc = filp->private_data;
	dev_data = misc->this_device->platform_data;
 
	if (*dev_data->read_ptr == 0)
		return 0;
	while (length && *dev_data->read_ptr) {
		put_user(*(dev_data->read_ptr++), buffer++);
		length--;
		bytes_read++;
	}
	return bytes_read;
}
 
static int xsscu_reset_fpga(struct xsscu_data *p)
{
	int i = 50;
	DBG("Resetting FPGA...");
	gpio_direction_output(p->prog_b, 0);
	mdelay(1);
	gpio_direction_output(p->prog_b, 1);
	while (i--) {
		xsscu_dbg_state(p);
		if (gpio_get_value(p->init_b) == 1)
			return 0;
		mdelay(1);
	}
	ERR("FPGA reset failed");
	return 1;
}
 
static ssize_t xsscu_write(struct file *filp,
			   const char *buff, size_t len, loff_t * off)
{
	struct miscdevice *misc;
	struct xsscu_device_data *dev_data;
	int i;
	int k;
	i = 0;
	misc = filp->private_data;
	dev_data = misc->this_device->platform_data;
 
	if ((*off == 0)) {
		if (strncmp(buff, "disable", 7) == 0) {
			DBG("Disabling FPGA");
			gpio_direction_output(dev_data->pdata->prog_b, 0);
			dev_data->state = XSSCU_STATE_DISABLED;
			goto all_written;
		} else if (xsscu_reset_fpga(dev_data->pdata) != 0)
			return -EIO;
		/*Wait a little bit, before starting to clock the fpga,
		as the datasheet suggests */
		mdelay(1);
		gpio_direction_output(dev_data->pdata->clk, 0);
		dev_data->state = XSSCU_STATE_UPLOADING;
	}
	/* bitbang data */
	while (i < len) {
		for (k = 7; k >= 0; k--) {
			gpio_direction_output(dev_data->pdata->sout,
					      (buff[i] & (1 << k)));
			gpio_direction_output(dev_data->pdata->clk, 1);
			DELAY;
			gpio_direction_output(dev_data->pdata->clk, 0);
			DELAY;
		}
		i++;
	}
all_written:
	*off += len;
	return len;
}
 
static const struct file_operations xsscu_fileops = {
	.owner = THIS_MODULE,
	.write = xsscu_write,
	.read = xsscu_read,
	.open = xsscu_open,
	.release = xsscu_release,
	.llseek = no_llseek,
};
 
static int xsscu_create_miscdevice(struct xsscu_data *p, int id)
{
	struct miscdevice *mdev;
	struct xsscu_device_data *dev_data;
	char *nm;
	int err;
	mdev = kzalloc(sizeof(struct miscdevice), GFP_KERNEL);
	if (!mdev) {
		ERR("Misc device allocation failed");
		return -ENOMEM;
	}
	nm = kzalloc(64, GFP_KERNEL);
	if (!nm) {
		err = -ENOMEM;
		goto freemisc;
	}
	dev_data = kzalloc(sizeof(struct xsscu_device_data), GFP_KERNEL);
	if (!dev_data) {
		err = -ENOMEM;
		goto freenm;
	}
 
	snprintf(nm, 64, "fpga%d", id);
	mdev->name = nm;
	mdev->fops = &xsscu_fileops;
	mdev->minor = MISC_DYNAMIC_MINOR;
	err = misc_register(mdev);
	if (!err) {
		mdev->this_device->platform_data = dev_data;
		dev_data->pdata = p;
	}
 
	return err;
 
freenm:
	kfree(nm);
freemisc:
	kfree(mdev);
 
	return err;
}
 
static int xsscu_pore(void)
{
	int err;
	int id;
	struct xsscu_data *pdata ;
	pdata=kzalloc(sizeof(struct xsscu_data), GFP_KERNEL);

	if (!pdata) {
		ERR("Missing platform_data, sorry dude");
		return -ENOMEM;
		goto free_xsscu_data;
	}
	pdata->name="Xilinx Spartan-XC3S500E";
	pdata->clk=PIN_clk;//6
	pdata->sout=PIN_sout;//5
	pdata->prog_b=PIN_prog_b;//9
	pdata->done=PIN_done;//7
	pdata->init_b=PIN_init_b;//8
	/* some id magic */

	id = 0;
	/* claim gpio pins */
	err = gpio_request(pdata->clk, "xilinx-sscu-clk") +
	    gpio_request(pdata->done, "xilinx-sscu-done") +
	    gpio_request(pdata->init_b, "xilinx-sscu-init_b") +
	    gpio_request(pdata->prog_b, "xilinx-sscu-prog_b") +
	    gpio_request(pdata->sout, "xilinx-sscu-sout");
	if (err) {
		ERR("Failed to claim required GPIOs, bailing out");
		return err;
	}
 
	gpio_direction_input(pdata->init_b);
	gpio_direction_input(pdata->done);
 
	err = xsscu_create_miscdevice(pdata, id);
	if (!err)
		INF("FPGA Device %s registered as /dev/fpga%d", pdata->name,
		    id);
	return err;
free_xsscu_data:
	kfree(pdata);
	return err;
}
 

 
static int __init xsscu_init(void)
{
	INF("Xilinx Slave Serial Configuration Upload Driver " DRVVER);
	return xsscu_pore();
}
 
static void __exit xsscu_cleanup(void)
{
	/* Normally you would not like to unload this driver. */

}
 
module_init(xsscu_init);
module_exit(xsscu_cleanup);
 
MODULE_AUTHOR("Andrew 'Necromant' Andrianov <necromant@necromant.ath.cx>");
MODULE_DESCRIPTION("Xilinx Slave Serial BitBang Uploader driver");
MODULE_LICENSE("GPL");