#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>
#include <linux/errno.h>

static int resolve_va_to_pa(struct mm_struct *mm,
                            unsigned long vaddr,
                            unsigned long *paddr)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    spinlock_t *ptl;
    unsigned long pfn;
    unsigned long offset;

    struct vm_area_struct *vma;

    vma = find_vma(mm, vaddr);
    if (!vma || vaddr < vma->vm_start)
        return -EINVAL;

    pgd = pgd_offset(mm, vaddr);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return -EINVAL;

    p4d = p4d_offset(pgd, vaddr);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return -EINVAL;

    pud = pud_offset(p4d, vaddr);
    if (pud_none(*pud) || pud_bad(*pud))
        return -EINVAL;

    pmd = pmd_offset(pud, vaddr);
    if (pmd_none(*pmd) || pmd_bad(*pmd))
        return -EINVAL;

    pte = pte_offset_map_lock(mm, pmd, vaddr, &ptl);
    if (!pte)
        return -EINVAL;

    if (!pte_present(*pte)) {
        pte_unmap_unlock(pte, ptl);
        return -EINVAL;
    }

    pfn = pte_pfn(*pte);
    offset = vaddr & ~PAGE_MASK;

    *paddr = (pfn << PAGE_SHIFT) | offset;

    pte_unmap_unlock(pte, ptl);

    return 0;
}

SYSCALL_DEFINE3(va_to_pa,
                pid_t, pid,
                unsigned long, vaddr,
                unsigned long __user *, user_paddr)
{
    struct task_struct *task;
    struct mm_struct *mm;
    unsigned long paddr;
    int ret;

    if (!user_paddr)
        return -EINVAL;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();

    if (!task)
        return -ESRCH;

    mm = get_task_mm(task);
    put_task_struct(task);

    if (!mm)
        return -EINVAL;

    mmap_read_lock(mm);
    ret = resolve_va_to_pa(mm, vaddr, &paddr);
    mmap_read_unlock(mm);

    mmput(mm);

    if (ret)
        return ret;

    if (copy_to_user(user_paddr, &paddr, sizeof(paddr)))
        return -EFAULT;

    return 0;
}
