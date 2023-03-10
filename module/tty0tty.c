/* ########################################################################

   tty0tty - linux null modem emulator (module)  for kernel > 3.8

   ########################################################################

   Copyright (c) : 2013-2022  Luis Claudio Gambôa Lopes

   Based in Tiny TTY driver -  Copyright (C) 2002-2004 Greg Kroah-Hartman (greg@kroah.com)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

   For e-mail suggestions :  lcgamboa@yahoo.com
   ######################################################################## */



#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/sched.h>
#include <asm/uaccess.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/signal.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)

static int user_termios_to_kernel_termios(struct ktermios *k,
						 struct termios2 __user *u)
{
	return copy_from_user(k, u, sizeof(struct termios2));
}

static int kernel_termios_to_user_termios(struct termios2 __user *u,
						 struct ktermios *k)
{
	return copy_to_user(u, k, sizeof(struct termios2));
}
static int user_termios_to_kernel_termios_1(struct ktermios *k,
						   struct termios __user *u)
{
	return copy_from_user(k, u, sizeof(struct termios));
}

static int kernel_termios_to_user_termios_1(struct termios __user *u,
						   struct ktermios *k)
{
	return copy_to_user(u, k, sizeof(struct termios));
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 13, 0)
int tty_check_change(struct tty_struct *tty);
speed_t tty_termios_input_baud_rate(struct ktermios *termios);
#else
#ifndef tty_alloc_driver
#define tty_alloc_driver(x, y) alloc_tty_driver(x)
#endif
#define tty_driver_kref_puf(x) put_tty_driver(x)
#endif

#define DRIVER_VERSION "v1.4"
#define DRIVER_AUTHOR "Luis Claudio Gamboa Lopes <lcgamboa@yahoo.com>"
#define DRIVER_DESC "tty0tty null modem driver"

/* Module information */
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");


#define TTY0TTY_MAJOR		0	/* dynamic allocation of major number */
#define TTY0TTY_MINORS		8	/* device number, always even*/

/* fake UART values */
//out
#define MCR_DTR		0x01
#define MCR_RTS		0x02
#define MCR_LOOP	0x04
//in
#define MSR_CTS		0x10
#define MSR_CD		0x20
#define MSR_DSR		0x40
#define MSR_RI		0x80


static struct tty_port tport[TTY0TTY_MINORS];

struct tty0tty_serial {
	struct tty_struct	*tty;		/* pointer to the tty for this device */
	int			open_count;	/* number of times this port has been opened */
	struct semaphore	sem;		/* locks this structure */

	/* for tiocmget and tiocmset functions */
	int			msr;		/* MSR shadow */
	int			mcr;		/* MCR shadow */

	/* for ioctl fun */
	struct serial_struct	serial;
	wait_queue_head_t	wait;
	struct async_icount	icount;
};

static struct tty0tty_serial *tty0tty_table[TTY0TTY_MINORS];	/* initially all NULL */



/*attributes*/

/*
 * /sys/device/virtual/tty/tntx/
 *
 * the attribute 'baudrate' contains the baudrate of virtual serial 
 * port (return 0 if port is not open) and it supports poll() 
 * to detect when value is changed.
 */
static struct device *tty0tty_dev[TTY0TTY_MINORS];

/* Sysfs attribute */
static ssize_t baudrate_show(struct device *dev,
			    struct device_attribute *attr, char *buf){
	int baud = 0;
	struct tty0tty_serial *tty0tty = dev_get_drvdata(dev);

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s \n", __FUNCTION__);
#endif

	if (!tty0tty)
		return sprintf(buf, "%i\n", baud);

    if (!tty0tty->tty)
		return sprintf(buf, "%i\n", baud);

    down(&tty0tty->sem);
	if (tty0tty->open_count) 
	{
		baud = tty_get_baud_rate(tty0tty->tty);
	}
	up(&tty0tty->sem);

	return sprintf(buf, "%i\n", baud);
}

static DEVICE_ATTR_RO(baudrate);

static struct attribute *tty0tty_dev_attrs[] = {
	&dev_attr_baudrate.attr,
	NULL
};

ATTRIBUTE_GROUPS(tty0tty_dev);


static struct tty0tty_serial *get_shadow_tty(int index)
{
	struct tty0tty_serial *shadow = NULL;
	int shadow_idx = index ^ 1;

	if ((index < TTY0TTY_MINORS) &&
		(tty0tty_table[shadow_idx] != NULL) &&
		(tty0tty_table[shadow_idx]->open_count > 0)) {
		shadow = tty0tty_table[shadow_idx];
#ifdef SCULL_DEBUG
		printk(KERN_DEBUG "%s - shadow idx: %d\n", __FUNCTION__, shadow_idx);
#endif
	}

	return shadow;
}

static void update_shadow_msr(int index, int msr)
{
	struct tty0tty_serial *shadow;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - 0x%02x\n", __FUNCTION__, msr);
#endif

	if ((shadow = get_shadow_tty(index)) != NULL) {
		if((shadow->msr & MSR_CTS) != (msr & MSR_CTS))
			shadow->icount.cts++;

		if((shadow->msr & MSR_DSR) != (msr & MSR_DSR))
			shadow->icount.dsr++;

		if((shadow->msr & MSR_CD) != (msr & MSR_CD))
			shadow->icount.dcd++;

		if (msr != shadow->msr) {
			shadow->msr = msr;
			wake_up_interruptible(&shadow->wait);
		}
	}
}

static int tty0tty_open(struct tty_struct *tty, struct file *file)
{
	struct tty0tty_serial *tty0tty;
	struct tty0tty_serial *shadow;
	int index;
	int msr=0;
	int mcr=0;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i \n", __FUNCTION__,tty->index);
#endif
	/* initialize the pointer in case something fails */
	tty->driver_data = NULL;

	/* get the serial object associated with this tty pointer */
	index = tty->index;
	tty0tty = tty0tty_table[index];
	tport[index].tty=tty;
	tty->port = &tport[index];

	if ((shadow = get_shadow_tty(index)) != NULL)
		mcr = shadow->mcr;

//null modem connection

	if( (mcr & MCR_RTS) == MCR_RTS )
	{
		msr |= MSR_CTS;
	}

	if( (mcr & MCR_DTR) == MCR_DTR )
	{
		msr |= MSR_DSR;
		msr |= MSR_CD;
	}

	tty0tty->msr = msr;
	tty0tty->mcr = 0;
	memset(&tty0tty->icount, 0, sizeof(tty0tty->icount));

	init_waitqueue_head(&tty0tty->wait);

	/* register the tty driver_data */

	down(&tty0tty->sem);

	/* save our structure within the tty structure */
	tty->driver_data = tty0tty;
	tty0tty->tty = tty;

	++tty0tty->open_count;

	up(&tty0tty->sem);

    /* Notify open*/
	if (tty0tty_dev[index]){
		sysfs_notify(&tty0tty_dev[index]->kobj, NULL, "baudrate");
#ifdef SCULL_DEBUG
	    printk(KERN_DEBUG "%s - %s\n", __FUNCTION__, "sysfs_notify baudrate (open)");
#endif
	}

	return 0;
}

static void do_close(struct tty0tty_serial *tty0tty)
{
	unsigned int msr=0;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i\n", __FUNCTION__,tty0tty->tty->index);
#endif
	update_shadow_msr(tty0tty->tty->index, msr);

	down(&tty0tty->sem);
	if (tty0tty->open_count) {
		--tty0tty->open_count;
	}
	up(&tty0tty->sem);

	/* Notify close*/
	if (tty0tty_dev[tty0tty->tty->index]){
		sysfs_notify(&tty0tty_dev[tty0tty->tty->index]->kobj, NULL, "baudrate");
#ifdef SCULL_DEBUG
	    printk(KERN_DEBUG "%s - %s\n", __FUNCTION__, "sysfs_notify baudrate (close)");
#endif
	}

	return;
}

static void tty0tty_close(struct tty_struct *tty, struct file *file)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i\n", __FUNCTION__, tty->index);
#endif
	if (tty0tty)
		do_close(tty0tty);
}

static int tty0tty_write(struct tty_struct *tty, const unsigned char *buffer, int count)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;
	struct tty0tty_serial *shadow;
	struct tty_struct  *ttyx = NULL;

#ifdef SCULL_DEBUG
	int i;
	printk(KERN_DEBUG "%s -tnt%i  [%02i] \n", __FUNCTION__,tty->index, count);
	for(i=0;i<count;i++)
		printk(KERN_DEBUG " 0x%02X \n",buffer[i]);
#endif

	if (!tty0tty)
		return -ENODEV;

	down(&tty0tty->sem);
	if (tty0tty->open_count)
	{
	  if ((shadow = get_shadow_tty(tty0tty->tty->index)) != NULL)
	  	  ttyx = shadow->tty;
//        tty->low_latency=1;
	  if(ttyx != NULL)
	  {
		  tty_insert_flip_string(ttyx->port, buffer, count);
		  tty_flip_buffer_push(ttyx->port);
	  }
	}
	up(&tty0tty->sem);
	return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
static unsigned int tty0tty_write_room(struct tty_struct *tty)
#else
static int tty0tty_write_room(struct tty_struct *tty)
#endif
{
	struct tty0tty_serial *tty0tty = tty->driver_data;
	int room = 0;
	
#ifdef SCULL_DEBUG
//	printk(KERN_DEBUG "%s - tnt%i\n", __FUNCTION__,tty->index);
#endif

	if (!tty0tty)
		return -ENODEV;

	down(&tty0tty->sem);
	if (tty0tty->open_count) 
	{
		/* calculate how much room is left in the device */
	    room = 255;
	}
	up(&tty0tty->sem);
	return room;
}



#define RELEVANT_IFLAG(iflag) ((iflag) & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void tty0tty_set_termios(struct tty_struct *tty, const struct ktermios *old_termios)
#else
static void tty0tty_set_termios(struct tty_struct *tty, struct ktermios *old_termios)
#endif
{
	unsigned int cflag;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s -tnt%i \n", __FUNCTION__, tty->index);
#endif

	cflag = tty->termios.c_cflag;

	/* check that they really want us to change something */
	if (old_termios) {
		if ((cflag == old_termios->c_cflag) &&
		    (RELEVANT_IFLAG(tty->termios.c_iflag) ==
		     RELEVANT_IFLAG(old_termios->c_iflag))) {
#ifdef SCULL_DEBUG
			printk(KERN_DEBUG " - nothing to change...\n");
#endif
			return;
		}
	}

#ifdef SCULL_DEBUG
	/* get the byte size */
	switch (cflag & CSIZE) {
		case CS5:
			printk(KERN_DEBUG " - data bits = 5\n");
			break;
		case CS6:
			printk(KERN_DEBUG " - data bits = 6\n");
			break;
		case CS7:
			printk(KERN_DEBUG " - data bits = 7\n");
			break;
		default:
		case CS8:
			printk(KERN_DEBUG " - data bits = 8\n");
			break;
	}

	/* determine the parity */
	if (cflag & PARENB)
		if (cflag & PARODD)
			printk(KERN_DEBUG " - parity = odd\n");
		else
			printk(KERN_DEBUG " - parity = even\n");
	else
		printk(KERN_DEBUG " - parity = none\n");

	/* figure out the stop bits requested */
	if (cflag & CSTOPB)
		printk(KERN_DEBUG " - stop bits = 2\n");
	else
		printk(KERN_DEBUG " - stop bits = 1\n");

	/* figure out the hardware flow control settings */
	if (cflag & CRTSCTS)
		printk(KERN_DEBUG " - RTS/CTS is enabled\n");
	else
		printk(KERN_DEBUG " - RTS/CTS is disabled\n");

	/* determine software flow control */
	/* if we are implementing XON/XOFF, set the start and
	 * stop character in the device */
	if (I_IXOFF(tty) || I_IXON(tty)) {
		unsigned char stop_char  = STOP_CHAR(tty);
		unsigned char start_char = START_CHAR(tty);

		/* if we are implementing INBOUND XON/XOFF */
		if (I_IXOFF(tty))
			printk(KERN_DEBUG " - INBOUND XON/XOFF is enabled, "
				"XON = %2x, XOFF = %2x\n", start_char, stop_char);
		else
			printk(KERN_DEBUG" - INBOUND XON/XOFF is disabled\n");

		/* if we are implementing OUTBOUND XON/XOFF */
		if (I_IXON(tty))
			printk(KERN_DEBUG" - OUTBOUND XON/XOFF is enabled, "
				"XON = %2x, XOFF = %2x\n", start_char, stop_char);
		else
			printk(KERN_DEBUG" - OUTBOUND XON/XOFF is disabled\n");
	}

	/* get the baud rate wanted */
	printk(KERN_DEBUG " - baud rate = %d\n", tty_get_baud_rate(tty));
#endif

    /* Notify speed*/
	if (tty0tty_dev[tty->index]){
		sysfs_notify(&tty0tty_dev[tty->index]->kobj, NULL, "baudrate");
#ifdef SCULL_DEBUG
	    printk(KERN_DEBUG "%s - %s\n", __FUNCTION__, "sysfs_notify baudrate (change)");
#endif
	}

}


static int tty0tty_tiocmget(struct tty_struct *tty)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;

	unsigned int result = 0;
	unsigned int msr = tty0tty->msr;
	unsigned int mcr = tty0tty->mcr;


	result = ((mcr & MCR_DTR)  ? TIOCM_DTR  : 0) |	/* DTR is set */
		((mcr & MCR_RTS)  ? TIOCM_RTS  : 0) |	/* RTS is set */
		((mcr & MCR_LOOP) ? TIOCM_LOOP : 0) |	/* LOOP is set */
		((msr & MSR_CTS)  ? TIOCM_CTS  : 0) |	/* CTS is set */
		((msr & MSR_CD)   ? TIOCM_CAR  : 0) |	/* Carrier detect is set*/
		((msr & MSR_RI)   ? TIOCM_RI   : 0) |	/* Ring Indicator is set */
		((msr & MSR_DSR)  ? TIOCM_DSR  : 0);	/* DSR is set */

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i 0x%08X \n", __FUNCTION__, tty->index, result);
#endif

	return result;
}





static int tty0tty_tiocmset(struct tty_struct *tty,
			unsigned int set, unsigned int clear)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;
	struct tty0tty_serial *shadow;
	unsigned int mcr = tty0tty->mcr;
	unsigned int msr=0;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i set=0x%08X clear=0x%08X \n", __FUNCTION__,tty->index, set ,clear);
#endif
	if ((shadow = get_shadow_tty(tty0tty->tty->index)) != NULL)
		msr = shadow->msr;

//null modem connection

	if (set & TIOCM_RTS)
	{
		mcr |= MCR_RTS;
		msr |= MSR_CTS;
	}

	if (set & TIOCM_DTR)
	{
		mcr |= MCR_DTR;
		msr |= MSR_DSR;
		msr |= MSR_CD;
	}

	if (clear & TIOCM_RTS)
	{
		mcr &= ~MCR_RTS;
		msr &= ~MSR_CTS;
	}

	if (clear & TIOCM_DTR)
	{
		mcr &= ~MCR_DTR;
		msr &= ~MSR_DSR;
		msr &= ~MSR_CD;
	}


	/* set the new MCR value in the device */
	tty0tty->mcr = mcr;

	update_shadow_msr(tty0tty->tty->index, msr);

	return 0;
}


static int tty0tty_ioctl_tiocgserial(struct tty_struct *tty,
			unsigned long arg)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;
	struct serial_struct tmp;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i \n", __FUNCTION__, tty->index);
#endif

	if (!arg)
		return -EFAULT;

	memset(&tmp, 0, sizeof(tmp));

	tmp.type		= tty0tty->serial.type;
	tmp.line		= tty0tty->serial.line;
	tmp.port		= tty0tty->serial.port;
	tmp.irq			= tty0tty->serial.irq;
	tmp.flags		= ASYNC_SKIP_TEST | ASYNC_AUTO_IRQ;
	tmp.xmit_fifo_size	= tty0tty->serial.xmit_fifo_size;
	tmp.baud_base		= tty0tty->serial.baud_base;
	tmp.close_delay		= 5*HZ;
	tmp.closing_wait	= 30*HZ;
	tmp.custom_divisor	= tty0tty->serial.custom_divisor;
	tmp.hub6		= tty0tty->serial.hub6;
	tmp.io_type		= tty0tty->serial.io_type;

	if (copy_to_user((void __user *)arg, &tmp, sizeof(struct serial_struct)))
		return -EFAULT;
	return 0;
}

static int tty0tty_ioctl_tiocsserial(struct tty_struct *tty,
			unsigned long arg)
{
/*
	struct tty0tty_serial *tty0tty = tty->driver_data;
	struct serial_struct tmp;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i\n", __FUNCTION__, tty->index);
#endif

	if (!arg)
		return -EFAULT;

	memset(&tmp, 0, sizeof(tmp));

	tmp.type		= tty0tty->serial.type;
	tmp.line		= tty0tty->serial.line;
	tmp.port		= tty0tty->serial.port;
	tmp.irq			= tty0tty->serial.irq;
	tmp.flags		= ASYNC_SKIP_TEST | ASYNC_AUTO_IRQ;
	tmp.xmit_fifo_size	= tty0tty->serial.xmit_fifo_size;
	tmp.baud_base		= tty0tty->serial.baud_base;
	tmp.close_delay		= 5*HZ;
	tmp.closing_wait	= 30*HZ;
	tmp.custom_divisor	= tty0tty->serial.custom_divisor;
	tmp.hub6		= tty0tty->serial.hub6;
	tmp.io_type		= tty0tty->serial.io_type;

	if (copy_to_user((void __user *)arg, &tmp, sizeof(struct serial_struct)))
		return -EFAULT;
	return 0;
*/
	return -EFAULT;//TODO
}

static int tty0tty_ioctl_tiocmiwait(struct tty_struct *tty,
			unsigned long arg)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;
	DECLARE_WAITQUEUE(wait, current);
	struct async_icount cnow;
	struct async_icount cprev;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i\n", __FUNCTION__, tty->index);
#endif
	cprev = tty0tty->icount;
	while (1) {
		add_wait_queue(&tty0tty->wait, &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
		remove_wait_queue(&tty0tty->wait, &wait);

		/* see if a signal woke us up */
		if (signal_pending(current))
			return -ERESTARTSYS;

		cnow = tty0tty->icount;
		if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr &&
		    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts)
			return -EIO; /* no change => error */
		if (((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) ||
		    ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) ||
		    ((arg & TIOCM_CD)  && (cnow.dcd != cprev.dcd)) ||
		    ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts)) ) {
			return 0;
		}
		cprev = cnow;
	}
}

static int tty0tty_ioctl_tiocgicount(struct tty_struct *tty,
			unsigned long arg)
{
	struct tty0tty_serial *tty0tty = tty->driver_data;
	struct async_icount cnow = tty0tty->icount;
	struct serial_icounter_struct icount;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i\n", __FUNCTION__, tty->index);
#endif

	icount.cts	= cnow.cts;
	icount.dsr	= cnow.dsr;
	icount.rng	= cnow.rng;
	icount.dcd	= cnow.dcd;
	icount.rx	= cnow.rx;
	icount.tx	= cnow.tx;
	icount.frame	= cnow.frame;
	icount.overrun	= cnow.overrun;
	icount.parity	= cnow.parity;
	icount.brk	= cnow.brk;
	icount.buf_overrun = cnow.buf_overrun;

	if (copy_to_user((void __user *)arg, &icount, sizeof(icount)))
		return -EFAULT;
	return 0;
}

static int tty0tty_ioctl_tcgets(struct tty_struct *tty,
			unsigned long arg, unsigned int opt)
{
	struct ktermios kterm;
#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i\n", __FUNCTION__, tty->index);
#endif
	down_read(&tty->termios_rwsem);
	kterm = tty->termios;
	up_read(&tty->termios_rwsem);

	if(opt){
		if (kernel_termios_to_user_termios((struct termios2 __user *)arg, &kterm))
			return  -EFAULT;
	}
	else{
		if (kernel_termios_to_user_termios_1((struct termios __user *)arg, &kterm))
			return  -EFAULT;
	}
	return 0;


}

static int tty0tty_ioctl_tcsets(struct tty_struct *tty,
			unsigned long arg, unsigned int opt)
{
	struct ktermios tmp_termios;
	int retval = tty_check_change(tty);

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i\n", __FUNCTION__, tty->index);
#endif


	if (retval)
		return retval;

	down_read(&tty->termios_rwsem);
	tmp_termios = tty->termios;
	up_read(&tty->termios_rwsem);

	if(opt){
		if (user_termios_to_kernel_termios(&tmp_termios,
						(struct termios2 __user *)arg))
			return -EFAULT;
	}
	else{
		if (user_termios_to_kernel_termios_1(&tmp_termios,
						(struct termios __user *)arg))
			return -EFAULT;
	}

	tmp_termios.c_ispeed = tty_termios_input_baud_rate(&tmp_termios);
	tmp_termios.c_ospeed = tty_termios_baud_rate(&tmp_termios);

	tty_set_termios(tty, &tmp_termios);

	return 0;
}

static int tty0tty_ioctl_tcflsh(struct tty_struct *tty,
			unsigned long arg)
{
	struct tty_ldisc *ld = tty->ldisc;
	int retval = tty_check_change(tty);

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i 0x%08lX\n", __FUNCTION__,tty->index, arg);
#endif

	if (retval)
		return retval;

	switch (arg) {
	case TCIFLUSH:
		if (ld && ld->ops->flush_buffer) {
			ld->ops->flush_buffer(tty);
			tty_unthrottle(tty);
		}
		break;
	case TCIOFLUSH:
		if (ld && ld->ops->flush_buffer) {
			ld->ops->flush_buffer(tty);
			tty_unthrottle(tty);
	 	        tty_driver_flush_buffer(tty);
		}
#if defined(__has_attribute)
#if __has_attribute(__fallthrough__)
		__attribute__((__fallthrough__));
#endif
#endif
	case TCOFLUSH:
		tty_driver_flush_buffer(tty);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int tty0tty_ioctl(struct tty_struct *tty,
			unsigned int cmd, unsigned long arg)
{
#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - tnt%i  cmd=0x%04X\n", __FUNCTION__, tty->index, cmd);
#endif
	switch (cmd) {
	case TIOCGSERIAL:
		return tty0tty_ioctl_tiocgserial(tty, arg);
	case TIOCSSERIAL:
		return tty0tty_ioctl_tiocsserial(tty, arg);
	case TIOCMIWAIT:
		return tty0tty_ioctl_tiocmiwait(tty, arg);
	case TIOCGICOUNT:
		return tty0tty_ioctl_tiocgicount(tty, arg);
	case TCGETS:
		return tty0tty_ioctl_tcgets(tty, arg, 0);
	case TCSETS:
		return tty0tty_ioctl_tcsets(tty, arg ,0);
	case TCFLSH:
		return tty0tty_ioctl_tcflsh(tty, arg);
	case TCGETS2:
		return tty0tty_ioctl_tcgets(tty, arg, 1);
	case TCSETS2:
		return tty0tty_ioctl_tcsets(tty, arg ,1);
#ifdef SCULL_DEBUG
	default:
		printk(KERN_DEBUG "ioctl 0x%04X Not Implemented!\n",cmd);
		break;
#endif
	}

	return -ENOIOCTLCMD;
}


static int tty0tty_break_ctl(struct tty_struct *tty, int state){

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - %i \n", __FUNCTION__, state);
#endif

	return 0;
}


static struct tty_operations serial_ops = {
	.open = tty0tty_open,
	.close = tty0tty_close,
	.write = tty0tty_write,
	.write_room = tty0tty_write_room,
	.set_termios = tty0tty_set_termios,
	.tiocmget = tty0tty_tiocmget,
	.tiocmset = tty0tty_tiocmset,
	.ioctl = tty0tty_ioctl,
	.break_ctl = tty0tty_break_ctl,
};


static struct tty_driver *tty0tty_tty_driver;

static int __init tty0tty_init(void)
{
	int retval;
	int i;
	struct tty0tty_serial *tty0tty;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - \n", __FUNCTION__);
#endif
	/* allocate the tty driver */
	tty0tty_tty_driver = tty_alloc_driver(TTY0TTY_MINORS, 0);
	if (IS_ERR(tty0tty_tty_driver) || !tty0tty_tty_driver)
		return -ENOMEM;

	/* initialize the tty driver */
	tty0tty_tty_driver->owner = THIS_MODULE;
	tty0tty_tty_driver->driver_name = "tty0tty";
	tty0tty_tty_driver->name = "tnt";
	/* no more devfs subsystem */
	tty0tty_tty_driver->major = TTY0TTY_MAJOR;
	tty0tty_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	tty0tty_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	tty0tty_tty_driver->flags = TTY_DRIVER_DYNAMIC_DEV | TTY_DRIVER_REAL_RAW ;
	/* no more devfs subsystem */
	tty0tty_tty_driver->init_termios = tty_std_termios;
	tty0tty_tty_driver->init_termios.c_iflag = 0;
	tty0tty_tty_driver->init_termios.c_oflag = 0;
	tty0tty_tty_driver->init_termios.c_cflag = B38400 | CS8 | CREAD;
	tty0tty_tty_driver->init_termios.c_lflag = 0;
	tty0tty_tty_driver->init_termios.c_ispeed = 38400;
	tty0tty_tty_driver->init_termios.c_ospeed = 38400;

	tty_set_operations(tty0tty_tty_driver, &serial_ops);

	for(i=0;i<TTY0TTY_MINORS;i++)
	{
		tty_port_init(&tport[i]);
		tty_port_link_device(&tport[i],tty0tty_tty_driver, i);
	}

	retval = tty_register_driver(tty0tty_tty_driver);
	if (retval) {
		printk(KERN_ERR "failed to register tty0tty tty driver");
		tty_driver_kref_put(tty0tty_tty_driver);
		return retval;
	}

	for (i = 0; i < TTY0TTY_MINORS; i++) {
        /* first time accessing this device, let's create it */
        tty0tty = kmalloc(sizeof(*tty0tty), GFP_KERNEL);
        if (!tty0tty)
           return -ENOMEM;
        tty0tty_table[i] = tty0tty;
        sema_init(&tty0tty->sem, 1);
        tty0tty_table[i]->open_count = 0;

        tty0tty_dev[i] = tty_register_device_attr(tty0tty_tty_driver, i, NULL, tty0tty, tty0tty_dev_groups);
        if (IS_ERR(tty0tty_dev[i])) {
            tty_unregister_device(tty0tty_tty_driver, i);
            return PTR_ERR(tty0tty_dev[i]);
        }
    }

	printk(KERN_INFO DRIVER_DESC " " DRIVER_VERSION "\n");
	return retval;
}

static void __exit tty0tty_exit(void)
{
	struct tty0tty_serial *tty0tty;
	int i;

#ifdef SCULL_DEBUG
	printk(KERN_DEBUG "%s - \n", __FUNCTION__);
#endif
	for (i = 0; i < TTY0TTY_MINORS; ++i)
	{
		tty_port_destroy(&tport[i]);
		tty_unregister_device(tty0tty_tty_driver, i);
	}
	tty_unregister_driver(tty0tty_tty_driver);

	/* shut down all of the timers and free the memory */
	for (i = 0; i < TTY0TTY_MINORS; ++i) {
		tty0tty = tty0tty_table[i];
		if (tty0tty) {
			/* close the port */
			while (tty0tty->open_count)
				do_close(tty0tty);

			/* shut down our timer and free the memory */
			kfree(tty0tty);
			tty0tty_table[i] = NULL;
		}
	}
}

module_init(tty0tty_init);
module_exit(tty0tty_exit);
