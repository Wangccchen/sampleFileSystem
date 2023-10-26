#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <malloc.h>
#include <math.h>

#define BLOCK_SIZE 512
#define FS_SIZE 8 * 1024 * 1024
#define INODE_SIZE 64
#define SUPER_BLOCK 1
#define ROOT_DIR_BLOCK 1                          // 根目录块
#define INODE_BITMAP_BLOCK 1                      // inode位图块数
#define DATA_BITMAP_BLOCK 4                       // 数据块位图 块数
#define INODE_AREA_BLOCK 512                      // inode区块数
#define MAX_DATA_IN_BLOCK 512                     // 一个数据块实际能装的大小
#define BLOCK_NUMS (8 * 1024 * 1024 / BLOCK_SIZE) // 文件系统的总块数8MB/512B

#define INODE_BITMAP_START_NUM SUPER_BLOCK                                                             // INODE位图区开始的块号
#define DATA_BITMAP_START_NUM (SUPER_BLOCK + INODE_BITMAP_BLOCK)                                       // 数据位图区开始的块号
#define INODE_BLOCK_START_NUM (SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK)                   // INODE区开始的块号
#define DATA_BLOCK_START_NUM (SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK + INODE_AREA_BLOCK) // 数据区开始的块数                                                      //系统总块数

#define DATA_AREA_BLOCK (BLOCK_NUMS - SUPER_BLOCK - INODE_BITMAP_BLOCK - DATA_BITMAP_BLOCK - INODE_AREA_BLOCK) // 剩下的空闲块数用作数据区的块
#define FILE_DIRECTORY_SIZE 16
#define MAX_FILENAME 8
#define MAX_EXTENSION 3
#define MAX_DIR_IN_BLOCK (MAX_DATA_IN_BLOCK / FILE_DIRECTORY_SIZE)
#define INODE_NUMS_IN_BLOCK (BLOCK_SIZE / INODE_SIZE)
// 用于一级二级三级索引的数据信息
#define FIRST_INDEX_NUMS 4
#define SINGLE_BLOCK_STORE_NO_NUMS 256
#define SECONDARY_INDEX_NUMS (FIRST_INDEX_NUMS + SINGLE_BLOCK_STORE_NO_NUMS)
#define TRIPLE_INDEX_NUMS (SECONDARY_INDEX_NUMS + SINGLE_BLOCK_STORE_NO_NUMS * SINGLE_BLOCK_STORE_NO_NUMS)

// 用于判断inode对应的文件是文件还是目录
#define IS_DIRECTORY(mode) (((mode)&S_IFMT) == S_IFDIR)
#define IS_REGULAR_FILE(mode) (((mode)&S_IFMT) == S_IFREG)

// 数据结构的定义
#pragma region 
// #pragma pack(1)
// 超级块sb 占用一个磁盘块 总字节为72B
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
    short int st_ino;         // inode占用2字节 表示inode的块号（从0开始）
    char standby[3];          // 备用占用最后3字节
};

// 存放文件的数据块，抽象成一个数据结构
struct data_block
{
    char data[MAX_DATA_IN_BLOCK]; // 一个块里面实际能存的数据大小 512B
};
#pragma endregion

// 我的磁盘8M文件路径
char *disk_path = "/home/wc/桌面/SFS/disk.img";

// 辅助函数声明
int read_block_by_no(struct data_block *dataB_blk, short no);                            // 该函数用于根据数据块号读取对应的数据块
int read_inode_by_no(struct inode *ind, short no);                                       // 该函数用于根据inode号读取对应的inode
int read_inode_by_fd(struct inode *ind, struct file_directory *fd);                      // 该函数用于根据给定的fd来读取对应的inode
int read_block_by_ind_and_indNo(struct inode *ind, short indNo, struct data_block *blk); // 该函数用于根据给定的inode和inode对应的数据块的块号（相对与inode对应的总的所有数据块）来获取对应文件的数据块
int write_block_by_no(struct data_block *dataB_blk, short no);                           // 该函数用于根据数据块号来对对应的数据块进行写入
int write_inode_by_no(struct inode *ind, short no);                                      // 该函数用于根据inode号来对inode写入
int write_block_by_ind_and_indNo(struct inode *ind, int indNo, struct data_block *blk);  // 该函数用于根据给定的inode和inode对应的数据块的块号（相对与inode对应的总的所有数据块）来写回对应文件的数据块
int read_inode(struct inode *ind, short no);

int determineFileType(const struct inode *myInode);
int is_inode_exists_fd_by_fname(struct inode *ind, char *fname, struct file_directory *fd, int *blk_no, int *offsize);
int is_same_fd(struct file_directory *fd, const char *fname);

// 功能函数声明
int get_fd_to_attr(const char *path, struct file_directory *attr);
// int get_fd_name(struct file_directory *fd, char *fname);
int get_parent_and_fname(const char *path, char **parent_path, char **fname);
int get_info_by_path(const char *path, struct inode *ind, struct file_directory *fd);
int create_file_dir(const char *path, int flag);
int remove_file_dir(struct inode *ind, const char *filename, int flag);
int create_new_fd_to_inode(struct inode *ind, const char *fname, int flag);
int create_dir_by_ino_number(char *fname, char *ext, short int ino_no, char *exp, struct file_directory *fd);
int clear_inode_map_by_no(const short st_ino);
int get_blk_no_by_indNo(struct inode *ind, const short int indNo); // 根据inode对应的文件的数据块的相对块号，来获取对应数据块的绝对块号
short assign_block();
short assign_inode();
int sec_index(short int addr, int offset, short int blk_offset); // 该函数用于获取二级索引块中的一级索引地址
int fir_index(short int addr, int offset, short int blk_offset); // 该函数用于获取一级索引块中的直接索引地址
int zerobit_no(unsigned char *p);
// // 要实现的核心文件系统函数在此，fuse会根据命令来对我们编写的函数进行调用
// static struct fuse_operations SFS_oper = {
//     .init = SFS_init,       // 初始化
//     .getattr = SFS_getattr, // 获取文件属性（包括目录的）
//     .mknod = SFS_mknod,     // 创建文件
//     .unlink = SFS_unlink,   // 删除文件
//     .open = SFS_open,       // 无论是read还是write文件，都要用到打开文件
//     .read = SFS_read,       // 读取文件内容
//     .write = SFS_write,     // 修改文件内容
//     .mkdir = SFS_mkdir,     // 创建目录
//     .rmdir = SFS_rmdir,     // 删除目录
//     .access = SFS_access,   // 进入目录
//     .readdir = SFS_readdir, // 读取目录
// };
// 核心函数的实现
#pragma region

// 对初始化函数SFS_init的实现
static void *SFS_init(struct fuse_conn_info *conn)
{
    // 其实这个init函数对整个文件系统左右不算太大，因为我们要得到的文件系统大小的数据
    // 其实在宏定义已经算出来了
    // 只不过该文件系统必须执行这个函数，所以只能按步骤走
    printf("SFS_init函数开始\n\n");
    FILE *fp = NULL;
    fp = fopen(disk_path, "r+");
    if (fp == NULL)
    {
        fprintf(stderr, "错误：打开文件失败，文件不存在，函数结束返回\n");
        return;
    }
    // 首先读取超级块里面的内容获取文件系统的信息
    struct sb *sb_blk = malloc(sizeof(struct sb));
    fread(sb_blk, sizeof(struct sb), 1, fp);
    fclose(fp);
    // 下面随便输出一些超级块的内容，确定读写是否成功
    printf("该文件系统的总块数为%ld\n", sb_blk->fs_size);
    // 测试完成可以free掉
    free(sb_blk);
    printf("SFS_init函数结束\n");
    return 0;
}
/*struct stat {
        mode_t     st_mode;       //文件对应的模式，文件，目录等
        ino_t      st_ino;       //inode节点号
        dev_t      st_dev;        //设备号码
        dev_t      st_rdev;       //特殊设备号码
        nlink_t    st_nlink;      //文件的连接数
        uid_t      st_uid;        //文件所有者
        gid_t      st_gid;        //文件所有者对应的组
        off_t      st_size;       //普通文件，对应的文件字节数
        time_t     st_atime;      //文件最后被访问的时间
        time_t     st_mtime;      //文件内容最后被修改的时间
        time_t     st_ctime;      //文件状态改变时间
        blksize_t st_blksize;    //文件内容对应的块大小
        blkcnt_t   st_blocks;     //文件内容对应的块数量
      };*/

// 读取文件属性的函数SFS_getattr,并且赋值给stbuf
// 查找输入的路径，确定它是一个目录还是一个文件。
// 如果是目录，返回适当的权限。如果是文件，返回适当的权限以及实际大小。
static int SFS_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    printf("SFS_getattr函数执行\n\n");
    // 通过对应的inode来判断文件的属性
    struct inode *ino_tmp = malloc(sizeof(struct inode));
    read_inode_by_no(ino_tmp,0);
    printf("SFS_getattr:拿到根目录inode号:%d，大小为:%d\n",ino_tmp->st_ino,ino_tmp->st_size);
    // 重新设置stbuf的内容
    memset(stbuf, 0, sizeof(struct stat));
    struct file_directory *t_file_directory = malloc(sizeof(struct file_directory));
    // 非根目录
    if (get_info_by_path(path, ino_tmp, t_file_directory) != 0)
    {
        free(t_file_directory);
        free(ino_tmp);
        printf("SFS_getattr：get_fd_to_attr时没找到文件，函数结束返回\n");
        return -ENOENT;
    }
    // 读取了path下对应的inode和fd，赋值给stbuf
    //  读取inode的数据并赋给stbuf
    // stbuf->st_ino = ino_tmp->st_ino;
    // stbuf->st_atime = ino_tmp->st_atim;
    // 下面判断文件是 目录 还是 一般的文件
    // 并且修改stbuf对应的权限模式
    //  0666代表允许所有用户读取和写入目录，权限位是rw-rw-rw-
    //  0766表示权限位是rwxrw-rw-
    // 根据返回值来判断
    int fileType = determineFileType(ino_tmp);
    int ret = 0; // 设置该函数的返回值
    switch (fileType)
    {
    case 1:
        printf("这是一个名为%s的目录，inode号为%d\n", t_file_directory->fname, t_file_directory->st_ino);
        stbuf->st_mode = S_IFDIR | 0766;
        break;
    case 2:
        printf("这是一个文件名为%s的文件，inode号为%d\n", t_file_directory->fname, t_file_directory->st_ino);
        stbuf->st_mode = S_IFREG | 0666;
        stbuf->st_size = ino_tmp->st_size;
        break;
    default:
        printf("文件不存在！！\n");
        ret = -ENOENT;
        break;
    }
    free(t_file_directory);
    free(ino_tmp);
    printf("SFS_getattr函数执行结束\n\n");
    return ret;
}

// SFS_readdir: 根据输入路径来读取该目录，并且显示目录内的内容
//  在终端中输入ls -l，libfuse会调用该函数
//  1.根据path拿到对应的inode
//  2.读取目录inode对应的目录数据块
//  3.遍历该数据块下的所有目录并且显示
static int SFS_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    printf("SFS_readdir开始执行!\n");
    struct inode *tinode = malloc(sizeof(struct inode));
    struct data_block *blk = malloc(sizeof(struct data_block));
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    // 调用上面刚刚实现的辅助函数
    // 打开path指定的inode
    if (get_info_by_path(path, tinode, tfd) != 0)
    {
        printf("readdir:找不到该文件！\n");
        free(tinode);
        free(blk);
        free(tfd);
        return -ENOENT;
    }
    // 拿到该inode之后，要保证对应的一定为目录！
    if (determineFileType(tinode) != 1)
    {
        // 如果返回值不为 1 ，说明inode对应的不是目录
        printf("readdir:%s下对应的不是目录！\n", path);
        free(tinode);
        free(tfd);
        free(blk);
        return -ENOENT;
    }
    // 对buf里面的先使用filter函数添加 . 和 ..
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    // // 设置一个装文件名的char数组
    char name[MAX_FILENAME + MAX_EXTENSION + 2];
    // 接下来的操作就是对inode中的每一个目录项进行提取对应的fd
    // 拿到对应的filename，然后放入buf中

    // 具体操作为：先利用for循环拿到inode目录项下的每一个数据块
    // 然后依次读取数据块存储的fd，提取其名字放入buf
    int fd_num = (tinode->st_size) / FILE_DIRECTORY_SIZE;                        // inode一共有多少个fd
    int fd_in_blk = fd_num;                                                      //  在第二层for循环中读取每个数据块中的数量的临时变量
    int blk_num = (tinode->st_size + MAX_DATA_IN_BLOCK - 1) / MAX_DATA_IN_BLOCK; // inode的目录项占有多少个数据块
    for (int i = 0; i < blk_num; i++)
    {
        // 依次读取inode下的每个数据块
        read_block_by_ind_and_indNo(tinode, i, blk);
        // 把blk分解成fd单位来遍历
        struct file_directory *fd = (struct file_directory *)(blk->data);
        if (fd_in_blk < MAX_DIR_IN_BLOCK)
        {
            // 如果inode对应的fd数量没有装满一个块
            for (int j = 0; j < fd_in_blk; j++)
            {
                // 读取该fd的名字
                // 拼接fd的文件名
                strncpy(name, fd->fname, 8);
                // 检查拓展名字段是否为空
                if (strlen(fd->fext) != 0)
                {
                    strcat(name, ".");
                }
                strncat(name, fd->fext, 3);
                filler(buf, name, NULL, 0, 0); // 写入到buf中
                fd++;                          // 指针偏移到blk中的下一个fd
            }
        }
        else
        {
            // inode对应的fd装满了一个块
            for (int j = 0; j < MAX_DIR_IN_BLOCK; j++)
            {
                // 读取该fd的名字
                // 拼接fd的文件名
                strncpy(name, fd->fname, 8);
                // 检查拓展名字段是否为空
                if (strlen(fd->fext) != 0)
                {
                    strcat(name, ".");
                }
                strncat(name, fd->fext, 3);
                filler(buf, name, NULL, 0, 0); // 写入到buf中
                fd++;                          // 指针偏移到blk中的下一个fd
            }
            // 剩余的没有遍历的inode中的fd数量更新
            fd_in_blk -= MAX_DIR_IN_BLOCK;
        }
    }
    free(blk);
    free(tfd);
    free(tinode);
    printf("SFS_readdir执行完成!\n");
    return 0;
}

// SFS_mkdir:将新目录添加到根目录，并且更新.directories文件
// 创建目录
static int SFS_mkdir(const char *path, mode_t mode)
{
    return create_file_dir(path, 2);
}

// SFS_rmdir：删除空目录
// 获取父目录的inode
// 在父目录的inode中遍历找到输入path的inode，对其进行判断是否为目录
// 设置两个位图区
static int SFS_rmdir(const char *path)
{
    // 首先获取父目录路径和文件名
    char *par_path = NULL;
    char *fname = NULL;
    get_parent_and_fname(path, &par_path, &fname);
    // 根据父目录来获取父目录的inode和fd
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    struct inode *tinode = malloc(sizeof(struct inode));
    get_info_by_path(par_path, tinode, tfd);
    // 要对此时父目录的inode判断，是否为一个目录而不是文件
    if (determineFileType(tinode) != 1)
    {
        // 不为目录
        free(tfd);
        free(tinode);
        return -ENOENT;
    }
    // flag置为2表示删除空目录
    int res = remove_file_dir(tinode, fname, 2);

    free(tfd);
    free(tinode);
    return res;
}

// SFS_mknod：创建一个新的文件
static int SFS_mknod(const char *path, mode_t mode, dev_t dev)
{
    return create_file_dir(path, 1);
}

// SFS_unlink:删除文件
static int SFS_unlink(const char *path)
{
    // 首先获取父目录路径和文件名
    char *par_path = NULL;
    char *fname = NULL;
    get_parent_and_fname(path, &par_path, &fname);
    // 根据父目录来获取父目录的inode和fd
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    struct inode *tinode = malloc(sizeof(struct inode));
    get_info_by_path(par_path, tinode, tfd);
    // 要对此时父目录的inode判断，是否为一个目录而不是文件
    if (determineFileType(tinode) != 1)
    {
        // 不为目录
        free(tfd);
        free(tinode);
        return -ENOTDIR;
    }
    // flag置为1表示删除文件
    int res = remove_file_dir(tinode, fname, 1);

    free(tfd);
    free(tinode);
    return res;
}

// SFS_access:进入目录
static int SFS_access(const char *path, int flag)
{
    printf("SFS_access:进入目录了！\n");
    return 0;
}

// SFS_open：打开文件时的操作
static int SFS_open(const char *path, struct fuse_file_info *fi)
{
    printf("SFS_open:打开了文件！\n");
    return 0;
}

// SFS_read:读取文件的操作
// 根据path找到该文件的inode
// 根据offset偏移量,读取size大小的数据写入buf
static int SFS_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    // 首先根据path获取文件的inode
    struct inode *tind = malloc(sizeof(struct inode));
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    get_info_by_path(path, tind, tfd);
    // 判断该文件是否为目录
    if (determineFileType(tind) != 2)
    {
        free(tind);
        return -EISDIR;
    }
    printf("从inode号为%d,偏移%ld的位置读取大小为%ld的文件内容\n", tind->st_ino, offset, size);
    // 读取的大小size要小于inode文件的大小tind->st_size
    // 并且offset不能超过该文件inode所示的大小tind->st_size
    size_t read_size = size < (tind->st_size - offset) ? size : (tind->st_size - offset);
    read_size = read_size < 0 ? 0 : read_size; // 保证真实读取的大小要大于0

    // 下面追踪要读取的文件所在数据块的位置信息
    int blk_no = offset / MAX_DATA_IN_BLOCK;   // 获取数据块号（在inode里面）
    int p_in_blk = offset % MAX_DATA_IN_BLOCK; // 在当前块中的下标指向
    size = read_size;                          // 更新读取文件的大小
    off_t buf_index = offset;                  // 读入buf的下标

    struct data_block *tblk = (struct data_block *)malloc(sizeof(struct data_block));
    // 利用while循环不断读入新的数据块
    // 根据size和p_in_blk来找到写入的位置
    // 然后将size大小的数据写入buf
    while (buf_index < size)
    {
        // 读入块
        read_block_by_ind_and_indNo(tind, blk_no, tblk);
        int max_read = MAX_DATA_IN_BLOCK - p_in_blk; // 在当前块中最多能读取的字节
        // 看看是否超出了本来要读取的size的大小的量
        if (max_read > size)
        {
            max_read = size;
        }
        // 写入buf
        memcpy(buf + buf_index, tblk->data + p_in_blk, max_read);
        // 读入buf之后，对对应的下标指针都进行相应大小的增加
        size -= max_read;      // 要读的数据减少已读的数据的大小
        p_in_blk = 0;          // 下标置为起始位置
        blk_no++;              // 读取下一个块
        buf_index += max_read; // 指针移动读取字节数个单位
    }
    // 读取完成，释放
    free(tind);
    free(tfd);
    free(tblk);
    return read_size;
}

// SFS_write:将buf内容写入文件
// 根据path找到该文件的inode
// 根据offset偏移量,读取buf中size大小的数据
// 写入对应文件的inode和数据块
static size_t SFS_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    // 首先获得该文件的inode
    struct inode *tind = malloc(sizeof(struct inode));
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    get_info_by_path(path, tind, tfd);
    // 处理写入的块号，在写入块中的下标，在buf中的下标，偏移量
    int blk_no = offset / MAX_DATA_IN_BLOCK;   // 在inode中的块号
    int p_in_blk = offset % MAX_DATA_IN_BLOCK; // 在当前块中的位置
    off_t write_off = offset;                  // 要写入的位置的偏移
    char *buf_index = buf;                     // buf的下标

    printf("将buf的内容:%s,写入inode号为:%d的偏移%ld \n", buf, tind->st_ino, offset);
    size_t res = size;
    size_t t_size = size;
    // 判断文件是否超出
    if (res + offset > tind->st_size)
    {
        // 文件大小超出
        // 但是仍需追加，不直接返回
        res = -EFBIG;
    }

    struct data_block *tblk = malloc(sizeof(struct data_block));
    // 利用while循环不断读入数据块
    // 根据size和p_in_blk写入当前的块
    while (t_size > 0)
    {
        read_block_by_ind_and_indNo(tind, blk_no, tblk);
        // 判断一下写入的文件大小和当前块剩余的大小
        int max_write = (MAX_DATA_IN_BLOCK - p_in_blk) < t_size ? (MAX_DATA_IN_BLOCK - p_in_blk) : t_size; // 剩余可写入的数据大小

        // 进行写入的操作
        memcpy(tblk->data + p_in_blk, buf_index, max_write); // 从tblk要写入的位置，写入max_write数据大小
        // 更新该文件的ind信息
        tind->st_size = tind->st_size > (write_off + max_write) ? tind->st_size : (write_off + max_write);
        // 把该更新的inode写回磁盘
        write_block_by_ind_and_indNo(tind, blk_no, tblk);
        // 对下标和大小进行更新
        write_off += max_write; // 当前偏移量增加写的大小
        buf_index += max_write; // buf偏移量增加写的大小
        t_size -= max_write;    // 要写入的总数减少
        blk_no++;               // 读取下一个块的块号
        p_in_blk = 0;           // 从数据块起始位置开始
    }

    // 把更新后的ind信息写回磁盘
    write_inode_by_no(tind, tind->st_ino);

    // 释放内存
    free(tind);
    free(tfd);
    free(tblk);

    return res;
}
#pragma endregion

// 功能函数的定义
// 根据传入的文件路径path找到对应的fd然后传给attr
int get_fd_to_attr(const char *path, struct file_directory *attr)
{
    printf("get_fd_to_attr函数运行!\n\n");
    printf("查询的路径为%s\n\n", path);
    // 先读取超级块
    // 获得根目录在数据区的位置（根据块数）
    struct sb *super_blk;
    struct data_block *data_blk;

    data_blk = malloc(sizeof(struct data_block));
    // 利用read_block_by_no函数读取超级块
    // 超级块的块号为0，刚好直接查询并且读到创建的data_blk中
    int tmp = read_block_by_no(data_blk, 0);
    // 根据查询的结果来判断是否读取成功
    if (tmp == -1)
    {
        printf("读取超级块失败!\n\n");
        free(data_blk);
        return -1;
    }
    // 读取成功
    super_blk = (struct sb *)data_blk;
    // 检查路径
    // 如果路径为空，则出错返回1
    char *tmp_path = strdup(path);
    if (!tmp_path)
    {
        printf("错误：get_fd_to_attr：路径为空，函数结束返回\n\n");
        free(super_blk);
        return -1;
    }
    // 根据路径来寻找对应的inode和fd文件
    // 创建临时的inode和file_directory来接收
    struct inode *tinode = malloc(sizeof(struct inode));
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    // 通过get_info_by_path函数找到路径对应文件的inode和file_directory，并且读取到临时的文件当中
    int flag = get_info_by_path(path, tinode, tfd);
    // 根据返回值判断是否读取成功
    if (flag != 0)
    {
        // 说明读取出现错误
        printf("读取对应的inode时出现问题,错误代码为%d!\n", flag);
        return flag;
    }
    // 读取成功，把fd的内容赋值给attr返回
    strcpy(attr->fname, tfd->fname);
    attr->st_ino = tfd->st_ino;
    // strcpy(attr->st_ino,tfd->st_ino);
    strcpy(attr->fext, tfd->fext);
    strcpy(attr->standby, tfd->standby);
    // 释放内存
    free(data_blk);
    free(tinode);
    free(tfd);
    return 0;
}

// 该函数根据输入文件的路径找到对应的inode和fd
int get_info_by_path(const char *path, struct inode *ind, struct file_directory *fd)
{
    // 查找路径path只会显示文件系统下的路径
    // 意思是把文件系统挂载的路径当成根目录
    printf("get_info_by_path函数开始执行,文件的路径为%s\n\n", path);
    // 对根目录的判断
    if (strcmp(path, "/") == 0)
    {
        // 为根目录
        printf("get_info_by_path：输入的路径为根目录：%s,函数已经执行\n", path);
        // 根目录的inode号为0
        read_inode_by_no(ind, 0);
        // 设置一下根目录的文件返回
        strcpy(fd->fname, path);
        fd->st_ino = 0;
        return 0;
    }
    // 不是根目录
    // 文件查找需要一层一层查找
    // 先拿到根目录的inode（inode号为0）
    read_inode_by_no(ind, 0);
    printf("拿到根目录inode，大小为:%d\n",ind->st_size);
    // 开始对路径进行分析
    char *tmp_path = NULL, *next_level = NULL, *cur_level = NULL, *next_ext = NULL; // tmp_path用于临时记录路径
    tmp_path = strdup(path);
    tmp_path++;            // 去掉最前面的一个"/"
    next_level = tmp_path; // 先把下级目录指向根目录下的第一个目录文件
    int flag = 0;          // 是否找到文件 的表示
    int layer_level = 0;   // 搜索的层级数
                           //  逐层检查(利用while循环)
    while (tmp_path != NULL)
    {
        cur_level = next_level;
        next_level = strchr(next_level, '/'); // 检查是否存在下级目录
        next_ext = '\0';
        if (next_level == NULL)
        {
            // 此时已到达输入路径的最底层
            // cur_level就为最终要找的文件
            flag = 1;
            // cur_level = strdup(tmp_path); //此时cur_level为目录的名字
            //  temp_path = strchr(temp_path, '/');
        }
        else
        {
            // 还存在下一级目录
            // 首先分割下级目录和当前目录
            *next_level = '\0';
            // 把指针指向下级目录起始位置
            next_level++;
            // tmp_path = strchr(tmp_path, "/");
            // char *tmp_next_ext = cur_level;create_new_fd_to_inode
            // tmp_next_ext = strchr(cur_level, "/");
            // // 分割当前目录和下级目录
            // if (tmp_next_ext)
            //     *tmp_next_ext = '\0';
        }
        // 搜索的层数+1
        layer_level++;
        // 要对当前cur_level指向的fd进行判断，是文件还是目录
        // 只有目录才能进行下一层的遍历
        if (determineFileType(ind) == 2 && flag != 1)
        {
            // 如果当前指向的fd是文件，并且没有还有下一级
            // 说明无法继续遍历，直接返回
            printf("路径中存在不可继续遍历的文件!路径无效\n");
            return -ENOENT;
        }
        // 已经遍历到最底层
        if (flag)
        {
            // 如果已经找到该文件的目录
            // 对文件及其后缀名进行分割
            char *tmp_dot = strchr(cur_level,'.');
            
            if (tmp_dot != NULL)
            {
                *tmp_dot = '\0'; // 把文件名和后缀名分开成两个串
                // 此时cur_level单独指向文件名
                tmp_dot++;
                next_ext = tmp_dot;
                printf("已找到文件！文件名为:%s,拓展名为%s\n", cur_level, next_ext);
            }
            else
            {
                printf("已找到文件！文件名为:%s,无拓展名!\n", cur_level);
            }
        }
        int isFindInode = 0; // 是否寻找到当前层级下curlevel的inode
        // 下面开始通过cur_level的目录名和inode，查找该inode下是否有该cur_level
        // 若找到，则把对应目录或者文件的fd返回到传入的buff:fd中
        isFindInode = is_inode_exists_fd_by_fname(ind, cur_level, fd, 0, 0);
        if (isFindInode == 1)
        {
            // 存在cur_level这个目录或者文件
            // 获取fd对应的inode，更新当前的ind,作为下一层的遍历
            read_inode_by_fd(ind, fd);
        }
        else
        {
            // 在inode里面找不到cur_level的fd
            // 直接返回
            printf("inode下没有对应名为:%s的文件或目录!\n",cur_level);
            return -ENOENT;
        }

        if (flag != 1 && layer_level < 4)
        {
            printf("当前目录层级为:%d,将从当前目录:%s 的子目录%s 继续寻找!\n", layer_level, cur_level, next_level);
        }
        else
        {
            printf("文件已找到或超出层级限制!\n");
            break;
        }
    }
    // 此时进行判断退出while循环的原因
    // 是为层级限制还是文件已找到
    if (layer_level >= 4)
    {
        printf("超出层级限制无法继续查找文件!\n");
        return -EPERM;
    }
    // 此时退出的情况为已找到文件
    printf("文件已经找到!\n");
    return 0;
}

// 该函数用于分割路径得到父目录和要创建的文件名或目录名
int get_parent_and_fname(const char *path, char **parent_path, char **fname)
{   
    printf("get_parent_and_fname函数被调用!输入的路径为: %s\n",path);
    char *tmp_path = strdup(path);
    // 利用strrchr移动到最后一级（文件名或目录名）来取得信息
    // 首先判断path是不是根目录
    *parent_path = tmp_path; // 记录父目录
    tmp_path++;                      // 跳过第一个"/"
    *fname = strrchr(tmp_path, '/'); // 把fname移动到最后一级的/上面
    if (*fname != NULL)
    {
        // 创建的文件不在根目录下
        **fname = '\0'; // 分割父目录和文件名
        (*fname)++;     // 指针移动到文件名上
    }
    else
    {
        // 路径指示文件在根目录下
        *fname = tmp_path; // 此时tmppath就是文件名
        *parent_path = strdup("/");//直接设置父目录为根目录
    }
    printf("父目录为:%s,创建的文件名或目录名为:%s\n", *parent_path, *fname);
    return 0;
}

// 该函数创建path所指文件或目录的fd
// 并且创建空闲数据块
// flag为2代表创建目录，1为代表创建文件
int create_file_dir(const char *path, int flag)
{ // 获得父目录和目录名
    printf("create_file_dir函数被调用！\n");
    char *fname = NULL, *parent_path = NULL;
    get_parent_and_fname(path, &parent_path, &fname);
    // 根据父目录来获取父目录的inode和fd
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    struct inode *tinode = malloc(sizeof(struct inode));
    int tmp1 = get_info_by_path(parent_path, tinode, tfd);
    printf("create_file_dir:创建成功前，父目录inode号为:%d,大小为%d\n",tinode->st_ino,tinode->st_size);
    // 判断文件名是否过长
    int tooLong = 0;
    int name_len = strlen(fname);        // 文件名长度
    char *exp_name = strchr(fname, '.'); // 判断是否有拓展名
    int exp_len = 0;
    if (!exp_name)
    {
        // 没有拓展名
        if (name_len > 8)
            tooLong = 1;
    }
    else
    {
        // 有拓展名
        if (exp_name - fname > 8)
            tooLong = 1;
        else
        {
            exp_name++;
            exp_len = strlen(exp_name);
            if (exp_len > 3)
                tooLong = 1;
        }
    }
    if (tooLong)
    {
        printf("文件名:%s 或拓展名过长!文件名长度为:%d,拓展名长度为:%d超出规定字节!函数终止", fname, name_len, exp_len);
        return -ENAMETOOLONG;
    }
    // 要对此时父目录的inode判断，是否为一个目录而不是文件
    if (determineFileType(tinode) != 1)
    {   
        // 不为目录
        free(tfd);
        free(tinode);
        return -ENOENT;
    }
    // 调用函数，对父目录的inode中加入新创建的目录
    
    int res = create_new_fd_to_inode(tinode, fname, flag);
    printf("create_file_dir:创建成功后，父目录inode号为:%d,大小为%d\n",tinode->st_ino,tinode->st_size);
    // 创建成功后返回
    free(tfd);
    free(tinode);
    return res;
}

// 该函数根据给定的inode，删除对应名称的目录
// 由rmdir和unlink同时调用
int remove_file_dir(struct inode *ind, const char *filename, int flag)
{
    struct inode *tind = malloc(sizeof(struct inode));
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    // 要找到父目录inode里面，对应名称的目录在inode的第几个块，在那个块里面的偏移量
    int blk_no = 0;
    int offsize = 0;
    // 首先判断删除的目录项是否存在
    if (!is_inode_exists_fd_by_fname(ind, filename, tfd, &blk_no, &offsize))
    {
        free(tfd);
        free(tind);
        return -ENOENT;
    }
    // 获取要删除的文件的indoe
    read_inode_by_fd(tind, tfd);
    // 判断要删除文件的类型
    // 此处由于该函数被多个函数调用，所以做一个if分支
    if (flag == 2)
    {
        // 由rmdir调用
        // 判断空目录和是否是目录
        if (tind->st_size != 0)
        {
            // 不是空目录
            free(tind);
            free(tfd);
            return -ENOTEMPTY;
        }
        // 判断是否是目录
        if (determineFileType(tind) != 1)
        {
            // 不是目录
            free(tind);
            free(tfd);
            return -ENOTDIR;
        }
    }
    else
    {
        // 由unlink调用
        // 判断是否为文件
        if (determineFileType(tind) != 2)
        {
            // 不是文件（是目录）
            free(tind);
            free(tfd);
            return -EISDIR;
        }
    }

    // 在父目录inode，删除在其中的目录项信息
    int end_blk = ind->st_size / MAX_DATA_IN_BLOCK; // 最后一块的块号
    int end_offsize = ((ind->st_size - 1) % MAX_DATA_IN_BLOCK) - sizeof(struct file_directory) + 1;
    end_offsize /= sizeof(struct file_directory); // 最后一个fd在最后一块的偏移量

    // 对父目录inode里面进行相关信息的删除
    // 设置一个临时的块进行读取后修改写入
    struct data_block *tblk = malloc(sizeof(struct data_block));
    // 读取最后一个块
    read_block_by_ind_and_indNo(ind, end_blk, tblk);
    // 利用偏移量读取最后一个块里面的最后一个fd
    memcpy(tfd, (struct file_directory *)tblk + end_offsize, sizeof(struct file_directory));
    // 读取要删除的fd项所在的块
    read_block_by_ind_and_indNo(ind, blk_no, tblk);
    // 将末尾的数据块覆盖原有的空目录
    memcpy((struct file_directory *)tblk + offsize, tfd, sizeof(struct file_directory));
    // 再把该数据块写回
    write_block_by_ind_and_indNo(ind, blk_no, tblk);
    // 空目录被覆盖（删除）后，释放其对应的ind与占用的数据块
    clear_inode_map_by_no(tind->st_ino);
    free(tind);
    // 修改父目录inode的大小信息（少了16B）
    ind->st_size -= sizeof(struct file_directory);
    // 再把父目录的ind写回
    write_inode_by_no(ind, ind->st_ino);

    // 释放申请的内存
    free(tblk);
    free(tfd);
    return 0;
}
// 该函数用于往已知的inode里面添加新建的目录项
int create_new_fd_to_inode(struct inode *ind, const char *fname, int flag)
{   
    // 为新建的目录项分配空间
    struct file_directory *fd = malloc(sizeof(struct file_directory));
    // TODO:对是否存在目录项进行判断
    if (is_inode_exists_fd_by_fname(ind, fname, fd, NULL, NULL))
    {   //  printf("存在！\n");
        free(fd);
        return -EEXIST;
    }

    // 对该inode中添加目录find_ino_dir
    struct inode *cr_ind = malloc(sizeof(struct inode));
    struct data_block *cr_blk = malloc(sizeof(struct data_block));
    // 设置创建的目录的inode信息
    // 由于此函数被mkdir和mknod同时调用
    // 所以要进行if判断
    if (flag == 2)
    {
        // 创建的是目录文件
        cr_ind->st_mode = (0766 | S_IFDIR);
    }
    else if (flag == 1)
    {
        // 创建的是文件
        cr_ind->st_mode = (0666 | S_IFREG);
    }
    // 下面为新创建的inode信息进行初始化
    cr_ind->st_ino = assign_inode();  // 分配新的inode号
    cr_ind->addr[0] = assign_block(); // 分配新的数据块
    cr_ind->st_size = 0;
    // 设置目录项信息
    char *tname = strdup(fname);     // 文件名
    char *text = strchr(tname, '.'); // 拓展名的分隔符的指针
    if (text == NULL)
    {   printf("准备创建文件:%s\n",tname);
        // 没有拓展名
        // 直接创建
        create_dir_by_ino_number(tname, "", cr_ind->st_ino, "", fd);
    }
    else
    {   printf("准备创建文件:%s\n",tname);
        // 有拓展名，需要分割一下文件名和拓展名
        *text = '\0'; // 分割
        text++;       // 移动到拓展名位置
        create_dir_by_ino_number(tname, text, cr_ind->st_ino, "", fd);
    }
    fd->st_ino = cr_ind->st_ino;
    printf("创建fd成功，名为:%s,inode号为:%d\n",fd->fname,fd->st_ino);
    // 寻找父目录对应的数据块的末尾块块号(ind相对块号)
    short int end_blk = ind->st_size / MAX_DATA_IN_BLOCK;
    printf("要放入的快号为%d\n",end_blk);
    // 首先判断父目录的末尾数据块是否装满
    // 否则需要重新申请一个数据块来放该新建的目录
    if (ind->st_size % MAX_DATA_IN_BLOCK == 0  && ind->st_size > 0)
    {   printf("需要新增数据块来存放该dir\n");
        // 该数据块正好被用完
        // 要新增一个数据块来装目录的fd
        end_blk++;                                         // 块号增加
        read_block_by_ind_and_indNo(ind, end_blk, cr_blk); // 根据最后一块块号和inode来获取对应的最后一块blk
        // 将上面设置好的新创建的fd复制到cr_blk
        memcpy(cr_blk->data, fd, sizeof(struct file_directory));
    }
    else
    {   printf("准备将新建的dir放入!\n");
        // 最后一个数据块未装满
        // 可以把新建的fd放进去
        read_block_by_ind_and_indNo(ind, end_blk, cr_blk); // 根据最后一块块号和inode来获取对应的最后一块blk
        // 需要知道在最后一个块要写入的位置
        short int lastIndex = ind->st_size % MAX_DATA_IN_BLOCK;
        printf("lastIndex:%d\n",lastIndex);
        // 把对应的fd写到cr_blk中
        memcpy(&(cr_blk->data[lastIndex]), fd, sizeof(struct file_directory));
    }
    printf("rootInodesize:%d\n",ind->st_size);
    // 新增的fd的inode的大小（一个）
    ind->st_size += sizeof(struct file_directory);
    //把修改后的inode写回
    write_inode_by_no(ind,ind->st_ino);
    // 最后就把新建的cr_blk写回磁盘
    write_block_by_ind_and_indNo(ind, end_blk, cr_blk);


    {
        //看看有没有写进去
        read_block_by_ind_and_indNo(ind, end_blk, cr_blk); // 根据最后一块块号和inode来获取对应的最后一块blk
        // 需要知道在最后一个块要写入的位置
        short int lastIndex = strlen(cr_blk->data) % MAX_DATA_IN_BLOCK;
        printf("lastIndex:%d\n",lastIndex);

        struct inode *tid = malloc(sizeof(struct inode));
        read_inode_by_no(tid,0);
        printf("rootInodesize:%d\n",tid->st_size);
        free(tid);
    }
    printf("新建的inode号:%d,对应存放的地址：%d\n",cr_ind->st_ino,cr_ind->addr[0]);
    // 把新建cr_blk对应的fd的inode也写回磁盘
    write_inode_by_no(cr_ind, cr_ind->st_ino);

    {   struct inode *tid1 = malloc(sizeof(struct inode));
        //看看有没有写进去
        read_inode_by_no(tid1,cr_ind->st_ino);
        printf("检验新建的inode号:%d,对应存放的地址：%d\n",tid1->st_ino,tid1->addr[0]);
         printf("newInodesize:%d\n",tid1->st_size);
        // free(tid);

        // struct inode *tid = malloc(sizeof(struct inode));
        read_inode_by_no(tid1,0);
        printf("rootInodesize:%d\n",tid1->st_size);
        free(tid1);   
    }
    // 释放空间
    free(cr_blk);
    free(cr_ind);
    free(fd);
    free(tname);

    return 0;
}

// 该函数用于直接创建一个dir并初始化内容
int create_dir_by_ino_number(char *fname, char *ext, short int ino_no, char *exp, struct file_directory *fd)
{   
    
    // 分别对传入的信息进行写入
    strncpy(fd->fname, fname, 8);
    strncpy(fd->fext, ext, 3);
    fd->st_ino = ino_no;
    strncpy(fd->standby, exp, 3);
    return 0;
}

// 该函数用于根据inode号,把该inode对应的文件的位图和其本身的位图置为0
int clear_inode_map_by_no(const short st_ino)
{
    // 将对应inode文件的位图置0
    // 首先要找到该inode
    struct inode *ind = malloc(sizeof(struct inode));
    read_inode_by_no(ind, st_ino);
    // 然后计算出该inode对应的文件占用了多少数据块
    int blk_num = (ind->st_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    // 分别对这些数据块的位图置0
    // 利用for循环对数据块逐一设置位图
    for (int i = 0; i < blk_num; i++)
    {
        // 首先找到数据块的绝对块号
        int a_blk_num = get_blk_no_by_indNo(ind, i);
        // 然后根据绝对块号来计算数据块在数据位图的位置
        int blkmap_index = a_blk_num / (BLOCK_SIZE * 8);
        int offsize = a_blk_num % (BLOCK_SIZE * 8); // 以bit为单位在位图快中的偏移位置
        // 读取该文件数据块在数据位图区的那个数据块
        struct data_block *blk = malloc(sizeof(struct data_block));
        read_block_by_no(blk, DATA_BITMAP_START_NUM + blkmap_index);
        // 找到要修改的对应的byte
        // 此处先把offsize转换成byte单位来索引到对应的字
        char *bit = &(blk->data[offsize / 8]);
        // 再找到byte中要修改的位
        offsize %= 8; // 再转化成bit
        // 进行置0的左移操作
        *bit &= ~(1 << offsize);
        // 把修改后的blk重新写回磁盘
        write_block_by_no(blk, DATA_BITMAP_START_NUM + blkmap_index);

        return 0;
    }
}

// 该函数用于获取一级索引块中的直接索引地址,并且返回块号
// addr为传入的索引块的地址，offset为在该块中的块号，blk_offset为该块的最大块号
int fir_index(short int addr, int offset, short int blk_offset)
{
    // 具体实现其实和sec_index实现的方法差不多
    // 设置要返回的块号
    short int direct_no = -1;
    //  该数据块属于一级索引下的数据块
    // 读取存储一级索引的数据块
    struct data_block *blk = malloc(sizeof(struct data_block));
    read_block_by_no(blk, addr);
    // 该数据块存储的就是所有的直接索引的地址
    // 找出目标数据块在哪个直接索引块（块号）
    if (offset > blk_offset)
    {
        // 此时由于传入的在该块中的目标块号offset超过了该块能存储的最大块号blk_offset
        // 重新分配一个块给offset
        *(short int *)(&(blk->data[offset * 2])) = assign_block(); // offset*2是因为一个块号2Byte
        write_block_by_no(blk, addr);
    }
    // 读取该索引块中offset的块号信息
    direct_no = *(short int *)(&(blk->data[offset * 2]));
    free(blk);
    // 返回块号
    return direct_no;
}

// 该函数用于获取二级索引块中的一级索引地址
int sec_index(short int addr, int offset, short int blk_offset)
{
    // 具体实现操作思路和三级索引片段代码差不多
    // 只需修改一下获取地址的逻辑数即可
    // 该数据块属于二级索引下的数据块
    // 读取存储二级索引的数据块
    struct data_block *blk = malloc(sizeof(struct data_block));
    read_block_by_no(blk, addr);
    // 该数据块存储的就是所有的一级索引的地址
    // 找出目标数据块在哪个一级索引块（块号），并且在该块的偏移位置

    int fir_blk_no = offset / (SINGLE_BLOCK_STORE_NO_NUMS);
    int fir_blk_offset = offset % (SINGLE_BLOCK_STORE_NO_NUMS);

    int max_fir_blk_num = -1;   // 表示二级索引块中的最大索引块数（一级块的数量）
    int max_fir_blk_index = -1; // 表示二级块中的最后的那个一级块中，所对应的最后一个索引
    // 通过对blk_offset的大小判断，该二级索引块是否为空
    if (blk_offset >= 0)
    {
        // 更新此时的二级索引块中的内容信息
        max_fir_blk_num = blk_offset / (SINGLE_BLOCK_STORE_NO_NUMS);
        max_fir_blk_index = blk_offset % (SINGLE_BLOCK_STORE_NO_NUMS);
    }
    // 若此时上面求出的一级索引块号（目标）是超出了二级块中最大索引块号
    if (fir_blk_no > max_fir_blk_num)
    {
        // 重新分配一级索引
        // fir_blk_no*2是因为块号为short类型2Byte，但是data是char*，里面存储的是char 1Byte，所以要*2
        *(short int *)(&(blk->data[fir_blk_no * 2])) = assign_block();
        // 此时最后一个一级块已经创建，但是里面没东西
        // 所以新创建的最后一个一级块的最后一个索引为-1（表示不存在咯）
        max_fir_blk_index = -1;
    }
    // 此时获得二级块里面的该一级块的地址
    short int fir_index_addr = *(short int *)(&(blk->data[fir_blk_no * 2]));

    free(blk);
    // 利用一级索引函数来进行查找
    return fir_index(fir_index_addr, fir_blk_offset, max_fir_blk_index);
}

// 根据inode对应的文件的数据块的相对块号，来获取对应数据块的绝对块号
int get_blk_no_by_indNo(struct inode *ind, const short indNo)
{
    // 首先获得当前inode对应的文件占用多少个块
    int block_nums = ind->st_size / MAX_DATA_IN_BLOCK;
    // 判断此时需要分配新的块(索引块是否分配)
    if (indNo > block_nums)
    {
        if (indNo == FIRST_INDEX_NUMS)
            ind->addr[4] = assign_block(); // 一级索引
        if (indNo == SECONDARY_INDEX_NUMS)
            ind->addr[5] = assign_block(); // 二级索引
        if (indNo == TRIPLE_INDEX_NUMS)
            ind->addr[6] = assign_block(); // 三级索引
    }
    int number = -1;
    // 对索引进行判断
    // 三级索引
    if (indNo >= TRIPLE_INDEX_NUMS)
    {
        // 找出目标数据块在哪个二级索引中
        int offset = indNo - TRIPLE_INDEX_NUMS;          // 该目标文件的数据块在三级索引块的范围内的偏移（三级块内的第几块）
        int blk_offset = block_nums - TRIPLE_INDEX_NUMS; // 当前inode对应文件在三级索引块多出来的偏移量
        // 该数据块属于三级索引下的数据块
        // 读取存储三级索引的数据块
        struct data_block *blk = malloc(sizeof(struct data_block));
        read_block_by_no(blk, ind->addr[6]);
        // 该数据块存储的就是所有的二级索引的地址
        // 找出目标数据块在哪个二级索引块（块号），并且在该块的偏移位置

        int sec_blk_no = offset / (SINGLE_BLOCK_STORE_NO_NUMS * SINGLE_BLOCK_STORE_NO_NUMS);
        int sec_blk_offset = offset % (SINGLE_BLOCK_STORE_NO_NUMS * SINGLE_BLOCK_STORE_NO_NUMS);

        int max_sec_blk_num = -1;   // 表示三级索引块中的最大索引块数（二级块的数量）
        int max_sec_blk_index = -1; // 表示三级块中的最后的那个二级块中，所对应的最后一个索引
        // 通过对blk_offset的大小判断，该三级索引块是否为空
        if (blk_offset >= 0)
        {
            // 更新此时的三级索引块中的内容信息
            max_sec_blk_num = blk_offset / (SINGLE_BLOCK_STORE_NO_NUMS * SINGLE_BLOCK_STORE_NO_NUMS);
            max_sec_blk_index = blk_offset % (SINGLE_BLOCK_STORE_NO_NUMS * SINGLE_BLOCK_STORE_NO_NUMS);
        }
        // 若此时上面求出的二级索引块号（目标）是超出了三级块中最大索引块号
        if (sec_blk_no > max_sec_blk_num)
        {
            // 重新分配二级索引
            // sec_blk_no*2是因为块号为short类型2Byte，但是data是char*，里面存储的是char 1Byte，所以要*2
            *(short int *)(&(blk->data[sec_blk_no * 2])) = assign_block();
            // 此时最后一个二级块已经创建，但是里面没东西
            // 所以新创建的最后一个二级块的最后一个索引为-1（表示不存在咯）
            max_sec_blk_index = -1;
        }
        // 此时获得三级块里面的该二级块的地址
        short int sec_index_addr = *(short int *)(&(blk->data[sec_blk_no * 2]));

        free(blk);
        // 利用二级索引函数来进行查找
        number = sec_index(sec_index_addr, sec_blk_offset, max_sec_blk_index);
    }
    // 二级索引
    else if (indNo >= SECONDARY_INDEX_NUMS)
    {
        int offset = indNo - SECONDARY_INDEX_NUMS;          // 该目标文件的数据块在二级索引块的范围内的偏移（二级块内的第几块）
        int blk_offset = block_nums - SECONDARY_INDEX_NUMS; // 当前inode对应文件在二级索引块多出来的偏移量
        number = sec_index(ind->addr[5], offset, blk_offset);
    }
    // 一级索引
    else if (indNo >= FIRST_INDEX_NUMS)
    {
        int offset = indNo - FIRST_INDEX_NUMS;          // 该目标文件的数据块在一级索引块的范围内的偏移（一级块内的第几块）
        int blk_offset = block_nums - FIRST_INDEX_NUMS; // 当前inode对应文件在一级索引块多出来的偏移量
        number = sec_index(ind->addr[4], offset, blk_offset);
    }
    // 直接索引
    else
    {
        // 若要找的该数据块的块号超出了总的块数
        if (indNo > block_nums)
        {
            // 分配新的块来存放
            ind->addr[indNo] = assign_block();
        }
        // 读取该数据块的块号(在存储在inode的直接索引里面找)
        number = ind->addr[indNo];
    }
    return DATA_BLOCK_START_NUM + number;
}

// 该函数用于为inode里面新建的目录项创建对应的inode
// 在inode位图块中寻找为0的位，置1，计算其inode号返回
short assign_inode()
{
    // 返回值是inode号
    short ind_no = 0;
    // 读取inode位图区数据块
    struct data_block *blk = malloc(sizeof(struct data_block));
    read_block_by_no(blk, INODE_BITMAP_START_NUM);
    // 对inode位图块的512B进行逐Byte的遍历(利用char*)
    unsigned char *p = blk->data;
    for (short i = 0; i < BLOCK_SIZE; i++)
    {
        // 此处做法和分配数据块函数一样
        // 判断该字节的8位是否为全1(0xFF)(是否已全部分配)
        if (*p != 0xFF)
        {
            // 有未分配的inode
            // 拿出来分配给新建的目录
            int tmp = zerobit_no(p);
            // 把该0位置1
            *p |= 0x80 >> tmp;
            // 把修改后的inode位图块写回磁盘
            write_block_by_no(blk, INODE_BITMAP_START_NUM);
            free(blk);
            return ind_no + tmp;
        }
        // 若为全1,则跳过该字节
        p++;
        ind_no += 8;
    }
    // 未找到空闲的inode
    free(blk);
    return -1;
}

// 该函数用于分配一个新的块用作(块号超出的存放)
// 返回新分配的块的块号
short assign_block()
{
    // 要返回的数据块号
    short blk_no = 0;
    // 数据位图区由4个块组成
    // 利用for循环依次读取每一个块
    struct data_block *blk = malloc(sizeof(struct data_block));
    for (short i = 0; i < 4; i++)
    {
        // 对数据位图的块进行读取
        read_block_by_no(blk, DATA_BITMAP_START_NUM + i);
        // 对读取的数据位图块进行逐字节的读取
        unsigned char *p = blk->data;
        // 利用for循环对该块的每一个字节进行遍历
        for (int j = 0; j < BLOCK_SIZE; j++)
        {
            if (*p != 0xFF)
            {
                // 如果该字节出现某个位没有置1的情况
                // 说明有空闲数据块可以分配
                // 找到该字节第一个0位的编号
                int tmp = zerobit_no(p);
                // 对该位置的bit置1
                *p |= 0x80 >> tmp;
                // 再把修改后的blk重新写回磁盘
                write_block_by_no(blk, DATA_BITMAP_START_NUM + i);
                blk_no += tmp; // 获得此时的块号
                free(blk);
                return blk_no;
            }
            // 如果该字节内的位全部被置1(0xFF)
            // 说明已经没有空闲的数据块
            p++;         // 此时指针偏移到下一个字节
            blk_no += 8; // 结果块号移动一个字节的大小
        }
    }
    // 没找到空闲的数据块
    free(blk);
    return -1;
}

// 该函数获取一个无符号字符（unsigned char）中最高位（即最左边）为0的位是第几位
int zerobit_no(unsigned char *p)
{

    int res = 0;
    unsigned char mask = 0x80; // 0x80表示最高位为1，其余位为0

    while (mask & *p)
    {
        mask >>= 1; // 右移一位，检查下一位
        res++;
    }

    return res;
}

// 该函数获取传入的fd的filename
int get_fd_name(struct file_directory *fd, char *fname)
{
}

// 辅助函数的定义

// 该函数根据输入的fname和inode，来判断该inode对应的目录中是否有名为fname的目录项
int is_inode_exists_fd_by_fname(struct inode *ind, char *fname, struct file_directory *fd, int *blk_no, int *offsize)
{   printf("is_inode_exists_fd_by_fname:传入的inode大小为:%d\n",ind->st_size);
    // 首先判断ind对应的是否是目录
    if (determineFileType(ind) != 1)
    {
        return -ENOTDIR;
    }
    // 利用for循环对ind目录项下每一块数据块中装着的每个目录进行遍历
    int total_blk = (ind->st_size + MAX_DATA_IN_BLOCK - 1) / MAX_DATA_IN_BLOCK; // 该inode下对应所有数据块数
    total_blk = total_blk < 1 ? 1 : total_blk;
    printf("一个file大小为:%d\n",sizeof(struct file_directory));
    printf("一个inode大小为:%d\n",sizeof(struct inode));
    int total_fd = (ind->st_size) / FILE_DIRECTORY_SIZE; // 表示所有目录的数量
    printf("is_inode_exists_fd_by_fname:总共的fd:%d\n",total_fd);
    printf("is_inode_exists_fd_by_fname:总共的块数:%d\n",total_blk);
    // 此时可能出现一个数据块就装完了所有fd
    // 要进行判断
    total_fd = total_fd < MAX_DIR_IN_BLOCK ? total_fd : MAX_DIR_IN_BLOCK;
    struct data_block *blk = malloc(sizeof(struct data_block)); // 用于一块块的读取数据块
    struct file_directory *tfd = NULL;                          // 用于指向当前的fd
    // cur1用于记录当前指向的fd的编号，用于第二层for循环的不断记录
    // 直到读完所有total_fd的数量
    // 所以要一直记录，定义在for外面
    int cur1 = 0;
    for (int cur = 0; cur < total_blk; cur++)
    {   
        // 循环读取inode下对应的所有数据块
        read_block_by_ind_and_indNo(ind, cur, blk);
        // fd指针指向当前读取数据块第一块

        tfd = (struct file_directory *)blk->data;
        
        for (; cur1 < total_fd && (cur1 + 1) % (512/16) != 0; cur1++)
        {  
            // 循环条件为：cur1表示的fd编号不超过总数；cur1循环次数不超过一个块里最多能装的fd个数
            // 如果上一级for循环中fd指向目录的名字和fname相同，则创建的目录已存在！
            int flag = is_same_fd(tfd, fname);
            printf("flag:%d\n",flag);
            if (flag)
            {   
                // 把当前查到的tfd复制到传入的fd
                memcpy(fd, tfd, sizeof(struct file_directory));
                free(blk);
                // 返回匹配项的对应的所在块号和偏移量
                if (blk_no)
                    *blk_no = cur;
                if (offsize)
                    *offsize = cur1;
                return 1;
            }
            tfd++; // 遍历该数据块的其他fd
        }
        cur1++; // 指向下个数据块的第一个fd
    }
    // 没有已存在的
    free(blk);
    return 0;
}

// 判断传入的fd和fname是否一致
int is_same_fd(struct file_directory *fd, const char *fname)
{   
    char name[MAX_FILENAME + MAX_EXTENSION + 2];
    // 拼接fd的文件名
    strncpy(name, fd->fname, 8);
    // 检查拓展名字段是否为空
    if (strlen(fd->fext) != 0)
    {
        strcat(name, ".");
    }
    strncat(name, fd->fext, 3);
    printf("is_same_fd:根据路径传入的文件名为:%s,遍历数据块的当前的fd的名字为:%s\n",fname,name);
    if (strcmp(name, fname) == 0)
        return 1;
    return 0;
}

// 函数用于判断inode对应的是文件还是目录
int determineFileType(const struct inode *myInode)
{   
    if (IS_DIRECTORY(myInode->st_mode))
    {   
        return 1; // 表示目录
    }
    else if (IS_REGULAR_FILE(myInode->st_mode))
    {   
        return 2; // 表示文件
    }
    else
    {   
        return 0; // 未知类型
    }
}

// 辅助函数的实现
#pragma region

// 该函数用于根据数据块号读取对应的数据块
int read_block_by_no(struct data_block *dataB_blk, short no)
{
    FILE *fp = NULL;
    fp = fopen(disk_path, "r+");
    if (fp == NULL)
    {
        printf("open file failed:file is not existed!\n");
        return -1;
    }
    // 根据数据块号移动fp指针到对应位置
    fseek(fp, no * BLOCK_SIZE, SEEK_SET);
    // 开始读取一个block大小的数据到指定的block buf里面
    fread(dataB_blk, sizeof(struct data_block), 1, fp);

    fclose(fp);
    return 0;
}

// 该函数用于根据inode号读取对应的inode
int read_inode_by_no(struct inode *ind, short no)
{
    // 首先找到该inode存在于哪一个快
    // 再找出该inode在该块中的哪一个位置
    int ind_in_blk_no = no % INODE_NUMS_IN_BLOCK;
    int blk_no = INODE_BLOCK_START_NUM + no / INODE_NUMS_IN_BLOCK;
    printf("read_inode_by_no:此时计算出要读入的inode号为%d,inode所在的块号为%d\n",no,blk_no);
    printf("read_inode_by_no:该inode所在块中的位置为%d\n",ind_in_blk_no);
    // 找到inode所在数据块，先将其读取出来
    struct data_block *blk = (struct data_block *)malloc(sizeof(struct data_block));
    read_block_by_no(blk, blk_no);
    
    // 然后再根据inode号和求出的块中位置找到这个inode，拷贝到ind的buf里面
    memcpy(ind, ((struct inode *)blk) + ind_in_blk_no, sizeof(struct inode));
    printf("read_inode_by_no:检验:此时读入的inode号为%d,inode大小为%d,连接数量为:%d\n",ind->st_ino,ind->st_size,ind->st_nlink);
    // 释放申请的空间
    free(blk);
    return 0;
}

// 该函数用于根据给定的fd来读取对应的inode
int read_inode_by_fd(struct inode *ind, struct file_directory *fd)
{
    read_inode_by_no(ind, fd->st_ino);
    return 0;
}

// 该函数用于根据给定的inode和inode对应的数据块的块号（相对与inode对应的总的所有数据块）来获取对应文件的数据块
int read_block_by_ind_and_indNo(struct inode *ind, short indNo, struct data_block *blk)
{   
    // 由于inode所对应的文件可能会占用多个数据块
    // 在这里的indNo就代表我们inode对应文件所占用的所有的数据块的相对块号
    // 要转换成对于文件系统的绝对块号
    int a_blk_no = get_blk_no_by_indNo(ind, indNo); // 此处获得的是绝对数据块号
    int res = read_block_by_no(blk, a_blk_no);      // 此处根据绝对数据块号来读取对应的blk
    return res;
}

// 该函数用于根据数据块号来对对应的数据块进行写入
int write_block_by_no(struct data_block *dataB_blk, short no)
{
    // 原理和read的那个函数差不多
    FILE *fp = NULL;
    fp = fopen(disk_path, "r+");
    if (fp == NULL)
    {
        printf("open file failed:file is not existed!\n");
        return -1;
    }
    // 移动指针到对应的数据块（根据数据块号）
    fseek(fp, no * BLOCK_SIZE, SEEK_SET);
    // 写入dataB_blk这个buf的内容到指定数据块中
    fwrite(dataB_blk, sizeof(struct data_block), 1, fp);

    fclose(fp);
    return 0;
}

// 该函数用于根据inode号来对inode写入
int write_inode_by_no(struct inode *ind, short no)
{
    // 首先拿到inode所在的块号
    int ind_blk_no = INODE_BLOCK_START_NUM + (no / INODE_NUMS_IN_BLOCK);
    printf("write_inode_by_no:此时写入的inode号为%d,inode所在的块号为%d\n",no,ind_blk_no);
    // 然后再拿到该inode所在块中的位置
    int ind_offsize = no % INODE_NUMS_IN_BLOCK;
    printf("write_inode_by_no:该inode所在块中的位置为%d\n",ind_offsize);
    // 根据上面的属性，读出inode所在的这个blk
    struct data_block *blk = (struct data_block *)malloc(sizeof(struct data_block));
    read_block_by_no(blk, ind_blk_no);
    // 把传入修改的inode写入该数据块
    memcpy((struct inode *)blk + ind_offsize, ind, sizeof(struct inode));
    // 写回该数据块
    write_block_by_no(blk, ind_blk_no);

    free(blk);
    return 0;
}

// 该函数用于根据给定的inode和inode对应的数据块的块号（相对与inode对应的总的所有数据块）来写回对应文件的数据块
int write_block_by_ind_and_indNo(struct inode *ind, int indNo, struct data_block *blk)
{
    // 此处操作和read的函数差不多，区别就是把read_block_by_no换成写函数write_inode_by_no
    int a_blk_no = get_blk_no_by_indNo(ind, indNo); // 此处获得的是绝对数据块号
    int res = write_block_by_no(blk, a_blk_no);     // 此处根据绝对数据块号来读取对应的blk
    return res;
}

// 要实现的核心文件系统函数在此，fuse会根据命令来对我们编写的函数进行调用
static struct fuse_operations SFS_oper = {
    .init = SFS_init,       // 初始化
    .getattr = SFS_getattr, // 获取文件属性（包括目录的）
    .mknod = SFS_mknod,     // 创建文件
    .unlink = SFS_unlink,   // 删除文件
    .open = SFS_open,       // 无论是read还是write文件，都要用到打开文件
    .read = SFS_read,       // 读取文件内容
    .write = SFS_write,     // 修改文件内容
    .mkdir = SFS_mkdir,     // 创建目录
    .rmdir = SFS_rmdir,     // 删除目录
    .access = SFS_access,   // 进入目录
    .readdir = SFS_readdir, // 读取目录
};

int main(int argc, char *argv[])
{
    umask(022);
    return fuse_main(argc, argv, &SFS_oper, NULL);
}
