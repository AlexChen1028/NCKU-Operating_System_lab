#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <asm/current.h>

#define procfs_name "Mythread_info"
#define BUFSIZE  1024
char buf[BUFSIZE]; //kernel buffer
static char stored_data[256]; // Store the written string
static int data_len = 0;

static ssize_t Mywrite(struct file *fileptr, const char __user *ubuf, size_t buffer_len, loff_t *offset){
    /*Your code here*/
    if (buffer_len >= sizeof(stored_data))
        buffer_len = sizeof(stored_data) - 1;
    
    if (copy_from_user(stored_data, ubuf, buffer_len))
        return -EFAULT;
    
    stored_data[buffer_len] = '\0';
    data_len = buffer_len;
    
    // Remove newline if present
    if (data_len > 0 && stored_data[data_len - 1] == '\n') {
        stored_data[data_len - 1] = '\0';
        data_len--;
    }
    
    return buffer_len;
    /****************/
}


static ssize_t Myread(struct file *fileptr, char __user *ubuf, size_t buffer_len, loff_t *offset){
    /*Your code here*/
    int len = 0;
    
    if (*offset > 0)
        return 0;
    
    // Display the string written by the thread
    if (data_len > 0) {
        len += sprintf(buf + len, "String: %s\n", stored_data);
    }
    
    // Display thread info
    len += sprintf(buf + len, "PID: %d\n", current->tgid);
    len += sprintf(buf + len, "TID: %d\n", current->pid);
    len += sprintf(buf + len, "Time (ms): %llu\n", current->utime / 100 / 1000);
    
    if (copy_to_user(ubuf, buf, len))
        return -EFAULT;
    
    *offset = len;
    return len;
    /****************/
}

static struct proc_ops Myops = {
    .proc_read = Myread,
    .proc_write = Mywrite,
};

static int My_Kernel_Init(void){
    proc_create(procfs_name, 0644, NULL, &Myops);   
    pr_info("My kernel says Hi");
    return 0;
}

static void My_Kernel_Exit(void){
    remove_proc_entry(procfs_name, NULL);
    pr_info("My kernel says GOODBYE");
}

module_init(My_Kernel_Init);
module_exit(My_Kernel_Exit);

MODULE_LICENSE("GPL");
