#ifndef UTIL_H
#define UTIL_H
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
namespace util
{
    int mem_addr(unsigned long vaddr, unsigned long *paddr);
    void print_address(const char *msg, unsigned long virt_addr, unsigned long virt_end);
}
/**
 * @brief 物理地址转虚拟地址
 *
 * @param vaddr
 * @param paddr
 * @return 0 is success, -1 is failed!
 */
int util::mem_addr(unsigned long vaddr, unsigned long *paddr)
{
    int pageSize = getpagesize(); // //调用此函数获取系统设定的页面大小

    unsigned long v_pageIndex = vaddr / pageSize;            //计算此虚拟地址相对于0x0的经过的页面数
    unsigned long v_offset = v_pageIndex * sizeof(uint64_t); //计算在/proc/pid/page_map文件中的偏移量
    unsigned long page_offset = vaddr % pageSize;            //计算虚拟地址在页面中的偏移量
    uint64_t item = 0;                                       //存储对应项的值

    int fd = open("/proc/self/pagemap", O_RDONLY); //以只读方式打开/proc/pid/page_map
    if (fd == -1)                                  //判断是否打开失败

    {
        printf("open /proc/self/pagemap error\n");
        return -1;
    }

    if (lseek(fd, v_offset, SEEK_SET) == -1) //将游标移动到相应位置，即对应项的起始地址且判断是否移动失败

    {
        printf("sleek error\n");
        return -1;
    }

    if (read(fd, &item, sizeof(uint64_t)) != sizeof(uint64_t)) //读取对应项的值，并存入item中，且判断读取数据位数是否正确

    {
        printf("read item error\n");
        return -1;
    }

    if ((((uint64_t)1 << 63) & item) == 0) //判断present是否为0
    {
        printf("page present is 0\n");
        return -1;
    }

    uint64_t phy_pageIndex = (((uint64_t)1 << 55) - 1) & item; //计算物理页号，即取item的bit0-54

    *paddr = (phy_pageIndex * pageSize) + page_offset; //再加上页内偏移量就得到了物理地址
	*paddr = *paddr - 0x80000000;
    return 0;
}
/**
 * @brief 给定两个虚拟地址，和一个打印信息，打印他们的虚拟地址区间和物理地址区间
 * 
 * @param msg 
 * @param virt_addr 
 */
void util::print_address(const char *msg, unsigned long virt_addr, unsigned long virt_end)
{
    printf("%s [virtual address] : [0x%lx,0x%lx]\n", msg, virt_addr,virt_end);
    unsigned long phys_start;
    unsigned long phys_end;
    int ret1 = mem_addr(virt_addr, &phys_start);
    int ret2 = mem_addr(virt_end, &phys_end);
    if (ret1 == 0 && ret2==0)
    {
        printf("%s [physical address] : [0x%lx,0x%lx]\n", msg, phys_start,phys_end);
    }
}

#endif
