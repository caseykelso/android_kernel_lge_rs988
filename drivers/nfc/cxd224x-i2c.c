/*
 *  cxd224x-i2c.c - cxd224x NFC i2c driver
 *
 * Copyright (C) 2013 Sony Corporation.
 * Copyright (C) 2012 Broadcom Corporation.
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>
#include <linux/poll.h>
#include <linux/version.h>

#include <linux/async.h>

/* for device tree */
#include <linux/of_gpio.h>

#include <soc/qcom/lge/board_lge.h>
#include <soc/qcom/lge/power/lge_board_revision.h>

#include <linux/nfc/cxd224x.h>
#include <linux/wakelock.h>
#define CXD224X_WAKE_LOCK_TIMEOUT   10      /* wake lock timeout for HOSTINT (sec) */
#define CXD224X_WAKE_LOCK_NAME  "cxd224x-i2c"       /* wake lock for HOSTINT */
#define CXD224X_WAKE_LOCK_TIMEOUT_LP    3       /* wake lock timeout for low-power-mode (sec) */
#define CXD224X_WAKE_LOCK_NAME_LP "cxd224x-i2c-lp"  /* wake lock for low-power-mode */

/* do not change below */
#define MAX_BUFFER_SIZE     780

/* Read data */
#define PACKET_HEADER_SIZE_NCI  (3)
#define PACKET_HEADER_SIZE_HCI  (3)
#define PACKET_TYPE_NCI     (16)
#define PACKET_TYPE_HCIEV   (4)
#define MAX_PACKET_SIZE     (PACKET_HEADER_SIZE_NCI + 255)

/* RESET */
#define RESET_ASSERT_MS         (1)

struct cxd224x_dev {
    wait_queue_head_t read_wq;
    struct mutex read_mutex;
    struct i2c_client *client;
    struct miscdevice cxd224x_device;
    unsigned int en_gpio;
    unsigned int irq_gpio;
    unsigned int wake_gpio;
    unsigned int hvdd_gpio;
    unsigned int rst_gpio;
    bool irq_enabled;
    struct mutex lock;
    spinlock_t irq_enabled_lock;
    unsigned int users;
    unsigned int count_irq;
    struct wake_lock wakelock;  /* wake lock for HOSTINT */
    struct wake_lock wakelock_lp;   /* wake lock for low-power-mode */
    /* Driver message queue */
    struct workqueue_struct *wqueue;
    struct work_struct qmsg;
};

#ifdef CONFIG_CXD224X_NFC_RST
static void cxd224x_workqueue(struct work_struct *work)
{
    struct cxd224x_dev *cxd224x_dev = container_of(work, struct cxd224x_dev, qmsg);
    unsigned long flags;

    dev_info(&cxd224x_dev->client->dev, "%s, xrst assert\n", __func__);
        spin_lock_irqsave(&cxd224x_dev->irq_enabled_lock, flags);
    gpio_set_value(cxd224x_dev->rst_gpio, CXDNFC_RST_ACTIVE);
        cxd224x_dev->count_irq=0; /* clear irq */
        spin_unlock_irqrestore(&cxd224x_dev->irq_enabled_lock, flags);

    msleep(RESET_ASSERT_MS);
    dev_info(&cxd224x_dev->client->dev, "%s, xrst deassert\n", __func__);
    gpio_set_value(cxd224x_dev->rst_gpio, ~CXDNFC_RST_ACTIVE & 0x1);
}

static int __init init_wqueue(struct cxd224x_dev *cxd224x_dev)
{
    INIT_WORK(&cxd224x_dev->qmsg, cxd224x_workqueue);
    cxd224x_dev->wqueue = create_workqueue("cxd224x-i2c_wrokq");
    if (cxd224x_dev->wqueue == NULL)
        return -EBUSY;
    return 0;
}
#endif /* CONFIG_CXD224X_NFC_RST */

static void cxd224x_init_stat(struct cxd224x_dev *cxd224x_dev)
{
    cxd224x_dev->count_irq = 0;
}

static void cxd224x_disable_irq(struct cxd224x_dev *cxd224x_dev)
{
    unsigned long flags;
    spin_lock_irqsave(&cxd224x_dev->irq_enabled_lock, flags);
    if (cxd224x_dev->irq_enabled) {
        disable_irq_nosync(cxd224x_dev->client->irq);
        cxd224x_dev->irq_enabled = false;
    }
    spin_unlock_irqrestore(&cxd224x_dev->irq_enabled_lock, flags);
}

static void cxd224x_enable_irq(struct cxd224x_dev *cxd224x_dev)
{
    unsigned long flags;
    spin_lock_irqsave(&cxd224x_dev->irq_enabled_lock, flags);
    if (!cxd224x_dev->irq_enabled) {
        cxd224x_dev->irq_enabled = true;
        enable_irq(cxd224x_dev->client->irq);
    }
    spin_unlock_irqrestore(&cxd224x_dev->irq_enabled_lock, flags);
}

static irqreturn_t cxd224x_dev_irq_handler(int irq, void *dev_id)
{
    struct cxd224x_dev *cxd224x_dev = dev_id;
    unsigned long flags;

    spin_lock_irqsave(&cxd224x_dev->irq_enabled_lock, flags);
    cxd224x_dev->count_irq++;
    spin_unlock_irqrestore(&cxd224x_dev->irq_enabled_lock, flags);
    wake_up(&cxd224x_dev->read_wq);

    return IRQ_HANDLED;
}

static unsigned int cxd224x_dev_poll(struct file *filp, poll_table *wait)
{
    struct cxd224x_dev *cxd224x_dev = filp->private_data;
    unsigned int mask = 0;
    unsigned long flags;

    poll_wait(filp, &cxd224x_dev->read_wq, wait);

    spin_lock_irqsave(&cxd224x_dev->irq_enabled_lock, flags);
    if (cxd224x_dev->count_irq > 0)
    {
        cxd224x_dev->count_irq--;
        mask |= POLLIN | POLLRDNORM;
    }
    spin_unlock_irqrestore(&cxd224x_dev->irq_enabled_lock, flags);
    if(mask)
        wake_lock_timeout(&cxd224x_dev->wakelock, CXD224X_WAKE_LOCK_TIMEOUT*HZ);

    return mask;
}

static ssize_t cxd224x_dev_read(struct file *filp, char __user *buf,
                  size_t count, loff_t *offset)
{
    struct cxd224x_dev *cxd224x_dev = filp->private_data;
    unsigned char tmp[MAX_BUFFER_SIZE];
    int total, len, ret;

    total = 0;
    len = 0;

    if (count > MAX_BUFFER_SIZE)
        count = MAX_BUFFER_SIZE;

    mutex_lock(&cxd224x_dev->read_mutex);

    ret = i2c_master_recv(cxd224x_dev->client, tmp, 3);
    if (ret == 3 && (tmp[0] != 0xff)) {
        total = ret;

        len = tmp[PACKET_HEADER_SIZE_NCI-1];

        /** make sure full packet fits in the buffer
        **/
        if (len > 0 && (len + total) <= count) {
            /** read the remainder of the packet.
            **/
            ret = i2c_master_recv(cxd224x_dev->client, tmp+total, len);
            if (ret == len)
                total += len;
        }
    }

    mutex_unlock(&cxd224x_dev->read_mutex);

    if (total > count || copy_to_user(buf, tmp, total)) {
        dev_err(&cxd224x_dev->client->dev,
            "failed to copy to user space, total = %d\n", total);
        total = -EFAULT;
    }

    return total;
}

static ssize_t cxd224x_dev_write(struct file *filp, const char __user *buf,
                   size_t count, loff_t *offset)
{
    struct cxd224x_dev *cxd224x_dev = filp->private_data;
    char tmp[MAX_BUFFER_SIZE];
    int ret;

    if (count > MAX_BUFFER_SIZE) {
        dev_err(&cxd224x_dev->client->dev, "out of memory\n");
        return -ENOMEM;
    }

    if (copy_from_user(tmp, buf, count)) {
        dev_err(&cxd224x_dev->client->dev,
            "failed to copy from user space\n");
        return -EFAULT;
    }

    mutex_lock(&cxd224x_dev->read_mutex);
    /* Write data */

    ret = i2c_master_send(cxd224x_dev->client, tmp, count);
    if (ret != count) {
        dev_err(&cxd224x_dev->client->dev,
            "failed to write %d\n", ret);
        ret = -EIO;
    }
    mutex_unlock(&cxd224x_dev->read_mutex);

    return ret;
}

static int cxd224x_dev_open(struct inode *inode, struct file *filp)
{
    int ret = 0;
    int call_enable = 0;
    struct cxd224x_dev *cxd224x_dev = container_of(filp->private_data,
                               struct cxd224x_dev,
                               cxd224x_device);
    filp->private_data = cxd224x_dev;
    mutex_lock(&cxd224x_dev->lock);
    if (!cxd224x_dev->users)
    {
    cxd224x_init_stat(cxd224x_dev);
        call_enable = 1;
    }
    cxd224x_dev->users++;
    mutex_unlock(&cxd224x_dev->lock);
    if (call_enable)
    cxd224x_enable_irq(cxd224x_dev);

    dev_info(&cxd224x_dev->client->dev,
         "open %d,%d users=%d\n", imajor(inode), iminor(inode), cxd224x_dev->users);

    return ret;
}

static int cxd224x_dev_release(struct inode *inode, struct file *filp)
{
    int ret = 0;
    int call_disable = 0;
    struct cxd224x_dev *cxd224x_dev = filp->private_data;

    mutex_lock(&cxd224x_dev->lock);
    cxd224x_dev->users--;
    if (!cxd224x_dev->users)
    {
        call_disable = 1;
    }
    mutex_unlock(&cxd224x_dev->lock);
    if (call_disable)
        cxd224x_disable_irq(cxd224x_dev);

    dev_info(&cxd224x_dev->client->dev,
         "release %d,%d users=%d\n", imajor(inode), iminor(inode), cxd224x_dev->users);

    return ret;
}

static long cxd224x_dev_unlocked_ioctl(struct file *filp,
                     unsigned int cmd, unsigned long arg)
{
    struct cxd224x_dev *cxd224x_dev = filp->private_data;

    switch (cmd) {
    case CXDNFC_RST_CTL:
#ifdef CONFIG_CXD224X_NFC_RST
        if(lge_get_boot_mode() <= LGE_BOOT_MODE_CHARGERLOGO ) {
        dev_info(&cxd224x_dev->client->dev, "%s, rst arg=%d\n", __func__, (int)arg);
        dev_info(&cxd224x_dev->client->dev, "%s, bootmode = %d\n", __func__, lge_get_boot_mode());
        return (queue_work(cxd224x_dev->wqueue, &cxd224x_dev->qmsg) ? 0 : 1);
        }
#endif
        break;
    case CXDNFC_POWER_CTL:
#ifdef CONFIG_CXD224X_NFC_VEN
        if (arg == 0) {
            gpio_set_value(cxd224x_dev->en_gpio, 1);
        } else if (arg == 1) {
            gpio_set_value(cxd224x_dev->en_gpio, 0);
        } else {
            /* do nothing */
        }
#endif
        break;
    case CXDNFC_WAKE_CTL:
        if (arg == 0) {
            wake_lock_timeout(&cxd224x_dev->wakelock_lp, CXD224X_WAKE_LOCK_TIMEOUT_LP*HZ);
            /* PON HIGH (normal power mode)*/
            gpio_set_value(cxd224x_dev->wake_gpio, 1);
        } else if (arg == 1) {
            /* PON LOW (low power mode) */
            if(lge_get_board_rev_no() >= HW_REV_A) {
                gpio_set_value(cxd224x_dev->wake_gpio, 0);
                wake_unlock(&cxd224x_dev->wakelock_lp);
            } else {
                gpio_set_value(cxd224x_dev->wake_gpio, 1);
                wake_unlock(&cxd224x_dev->wakelock_lp);
            }
        } else {
            /* do nothing */
        }
        break;
    case CXDNFC_BOOTMODE_CTL:
        return lge_get_boot_mode();
    default:
        dev_err(&cxd224x_dev->client->dev,
            "%s, unknown cmd (%x, %lx)\n", __func__, cmd, arg);
        return 0;
    }

    return 0;
}

static const struct file_operations cxd224x_dev_fops = {
    .owner = THIS_MODULE,
    .llseek = no_llseek,
    .poll = cxd224x_dev_poll,
    .read = cxd224x_dev_read,
    .write = cxd224x_dev_write,
    .open = cxd224x_dev_open,
    .release = cxd224x_dev_release,
    .unlocked_ioctl = cxd224x_dev_unlocked_ioctl,
    .compat_ioctl = cxd224x_dev_unlocked_ioctl,
};

static int cxd224x_probe(struct i2c_client *client,
               const struct i2c_device_id *id)
{
    int ret;
    struct cxd224x_dev *cxd224x_dev;
       /* for device_tree */
    struct device_node *np = client->dev.of_node;

    int irq_gpio_ok  = 0;
#ifdef CONFIG_CXD224X_NFC_VEN
    int en_gpio_ok   = 0;
#endif
#ifdef CONFIG_CXD224X_NFC_RST
    int rst_gpio_ok = 0;
#endif
    int wake_gpio_ok = 0;

    int hvdd_gpio_value = 0;
    cxd224x_dev = kzalloc(sizeof(*cxd224x_dev), GFP_KERNEL);
    if (cxd224x_dev == NULL) {
        dev_err(&client->dev,
            "failed to allocate memory for module data\n");
        ret = -ENOMEM;
        goto err_exit;
    }

     cxd224x_dev->irq_gpio = of_get_named_gpio_flags(np, "sony,gpio_irq", 0, NULL);
     cxd224x_dev->hvdd_gpio = of_get_named_gpio_flags(np, "sony,gpio_hvdd", 0, NULL);
     cxd224x_dev->wake_gpio = of_get_named_gpio_flags(np, "sony,gpio_mode", 0, NULL);
     cxd224x_dev->rst_gpio = of_get_named_gpio_flags(np, "sony,gpio_xrst", 0, NULL);
     cxd224x_dev->client = client;
/*
    ret = gpio_request_one(cxd224x_dev->rst_gpio, GPIOF_OUT_INIT_LOW, "nfc_rst");
    if (ret)
        return -ENODEV;
//    irq_gpio_ok=1;
    gpio_set_value(cxd224x_dev->rst_gpio, 1);
*/

    if(lge_get_boot_mode() != LGE_BOOT_MODE_CHARGERLOGO ) {
        hvdd_gpio_value = GPIOF_OUT_INIT_HIGH;
    } else {
        hvdd_gpio_value = GPIOF_OUT_INIT_LOW;
    }

    ret = gpio_request_one(cxd224x_dev->hvdd_gpio, hvdd_gpio_value, "nfc_hvdd");
    if (ret)
        return -ENODEV;
//    irq_gpio_ok=1;
    gpio_set_value(cxd224x_dev->hvdd_gpio, hvdd_gpio_value);

    ret = gpio_request_one(cxd224x_dev->irq_gpio, GPIOF_IN, "nfc_int");
    if (ret)
        return -ENODEV;
    irq_gpio_ok=1;

#ifdef CONFIG_CXD224X_NFC_VEN
    ret = gpio_request_one(cxd224x_dev->en_gpio, GPIOF_OUT_INIT_LOW, "nfc_cen");
    if (ret)
        goto err_exit;
    en_gpio_ok=1;
    gpio_set_value(cxd224x_dev->en_gpio, 0);
#endif

#ifdef CONFIG_CXD224X_NFC_RST
    ret = gpio_request_one(cxd224x_dev->rst_gpio, GPIOF_OUT_INIT_LOW, "nfc_rst");
    if (ret)
        goto err_exit;
    rst_gpio_ok=1;
    dev_info(&client->dev, "%s, xrst deassert\n", __func__);
    gpio_set_value(cxd224x_dev->rst_gpio, 0);
#endif

    ret = gpio_request_one(cxd224x_dev->wake_gpio, GPIOF_OUT_INIT_LOW, "nfc_wake");
    if (ret)
        goto err_exit;
    wake_gpio_ok=1;
    gpio_set_value(cxd224x_dev->wake_gpio, 0);

    dev_info(&client->dev, "wake_gpio = %d\n",  cxd224x_dev->wake_gpio);
    dev_info(&client->dev, "wake_gpio_value = %d\n",  gpio_get_value(cxd224x_dev->wake_gpio));

    dev_info(&client->dev, "hvdd_gpio = %d\n",  cxd224x_dev->hvdd_gpio);
    dev_info(&client->dev, "hvdd_gpio_value = %d\n",  gpio_get_value(cxd224x_dev->hvdd_gpio));

    dev_info(&client->dev, "xrst_gpio_value = %d\n",  gpio_get_value(cxd224x_dev->rst_gpio));

    wake_lock_init(&cxd224x_dev->wakelock, WAKE_LOCK_SUSPEND, CXD224X_WAKE_LOCK_NAME);
    wake_lock_init(&cxd224x_dev->wakelock_lp, WAKE_LOCK_SUSPEND, CXD224X_WAKE_LOCK_NAME_LP);
    cxd224x_dev->users =0;

    /* init mutex and queues */
    init_waitqueue_head(&cxd224x_dev->read_wq);
    mutex_init(&cxd224x_dev->read_mutex);
    mutex_init(&cxd224x_dev->lock);
    spin_lock_init(&cxd224x_dev->irq_enabled_lock);

#ifdef CONFIG_CXD224X_NFC_RST
    if (init_wqueue(cxd224x_dev) != 0) {
        dev_err(&client->dev, "init workqueue failed\n");
        goto err_exit;
    }
#endif

    cxd224x_dev->cxd224x_device.minor = MISC_DYNAMIC_MINOR;
    cxd224x_dev->cxd224x_device.name = "cxd224x-i2c";
    cxd224x_dev->cxd224x_device.fops = &cxd224x_dev_fops;

    ret = misc_register(&cxd224x_dev->cxd224x_device);
    if (ret) {
        dev_err(&client->dev, "misc_register failed\n");
        goto err_misc_register;
    }

    /* request irq.  the irq is set whenever the chip has data available
     * for reading.  it is cleared when all data has been read.
     */
    dev_info(&client->dev, "requesting IRQ %d\n", client->irq);
    cxd224x_dev->irq_enabled = true;
    ret = request_irq(client->irq, cxd224x_dev_irq_handler,
              IRQF_TRIGGER_FALLING, client->name, cxd224x_dev);
    if (ret) {
        dev_err(&client->dev, "request_irq failed\n");
        goto err_request_irq_failed;
    }
    cxd224x_disable_irq(cxd224x_dev);
    i2c_set_clientdata(client, cxd224x_dev);
    dev_info(&client->dev,
         "%s, probing cxd224x driver exited successfully\n",
         __func__);
    return 0;

err_request_irq_failed:
    misc_deregister(&cxd224x_dev->cxd224x_device);
err_misc_register:
    mutex_destroy(&cxd224x_dev->read_mutex);
    kfree(cxd224x_dev);
err_exit:

    return ret;
}

static int cxd224x_remove(struct i2c_client *client)
{
    struct cxd224x_dev *cxd224x_dev;

    cxd224x_dev = i2c_get_clientdata(client);
    wake_lock_destroy(&cxd224x_dev->wakelock);
    wake_lock_destroy(&cxd224x_dev->wakelock_lp);
    free_irq(client->irq, cxd224x_dev);
    misc_deregister(&cxd224x_dev->cxd224x_device);
    mutex_destroy(&cxd224x_dev->read_mutex);
    gpio_free(cxd224x_dev->irq_gpio);
    gpio_free(cxd224x_dev->en_gpio);
    gpio_free(cxd224x_dev->wake_gpio);
    kfree(cxd224x_dev);

    return 0;
}

#ifdef CONFIG_PM
static int cxd224x_suspend(struct device *dev)
{
    struct platform_device *pdev = to_platform_device(dev);
    struct cxd224x_platform_data *platform_data = pdev->dev.platform_data;

    if (device_may_wakeup(&pdev->dev)) {
        int irq = gpio_to_irq(platform_data->irq_gpio);
        enable_irq_wake(irq);
    }
    return 0;
}

static int cxd224x_resume(struct device *dev)
{
    struct platform_device *pdev = to_platform_device(dev);
    struct cxd224x_platform_data *platform_data = pdev->dev.platform_data;

    if (device_may_wakeup(&pdev->dev)) {
        int irq = gpio_to_irq(platform_data->irq_gpio);
        disable_irq_wake(irq);
    }
    return 0;
}

static const struct dev_pm_ops cxd224x_pm_ops = {
    .suspend    = cxd224x_suspend,
    .resume     = cxd224x_resume,
};
#endif

static const struct i2c_device_id cxd224x_id[] = {
    {"cxd224x-i2c", 0},
    {}
};

static struct of_device_id cxd224x_match_table[] = {
    { .compatible = "sony,cxd224x",},
    { },
};

static struct i2c_driver cxd224x_driver = {
    .id_table = cxd224x_id,
    .probe = cxd224x_probe,
    .remove = cxd224x_remove,
    .driver = {
        .owner = THIS_MODULE,
        .name = "cxd224x-i2c",
#ifdef CONFIG_PM
        .pm = &cxd224x_pm_ops,
#endif
        .of_match_table = cxd224x_match_table,
    },
};

/*
 * module load/unload record keeping
 */

static void async_dev_init(void *data, async_cookie_t cookie)
{
    int ret = 0;
    ret = i2c_add_driver(&cxd224x_driver);
    return;
}

static int __init cxd224x_dev_init(void)
{
     async_schedule(async_dev_init, NULL);

     return 0;
//    return i2c_add_driver(&cxd224x_driver);
}
module_init(cxd224x_dev_init);

static void __exit cxd224x_dev_exit(void)
{
    i2c_del_driver(&cxd224x_driver);
}
module_exit(cxd224x_dev_exit);
MODULE_DEVICE_TABLE(i2c, cxd224x_id);

MODULE_AUTHOR("Sony");
MODULE_DESCRIPTION("NFC cxd224x driver");
MODULE_LICENSE("GPL");
