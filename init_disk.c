#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <unistd.h>

#define BLOCK_SIZE 512
#define FS_SIZE 8 * 1024 * 1024
#define SUPER_BLOCK 1
#define ROOT_DIR_BLOCK 1     // 根目录块
#define INODE_BITMAP_BLOCK 1 // inode位图块数
#define DATA_BITMAP_BLOCK 4  // 数据块位图 块数
#define INODE_AREA_BLOCK 512 // inode区块数
#define MAX_DATA_IN_BLOCK 512
#define BLOCK_NUMS (8 * 1024 * 1024 / BLOCK_SIZE)                                                              // 系统总块数
#define DATA_AREA_BLOCK (BLOCK_NUMS - SUPER_BLOCK - INODE_BITMAP_BLOCK - DATA_BITMAP_BLOCK - INODE_AREA_BLOCK) // 剩下的空闲块数用作数据区的块
#define DIR_SIZE 16
#define MAX_FILENAME 8
#define MAX_EXTENSION 3

// 数据结构的定义
// 超级块sb 占用一个磁盘块 总字节为56B
struct sb
{
    long fs_size;                  // 文件系统的大小，以块为单位
    long first_blk;                // 数据区的第一块块号，根目录也放在此
    long datasize;                 // 数据区大小，以块为单位
    long first_inode;              // inode区起始块号
    long inode_area_size;          // inode区大小，以块为单位
    long fisrt_blk_of_inodebitmap; // inode位图区起始块号
    long inodebitmap_size;         // inode位图区大小，以块为单位
    long first_blk_of_databitmap;  // 数据块位图起始块号
    long databitmap_size;          // 数据块位图大小，以块为单位
};
// Inode 占用一个磁盘块 总字节为64B
struct inode
{
    short int st_mode;       /* 权限，2字节 */
    short int st_ino;        /* i-node号，2字节 */
    char st_nlink;           /* 连接数，1字节 */
    uid_t st_uid;            /* 拥有者的用户 ID ，4字节 */
    gid_t st_gid;            /* 拥有者的组 ID，4字节  */
    off_t st_size;           /*文件大小，4字节 */
    struct timespec st_atim; /* 16个字节time of last access */
    short int addr[7];       /*磁盘地址，14字节*/
};

// 记录目录信息的数据结构
struct file_directory
{
    // 文件名占用前8字节，拓展名占用3字节
    char fname[MAX_FILENAME]; // 文件名
    char fext[MAX_EXTENSION]; // 扩展名
    // inode占用2字节
    short int st_ino;
    // 备用占用最后3字节
    char standby[3];
};

// 存放文件的数据块，抽象成一个数据结构
struct data_block
{
    char data[MAX_DATA_IN_BLOCK]; // 一个块里面实际能存的数据大小
};

int main()
{
    FILE *fp = NULL;
    fp = fopen("/home/wangchen/桌面/SFS/disk.img", "r+"); // 打开文件
    if (fp == NULL)
    {
        printf("打开文件失败，文件不存在\n");
        return 0;
    }
    // 文件系统初始化
    //  1.初始化超级块sb 1块
    struct sb *super_blk = malloc(sizeof(struct sb));
    super_blk->fs_size = BLOCK_NUMS;
    super_blk->first_blk = SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK + INODE_AREA_BLOCK;
    super_blk->datasize = DATA_AREA_BLOCK;
    super_blk->first_inode = SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK;
    super_blk->inode_area_size = INODE_AREA_BLOCK;
    super_blk->first_blk_of_databitmap = SUPER_BLOCK;
    super_blk->inodebitmap_size = INODE_BITMAP_BLOCK;
    super_blk->first_blk_of_databitmap = SUPER_BLOCK + INODE_BITMAP_BLOCK;
    super_blk->databitmap_size = DATA_BITMAP_BLOCK;
    fseek(fp, 0, SEEK_SET);
    fwrite(super_blk, sizeof(struct sb), 1, fp);
    printf("initial super_block success!\n");

    // 2.初始化inode位图区 1块 512B = 4Kbit
    // 首先将指针移动到文件的第二块
    if (fseek(fp, BLOCK_SIZE * 1, SEEK_SET) != 0)
    {
        fprintf(stderr, "inode bitmap fseek failed!\n");
    }
    // 此时数据区第一块放的是根目录文件，所以inode位图区的第一个bit标记为1，其他为0
    struct data_block *inode_bitmap = (struct data_block *)malloc(sizeof(struct data_block));
    memset(inode_bitmap, 0, sizeof(struct data_block));
    // 0x80是128，表示一个B里面的8个bit全1（补码表示）
    inode_bitmap->data[0] = 0x80;
    fseek(fp, SUPER_BLOCK * BLOCK_SIZE, SEEK_SET);
    fwrite(inode_bitmap, sizeof(struct inode_bitmap), 1, fp);
    printf("initial inode_bitmap_area success!\n");
    free(inode_bitmap);

    // 3.初始化数据位图区 4块 4*512B = 16K bit
    // 首先将指针移动到文件的第三块开头
    if (fseek(fp, BLOCK_SIZE * (SUPER_BLOCK + INODE_BITMAP_BLOCK), SEEK_SET) != 0)
    {
        fprintf(stderr, "inode bitmap fseek failed!\n");
    }
    // 分别写入四个数据块
    // 该数据块可以一直循环利用
    struct data_block *block = (struct data_block *)malloc(sizeof(struct data_block));
    // 第一个数据块被目录文件占用
    block->data[0] = 0x80;
    fwrite(block, sizeof(struct data_block), 1, fp);
    // 重置一下数据块
    memset(block, 0, 512);
    fwrite(block, sizeof(struct data_block), 1, fp);
    fwrite(block, sizeof(struct data_block), 1, fp);
    fwrite(block, sizeof(struct data_block), 1, fp);
    printf("initial data_bitmap_block success!\n");

    // 4.初始化inode区  512块
    // 这里先把数据块转化成inode
    struct inode *dirInode = (struct inode *)block;
    // 进行初始化的设置
    dirInode->st_mode = S_IFDIR | 0766;                                                                // 表示为目录,并且可读写执行
    dirInode->st_ino = 0;                                                                              // 先把inode号初始化为0
    dirInode->st_nlink = 2;                                                                            // 一个链接是目录自身，另一个链接是指向该目录的父目录。
    dirInode->st_size = 0;                                                                             // 一个目录下file_directory大小为16B，根目录下还没有目录项，初始化为0
    dirInode->st_uid = getuid();                                                                       // 拥有者的用户ID
    dirInode->st_gid = getgid();                                                                       // 拥有者的组ID
    memset(dirInode->addr, 0, sizeof(dirInode->addr));                                                 // 不包含实际文件数据，可以初始化为全0
                                                                                                       //  dirInode->addr[0] = 0;
    if (fseek(fp, (SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK) * BLOCK_SIZE, SEEK_SET) != 0) // 将指针移动到INODE区的起始位置
        fprintf(stderr, "INODE area fseek failed!\n");
    fwrite(block, sizeof(struct data_block), 1, fp);
    printf("initial inode_area success!\n");

    // 5.初始化数据区dataArea
    // 其实就是写入一个目录文件的数据块
    // 先移动指针
    if (fseek(fp, (SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK + DATA_AREA_BLOCK) * BLOCK_SIZE, SEEK_SET) != 0) // 将指针移动到数据区的起始位置
        fprintf(stderr, "DATA area fseek failed!\n");
    fwrite(block, sizeof(struct data_block), 1, fp);
    // 把目录项写进数据区之后，释放掉该block
    free(block);
    close(fp);
    printf("DATA AREA init success!\n");

    // 上述操作完成之后初始化完毕

    {
        FILE *fp;
        fp = fopen(diskpath, "r+");
        fseek(fp, (ROOT_DIR_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK) * BLOCK_SIZE, SEEK_SET);
        struct data_block *b = malloc(sizeof(struct data_block));
        fread(b, sizeof(struct data_block), 1, fp);
        struct inode *ino = (struct inode *)b;
        printf("%d\n%o\n", ino->st_size, ino->st_mode);
        free(b);
    }
    return 0;
}