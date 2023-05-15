#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <asm/uaccess.h> 

#include "shub_io.h"
#include "ml630q790.h"

#ifdef CONFIG_ANDROID_ENGINEERING
static int shub_baro_log = 0;
module_param(shub_baro_log, int, 0600);
#define DBG_BARO_IO(msg, ...) {                      \
    if(shub_baro_log & 0x01)                         \
        printk("[shub][baro] " msg, ##__VA_ARGS__);  \
}
#define DBG_BARO_DATA(msg, ...) {                    \
    if(shub_baro_log & 0x02)                         \
        printk("[shub][baro] " msg, ##__VA_ARGS__);  \
}
#else
#define DBG_BARO_IO(msg, ...)
#define DBG_BARO_DATA(msg, ...)
#endif

#define BARO_MAX       BARO_CMN_MAX
#define BARO_MIN       BARO_CMN_MIN
#define INDEX_PRESSURE 0
#define INDEX_TM       1
#define INDEX_TMNS     2
#define INDEX_SUM      3
#define INPUT_DEV_NAME "shub_baro"
#define MISC_DEV_NAME  "shub_io_baro"
//#define SHUB_EVENTS_PER_PACKET ((LOGGING_RAM_SIZE/(DATA_SIZE_BARO+1))*2) /*+1 is ID size*/
#define SHUB_ACTIVE_SENSOR SHUB_ACTIVE_BARO
static DEFINE_MUTEX(shub_lock);

static struct platform_device *pdev;
static struct input_dev *shub_idev;
static int32_t        power_state     = 0;
static int32_t        delay           = 200;//200ms
static IoCtlBatchInfo batch_param     = { 0, 0, 0 };
static struct work_struct sensor_poll_work;

static void shub_sensor_poll_work_func(struct work_struct *work);
static void shub_set_sensor_poll(int32_t en);
static int32_t currentActive;

static struct hrtimer poll_timer;
extern int32_t setMaxBatchReportLatency(uint32_t sensor, int64_t latency);

static long shub_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    void __user *argp = (void __user *)arg;
    int32_t ret = -1, tmpval = 0;
    switch (cmd) {
        case SHUBIO_BARO_ACTIVATE:
            ret = copy_from_user(&tmpval, argp, sizeof(tmpval));
            if (ret) {
                printk("error : shub_ioctl(cmd = SHUBIO_BARO_ACTIVATE)\n");
                return -EFAULT;
            }
            DBG_BARO_IO("ioctl(cmd = Set_Active) : val=%d\n", tmpval);
            mutex_lock(&shub_lock);
            currentActive = shub_get_current_active() & SHUB_ACTIVE_SENSOR;
            if((batch_param.m_Latency != 0) && (currentActive == 0)){
                //polling off and batch enable/disable
                if(tmpval != 0){
                    //batch start/stop
                    if(batch_param.m_Latency > 0){
                        ret = shub_activate_logging(SHUB_ACTIVE_SENSOR, 1);
                    }else{
                        ret = shub_activate_logging(SHUB_ACTIVE_SENSOR, 0);
                    }
                }else{
                    //batch stop
                    ret = shub_activate_logging(SHUB_ACTIVE_SENSOR, 0);
                    setMaxBatchReportLatency(SHUB_ACTIVE_SENSOR, 0);
                }
                //set polling stop 
                shub_set_sensor_poll(0);
            }else{
                //set mcu sensor measure
                ret = shub_activate( SHUB_ACTIVE_SENSOR, tmpval);
                currentActive = shub_get_current_active() & SHUB_ACTIVE_SENSOR;
                //set polling start 
                shub_set_sensor_poll(tmpval);

                //batch stop 
                ret = shub_activate_logging(SHUB_ACTIVE_SENSOR, 0);
                setMaxBatchReportLatency(SHUB_ACTIVE_SENSOR, 0);
            }
            if(ret != -1){
                power_state = tmpval;
            }else{
                mutex_unlock(&shub_lock);
                return -EFAULT;
            }
            mutex_unlock(&shub_lock);
            break;

        case SHUBIO_BARO_SET_FREQ:
            {
                ret = copy_from_user(&tmpval, argp, sizeof(tmpval));
                if (ret) {
                    printk( "error : shub_ioctl(cmd = SHUBIO_BARO_SET_FREQ)\n" );
                    return -EFAULT;
                }
                DBG_BARO_IO("ioctl(cmd = Set_Delay) : delay=%d\n", tmpval);
                mutex_lock(&shub_lock);
                delay = tmpval;
                delay = (delay > SHUB_TIMER_MAX) ? SHUB_TIMER_MAX : delay;
                shub_set_delay(SHUB_ACTIVE_SENSOR, delay);
                if(currentActive != 0){
                    shub_set_sensor_poll(1);
                }
                mutex_unlock(&shub_lock);
            }
            break;

        case SHUBIO_BARO_SET_BATCH :
            {
                IoCtlBatchInfo param;
                uint64_t delayNs;
                ret = copy_from_user(&param, argp, sizeof(param));
                if (ret) {
                    printk( "error(copy_from_user) : shub_ioctl(cmd = SHUBIO_BARO_SET_BATCH)\n" );
                    return -EFAULT;
                }
                DBG_BARO_IO("ioctl(cmd = Set_Batch) : flg=%d, Period=%lld, Latency=%lld\n", param.m_Flasg, param.m_PeriodNs, param.m_Latency);
                if((param.m_Flasg & 0x01) == 0x01){
                    return 0;
                }
                mutex_lock(&shub_lock);
                delayNs = param.m_PeriodNs;
                delay = (int32_t)do_div(delayNs, 1000000);
                delay = (int32_t)delayNs;
                delay = (delay > SHUB_TIMER_MAX) ? SHUB_TIMER_MAX : delay;
                if(power_state != 0){
                    //poll on -> batch on
                    if((batch_param.m_Latency == 0) && (param.m_Latency > 0))
                    {
                        batch_param.m_Flasg    = param.m_Flasg;
                        batch_param.m_PeriodNs = param.m_PeriodNs;
                        batch_param.m_Latency  = param.m_Latency;

                        //pause poll 
                        currentActive = 0;
                        shub_set_sensor_poll(0);

                        //enable batch
                        shub_set_delay_logging(SHUB_ACTIVE_SENSOR, batch_param.m_PeriodNs);
                        ret = shub_activate_logging(SHUB_ACTIVE_SENSOR, 1);
                        if(ret == -1){
                            mutex_unlock(&shub_lock);
                            return -EFAULT;
                        }

                        //disable poll 
                        ret = shub_activate( SHUB_ACTIVE_SENSOR, 0);
                        if(ret == -1){
                            mutex_unlock(&shub_lock);
                            return -EFAULT;
                        }

                        //start batch
                        setMaxBatchReportLatency(SHUB_ACTIVE_SENSOR, batch_param.m_Latency);

                        mutex_unlock(&shub_lock);
                        return 0;
                    }
                    //batch on -> poll on
                    if((batch_param.m_Latency > 0) && (param.m_Latency == 0))
                    {
                        batch_param.m_Flasg    = param.m_Flasg;
                        batch_param.m_PeriodNs = param.m_PeriodNs;
                        batch_param.m_Latency  = param.m_Latency;

                        //pause batch
                        setMaxBatchReportLatency(SHUB_ACTIVE_SENSOR, batch_param.m_Latency);

                        //enable poll
                        shub_set_delay(SHUB_ACTIVE_SENSOR, delay);
                        ret = shub_activate( SHUB_ACTIVE_SENSOR, 1);

                        //disable batch
                        ret = shub_activate_logging(SHUB_ACTIVE_SENSOR, 0);
                        if(ret == -1){
                            mutex_unlock(&shub_lock);
                            return -EFAULT;
                        }

                        //start poll
                        currentActive = shub_get_current_active() & SHUB_ACTIVE_SENSOR;
                        if(ret == -1){
                            mutex_unlock(&shub_lock);
                            return -EFAULT;
                        }
                        shub_set_sensor_poll(1);
                        mutex_unlock(&shub_lock);
                        return 0;
                    }
                }
                /* flag SENSORS_BATCH_DRY_RUN is OFF */
                batch_param.m_Flasg    = param.m_Flasg;
                batch_param.m_PeriodNs = param.m_PeriodNs;
                batch_param.m_Latency  = param.m_Latency;

                if(param.m_Latency == 0){
                    shub_set_delay(SHUB_ACTIVE_SENSOR, delay);
                    if(currentActive != 0){
                        shub_set_sensor_poll(1);
                    }
                }else{
                    shub_set_delay_logging(SHUB_ACTIVE_SENSOR, batch_param.m_PeriodNs);
                }
                setMaxBatchReportLatency(SHUB_ACTIVE_SENSOR, batch_param.m_Latency);
                mutex_unlock(&shub_lock);
            }
            break;

        case SHUBIO_BARO_FLUSH :
            {
                DBG_BARO_IO("ioctl(cmd = Flush)\n");
                mutex_lock(&shub_lock);
                if(power_state != 0){
                    shub_logging_flush();
                    setMaxBatchReportLatency(SHUB_ACTIVE_SENSOR, batch_param.m_Latency);
                    shub_input_sync_init(shub_idev);
                    shub_input_first_report(shub_idev, 1);
                    input_event(shub_idev, EV_SYN, SYN_REPORT, (SHUB_INPUT_META_DATA | SHUB_INPUT_BARO));
                }else{
                    mutex_unlock(&shub_lock);
                    return -EFAULT;
                }
                mutex_unlock(&shub_lock);
            }
            break;

        case SHUBIO_BARO_SET_OFFSET:
            {
                IoCtlSetOffset ioc;
                memset(&ioc , 0x00, sizeof(ioc));
                ret = copy_from_user(&ioc,argp,sizeof(ioc));

                if (ret) {
                    printk( "error(copy_from_user) : shub_ioctl(cmd = SHUBIO_BARO_SET_OFFSET)\n" );
                    return -EFAULT;
                }
                DBG_BARO_IO("ioctl(cmd = Set_Offset) : value=%x\n", ioc.m_iOffset[0]);
                shub_set_baro_offset(ioc.m_iOffset[0]);
            }
            break;

        case SHUBIO_BARO_GET_OFFSET:
            {
                IoCtlSetOffset ioc;
                shub_get_baro_offset(ioc.m_iOffset);
                DBG_BARO_IO("ioctl(cmd = Get_Offset) : value=%x\n", ioc.m_iOffset[0]);
                ret = copy_to_user(argp, &ioc, sizeof(IoCtlSetOffset));
                if (ret) {
                    printk( "error(copy_from_user) : shub_ioctl(cmd = SHUBIO_BARO_GET_OFFSET )\n" );
                    return -EFAULT;
                }
            }
            break;

        default:
            return -ENOTTY;
    }
    return 0;
}


static long shub_ioctl_wrapper(struct file *filp, unsigned int cmd, unsigned long arg)
{
    SHUB_DBG_TIME_INIT
    long ret = 0;

    shub_qos_start();
    SHUB_DBG_TIME_START
    ret = shub_ioctl(filp, cmd , arg);
    SHUB_DBG_TIME_END(cmd)
    shub_qos_end();

    return ret;
}

static void shub_sensor_poll_work_func(struct work_struct *work)
{
    int32_t xyz[INDEX_SUM]= {0};
    if(currentActive != 0){
        mutex_lock(&shub_lock);
        shub_qos_start();
        shub_get_sensors_data(SHUB_ACTIVE_SENSOR, xyz);
        shub_input_report_baro(xyz);
        shub_qos_end();
        mutex_unlock(&shub_lock);
    }
}

static enum hrtimer_restart shub_sensor_poll(struct hrtimer *tm)
{
    schedule_work(&sensor_poll_work);
    hrtimer_forward_now(&poll_timer, ns_to_ktime((int64_t)delay * NSEC_PER_MSEC));
    return HRTIMER_RESTART;
}


void shub_suspend_baro(void)
{
    if(currentActive != 0){
        shub_set_sensor_poll(0);
        cancel_work_sync(&sensor_poll_work);
    }
}

void shub_resume_baro(void)
{
    if(currentActive != 0){
        shub_set_sensor_poll(1);
    }
}

void shub_input_report_baro(int32_t *data)
{
    if(data == NULL) {
        return;
    }

    data[INDEX_PRESSURE] = shub_adjust_value(BARO_MIN, BARO_MAX, data[INDEX_PRESSURE]); /* SHMDS_HUB_0120_01 add */

    DBG_BARO_DATA("data Pressure=%d, t(s)=%d, t(ns)=%d\n", data[INDEX_PRESSURE],data[INDEX_TM],data[INDEX_TMNS]);

    SHUB_INPUT_VAL_CLEAR(shub_idev, ABS_PRESSURE, data[INDEX_PRESSURE]);
    input_report_abs(shub_idev, ABS_PRESSURE, data[INDEX_PRESSURE]);
    input_report_abs(shub_idev, ABS_HAT2Y, data[INDEX_TM]);
    input_report_abs(shub_idev, ABS_HAT2X, data[INDEX_TMNS]);
    shub_input_sync_init(shub_idev);
    input_event(shub_idev, EV_SYN, SYN_REPORT, SHUB_INPUT_BARO);
}

static void shub_set_sensor_poll(int32_t en)
{
    hrtimer_cancel(&poll_timer);
    if(en){
        hrtimer_start(&poll_timer, ns_to_ktime((int64_t)delay * NSEC_PER_MSEC), HRTIMER_MODE_REL);
    }
}

void shub_sensor_rep_input_baro(struct seq_file *s)
{
    seq_printf(s, "[baro      ]");
    seq_printf(s, "power_state=%d, ",power_state);
    seq_printf(s, "delay=%d, ",delay);
    seq_printf(s, "batch_param.m_Flasg=%d, ",batch_param.m_Flasg);
    seq_printf(s, "batch_param.m_PeriodNs=%lld, ",batch_param.m_PeriodNs);
    seq_printf(s, "batch_param.m_Latency=%lld\n",batch_param.m_Latency);
}

static struct file_operations shub_fops = {
    .owner   = THIS_MODULE,
    .unlocked_ioctl = shub_ioctl_wrapper,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = shub_ioctl_wrapper,
#endif
};

static struct miscdevice shub_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = MISC_DEV_NAME,
    .fops  = &shub_fops,
};

static int32_t __init shub_init(void)
{
    int32_t ret = 0;


    if(!shub_connect_check()){
        printk(KERN_INFO "shub_baro Connect Error!!\n");
        ret = -ENODEV;
        goto out_driver;
    }


    pdev = platform_device_register_simple(INPUT_DEV_NAME, -1, NULL, 0);
    if (IS_ERR(pdev)) {
        ret = PTR_ERR(pdev);
        goto out_driver;
    }

    shub_idev = shub_com_allocate_device(SHUB_INPUT_BARO, &pdev->dev);
    if (!shub_idev) {
        ret = -ENOMEM;
        goto out_device;
    }

    ret = misc_register(&shub_device);
    if (ret) {
        printk("shub-init: shub_io_device register failed\n");
        goto exit_misc_device_register_failed;
    }
    INIT_WORK(&sensor_poll_work, shub_sensor_poll_work_func);
    hrtimer_init(&poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    poll_timer.function = shub_sensor_poll;
    return 0;

exit_misc_device_register_failed:
out_device:
    platform_device_unregister(pdev);
out_driver:
    return ret;
}

static void __exit shub_exit(void)
{
    misc_deregister(&shub_device);
    shub_com_unregister_device(SHUB_INPUT_BARO);
    platform_device_unregister(pdev);

    cancel_work_sync(&sensor_poll_work);
}

module_init(shub_init);
module_exit(shub_exit);

MODULE_DESCRIPTION("SensorHub Input Device (Barometric)");
MODULE_AUTHOR("LAPIS SEMICOMDUCTOR");
MODULE_LICENSE("GPL v2");
