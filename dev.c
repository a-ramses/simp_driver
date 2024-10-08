#ifndef __KERNEL__
#  define __KERNEL__
#endif
#ifndef MODULE
#  define MODULE
#endif

#undef PDEBUG
#ifdef DM510_DEBUG
#  ifdef __KERNEL__
#    define PDEBUG(fmt, args...) printk(KERN_DEBUG "SIMP: " fmt, ## args)
#  else
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...)
#endif

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h> 
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/cdev.h>
/* #include <asm/uaccess.h> */
#include <linux/uaccess.h>
#include <linux/semaphore.h>
/* #include <asm/system.h> */
#include <asm/switch_to.h>
/* Prototypes - this would normally go in a .h file */
static int simp_open( struct inode*, struct file* );
static int simp_release( struct inode*, struct file* );
static ssize_t simp_read( struct file*, char*, size_t, loff_t* );
static ssize_t simp_write( struct file*, const char*, size_t, loff_t* );
long simp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

#define DEVICE_NAME "simp_dev" /* Dev name as it appears in /proc/devices */
#define MAJOR_NUMBER 254
#define MIN_MINOR_NUMBER 0
#define MAX_MINOR_NUMBER 1
#define DEVICE_COUNT 2
#define DM510_IOC_MAGIC 'k'
#define DM510_P_IOCTSIZE _IO(DM510_IOC_MAGIC, 1)
#define DM510_P_IOCTPOS _IO(DM510_IOC_MAGIC, 2)

/* end of what really should have been in a .h file */
struct pipe {
    wait_queue_head_t inq, outq;
    char *buffer, *end;
    int bufferSize;
    char *rp, *wp;
    int nReaders, nWriters;
    struct mutex mutex;
    struct cdev cdev;
    struct pipe *otherDev;
};
/* Parameters */
static struct simp_pipe *simp_p_devices;
dev_t simp_p_devno[2];
int maxReaders = INT_MAX, maxWriters = 1;
int simp_p_buffer = 4000;

/* file operations struct */
static struct file_operations simp_fops = {
    .owner          = THIS_MODULE,
    .read           = simp_read,
    .write          = simp_write,
    .open           = simp_open,
    .release        = simp_release,
    .unlocked_ioctl = simp_ioctl
};

static void simp_p_setup_cdev(struct simp_pipe *dev, int index) {
    int err, devno = simp_p_devno[index];

    // Initializing char devs
    cdev_init(&dev->cdev, &simp_fops);
    dev->cdev.owner = THIS_MODULE;

    // Adding char devs
    err = cdev_add(&dev->cdev, devno, 1);
    if(err)
        printk(KERN_NOTICE "Error %d adding SIMP pipe%d", err, index);
}

static int spacefree(struct simp_pipe *dev) {
    if(dev->rp == dev->wp) {
        return dev->bufferSize; 
    } else {
        return ((dev->rp + dev->bufferSize - dev->wp) % dev->bufferSize) - 1;
    }

    return 0;
}

static int simp_getwritespace(struct simp_pipe *dev, struct file *filp) {
    while(spacefree(dev) == 0) {
        DEFINE_WAIT(wait);

        mutex_unlock(&dev->mutex);
        if(filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        PDEBUG("\"%s\" writing: going to sleep\n", current->comm);
        prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
        if(spacefree(dev) == 0)
            schedule();
        finish_wait(&dev->outq, &wait);
        if(signal_pending(current))
            return -ERESTARTSYS;
        if(mutex_lock_interruptible(&dev->mutex))
            return -ERESTARTSYS;
    }
    return 0;
}

/* called when module is loaded */
int simp_init_module( void ) {
    int i, result;
    simp_p_devno[0] = MKDEV(MAJOR_NUMBER, MIN_MINOR_NUMBER);
    simp_p_devno[1] = MKDEV(MAJOR_NUMBER, MAX_MINOR_NUMBER);
    result = register_chrdev_region(MAJOR_NUMBER, DEVICE_COUNT, DEVICE_NAME);
    if(result < 0) {
        printk(KERN_ALERT "DM510: Failed to load module\n");
        return result;
    } else {
        printk(KERN_INFO "DM510: Hello from your device!\n");
    }

    simp_p_devices = kmalloc(DEVICE_COUNT * sizeof(struct simp_pipe), GFP_KERNEL);
    if(simp_p_devices == NULL) {
        unregister_chrdev_region(MAJOR_NUMBER, DEVICE_COUNT);
        return -ENOMEM;
    }

    // Initializing simpies
    memset(simp_p_devices, 0, DEVICE_COUNT * sizeof(struct simp_pipe));
    for(i = 0; i < DEVICE_COUNT; i++) {
        init_waitqueue_head(&(simp_p_devices[i].inq));
        init_waitqueue_head(&(simp_p_devices[i].outq));
        mutex_init(&simp_p_devices[i].mutex);
        simp_p_setup_cdev(simp_p_devices + i, i);
        simp_p_devices[i].otherDev = &(simp_p_devices[i ^ 1]);
    }

    return 0;
}

/* Called when module is unloaded */
void simp_cleanup_module( void ) {
    int i;

    if(!simp_p_devices)
        return;

    for(i = 0; i < DEVICE_COUNT; i++) {
        cdev_del(&simp_p_devices[i].cdev);
        kfree(simp_p_devices[i].buffer);
    }
    kfree(simp_p_devices);
    unregister_chrdev_region(MAJOR_NUMBER, DEVICE_COUNT);
    simp_p_devices = NULL;

    printk(KERN_INFO "DM510: Module unloaded.\n");
}

/* Called when a process tries to open the device file */
static int simp_open( struct inode *inode, struct file *filp ) {
    // Getting device
    struct simp_pipe *dev;
    dev = container_of(inode->i_cdev, struct simp_pipe, cdev);

    // Saving device to file
    filp->private_data = dev;

    // Locking mutex
    if(mutex_lock_interruptible(&dev->mutex)) {
        return -ERESTARTSYS;
    }

    // Making sure there is only one writer at a time,
    // and unlimited, unless changed, readers.
    if(filp->f_mode & FMODE_READ){
    	if(dev->nReaders < maxReaders)
            dev->nReaders++;
        else 
            return -EBUSY;
    }
    if(filp->f_mode & FMODE_WRITE){
        if(dev->nWriters < maxWriters)
	    	dev->nWriters++;
        else
            return -EBUSY;
    }

    // Allocating buffer if not present
    if(!dev->buffer) {
        dev->buffer = kmalloc(simp_p_buffer, GFP_KERNEL);
        if(!dev->buffer) {
            mutex_unlock(&dev->mutex);
            return -ENOMEM;
    	}
        dev->bufferSize = simp_p_buffer;
        dev->end = dev->buffer + dev->bufferSize;
        dev->rp = dev->buffer;
        dev->wp = dev->buffer;
    }

    // Unlocking mutex
    mutex_unlock(&dev->mutex);
    return nonseekable_open(inode, filp);
}

/* Called when a process closes the device file. */
static int simp_release( struct inode *inode, struct file *filp ) {

    /* device release code belongs here */
    struct simp_pipe *dev = filp->private_data;

    // Locking mutex
    mutex_lock(&dev->mutex);

    // Decrementing nReaders or nWriters
    if(filp->f_mode & FMODE_READ){
        dev->nReaders--;
    }
    if(filp->f_mode & FMODE_WRITE){
        dev->nWriters--;
    }

    // Freeing buffer if there are no readers or writers
    if(dev->nReaders + dev->nWriters == 0) {
        kfree(dev->buffer);
        dev->buffer = NULL;
    }

    // Unlocking mutex
    mutex_unlock(&dev->mutex);
    return 0;
}

/* Called when a process, which already opened the dev file, attempts to read from it. */
static ssize_t simp_read( struct file *filp, char *buf, size_t count, loff_t *f_pos)
{
    struct simp_pipe *dev = filp->private_data;
    //printk(KERN_INFO "--READ START\n");
    if(mutex_lock_interruptible(&dev->mutex))
        return -ERESTARTSYS;

    // Sleep if no new data
    while(dev->rp == dev->wp) {
        mutex_unlock(&dev->mutex);
        if(filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        PDEBUG("\"%s\" reading: going to sleep\n", current->comm);
        if(wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))
            return -ERESTARTSYS;
        if(mutex_lock_interruptible(&dev->mutex))
            return -ERESTARTSYS;
    }

    // write pointer ahead of read pointer
    if(dev->wp > dev->rp)
        count = min(count, (size_t)(dev->wp - dev->rp));
    else // wp behind rp
        count = min(count, (size_t)(dev->end - dev->rp));

    // Copying count bytes from rp to buf
    if(copy_to_user(buf, dev->rp, count)) {
        mutex_unlock(&dev->mutex);
        return -EFAULT;
    }

    // Move read pointer
    dev->rp += count;
    // Move rp to start if it is at the end
    if(dev->rp == dev->end)
        dev->rp = dev->buffer;

    // Unlock mutex
    mutex_unlock(&dev->mutex);

    // Wakey Wakey
    wake_up_interruptible(&dev->outq);
    wake_up_interruptible(&dev->inq);

    PDEBUG("\"%s\" did read %li bytes\n", current->comm, (long)count);
    //printk(KERN_INFO "--READ END\n");
    return count;
}

/* Called when a process writes to dev file */
static ssize_t simp_write( struct file *filp, const char *buf, size_t count, loff_t *f_pos)
{
    /* write code belongs here */
    struct simp_pipe *dev = filp->private_data;
    struct simp_pipe *otherDev = dev->otherDev;
    int result;
    //printk(KERN_INFO "--WRITE START\n");
    if(mutex_lock_interruptible(&otherDev->mutex))
        return -ERESTARTSYS;

    result = simp_getwritespace(otherDev, filp);
    if(result)
        return result;

    count = min(count, (size_t)spacefree(otherDev));
    if(otherDev->wp >= otherDev->rp)
        count = min(count, (size_t)(otherDev->end - otherDev->wp));
    else
        count = min(count, (size_t)(otherDev->rp - otherDev->wp - 1));
    PDEBUG("Going to accept %li bytes to %p from %p\n", (long)count, otherDev->wp, buf);
    if(copy_from_user(otherDev->wp, buf, count)) {
        mutex_unlock(&otherDev->mutex);
        return -EFAULT;
    }
    otherDev->wp += count;
    if(otherDev->wp == otherDev->end)
        otherDev->wp = otherDev->buffer;
    mutex_unlock(&otherDev->mutex);

    wake_up_interruptible(&otherDev->inq);
    wake_up_interruptible(&otherDev->outq);
    return count; //return number of bytes written
}

/* called by system call icotl */
long simp_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    printk(KERN_INFO "DM510: ioctl called.\n");

	switch(cmd) {
		// Let's the user change the buffer sizes
		case SIMP_P_IOCTSIZE:
			simp_p_buffer = arg;
			break;

		// Let's the user change the amount of processes that are allowed to read simultaneously.
		case SIMP_P_IOCTPOS:
			maxReaders = arg;
            break;
	}

    return 0;
}

module_init( simp_init_module );
module_exit( simp_cleanup_module );

MODULE_AUTHOR( "SIMP" );
MODULE_LICENSE( "GPL" );
