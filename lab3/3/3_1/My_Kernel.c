#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <asm/current.h>

#define procfs_name "Mythread_info"
#define BUFSIZE  1024
char buf[BUFSIZE];

static ssize_t Mywrite(struct file *fileptr, const char __user *ubuf, size_t buffer_len, loff_t *offset){
    /* Do nothing */
	return 0;
}


static ssize_t Myread(struct file *fileptr, char __user *ubuf, size_t buffer_len, loff_t *offset){
    /*Your code here*/
    int len = 0;
    struct task_struct *thread;
    
    if (*offset > 0)
        return 0;
    
    // Print main process ID
    len += sprintf(buf + len, "PID: %d\n", current->pid);
    
    // Iterate through all threads in the thread group
    for_each_thread(current, thread) {
        len += sprintf(buf + len, "TID: %d, Priority: %d, State: %ld\n",
                      thread->pid,
                      thread->prio,
                      thread->__state);
    }
    
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