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
#define SUPER_BLOCK 1
#define ROOT_DIR_BLOCK 1      // 根目录块
#define INODE_BITMAP_BLOCK 1  // inode位图块数
#define DATA_BITMAP_BLOCK 4   // 数据块位图 块数
#define INODE_AREA_BLOCK 512  // inode区块数
#define MAX_DATA_IN_BLOCK 508 // 一个数据块实际能装的大小
#define BLOCK_NUMS (8 * 1024 * 1024 / BLOCK_SIZE)
#define DATA_BLOCK_START_NUM (SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK + INODE_AREA_BLOCK)         // 数据区开始的块数                                                      //系统总块数
#define DATA_AREA_BLOCK (BLOCK_NUMS - SUPER_BLOCK - INODE_BITMAP_BLOCK - DATA_BITMAP_BLOCK - INODE_AREA_BLOCK) // 剩下的空闲块数用作数据区的块
#define FILE_DIRECTORY_SIZE 16
#define MAX_FILENAME 8
#define MAX_EXTENSION 3

// 用于判断inode对应的文件是文件还是目录
#define IS_DIRECTORY(mode) (((mode)&S_IFMT) == S_IFDIR)
#define IS_REGULAR_FILE(mode) (((mode)&S_IFMT) == S_IFREG)

// 数据结构的定义
#pragma region

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
    short int st_ino;         // inode占用2字节 表示inode的块号（从0开始）
    char standby[3];          // 备用占用最后3字节
};

// 存放文件的数据块，抽象成一个数据结构
struct data_block
{
    size_t size;                  // 文件使用了这个块里面的多少Bytes 占用4B
    char data[MAX_DATA_IN_BLOCK]; // 一个块里面实际能存的数据大小 508B
};
#pragma endregion

// 我的磁盘8M文件路径
char *disk_path = "/home/wangchen/桌面/SFS/disk.img";

// 辅助函数声明
int read_block_by_no(struct data_block *dataB_blk, long no);
int read_inode_by_no(struct inode *ind, long no);
int write_block_by_no(struct data_block *dataB_blk, long no);
int write_inode_by_no(struct inode *ind, long no);
int read_inode(struct inode *ind, long no);

int determineFileType(const struct inode *myInode);
int isExistDir(struct inode *ind, char *fname, struct file_directory *fd);

// 功能函数声明
int get_fd_to_attr(const char *path, struct file_directory *attr);
int get_parent_and_fname(const char *path, char *parent_path, char *fname);
int get_info_by_path(const char *path, struct inode *ind, struct file_directory *fd);
int create_file_dir(const char *path, int flag);
int remove_file_dir(const char *path, int flag);
int create_new_fd_to_inode(struct inode *ind, const char *fname);
int create_dir_by_ino_number(char *fname,char *ext,short int ino_no,char *exp,struct file_directory *fd);

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
    printf("该文件系统的总块数为%d\n", sb_blk->fs_size);
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
    // 重新设置stbuf的内容
    memset(stbuf, 0, sizeof(struct stat));
    // 首先读取该目录下的文件到一个临时的文件中
    struct file_directory *t_file_directory = malloc(sizeof(struct file_directory));
    // 非根目录
    if (get_fd_to_attr(path, t_file_directory) == -1)
    {
        free(t_file_directory);
        printf("SFS_getattr：get_fd_to_attr时没找到文件，函数结束返回\n");
        return -ENOENT;
    }
    //
    FILE *fp = NULL;
    fp = fopen(disk_path, "r+");
    if (fp == NULL)
    {
        fprintf(stderr, "错误：打开文件失败，文件不存在，函数结束返回\n");
        return;
    }
    // 此时已经拿到了该文件/目录的信息
    // 根据文件里面的inode号
    // 移动指针进行读取属性
    fseek(fp, (SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK) * BLOCK_SIZE + t_file_directory->st_ino * FILE_DIRECTORY_SIZE, SEEK_SET);
    fread(ino_tmp, sizeof(struct inode), fp);
    // 读取inode的数据并赋给stbuf
    stbuf->st_ino = ino_tmp->st_ino;
    stbuf->st_atime = ino_tmp->st_atim;
    // 下面判断文件是 目录 还是 一般的文件
    // 并且修改stbuf对应的权限模式
    //  0666代表允许所有用户读取和写入目录，权限位是-rw-rw-rw-
    // 根据返回值来判断
    int fileType = determineFileType(&ino_tmp);
    int ret = 0; // 设置该函数的返回值
    switch (fileType)
    {
    case 1:
        printf("这是一个名为%s的目录，inode号为%d\n", t_file_directory->fname, t_file_directory->st_ino);
        stbuf->st_mode = S_IFDIR | 0666;
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
    free(ino_t);
    printf("SFS_getattr函数执行结束\n\n");
    return ret;
}

// SFS_readdir: 根据输入路径来读取该目录，并且显示目录内的内容
//  在终端中输入ls -l，libfuse会调用该函数
//  1.根据path读取对应的fd，拿到对应的inode
//  2.读取目录inode对应的目录数据块
//  3.遍历该数据块下的所有目录并且显示
static int SFS_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct file_directory *attr = malloc(sizeof(strcut file_directory));
    struct data_block *blk = malloc(sizeof(strcut data_block));
    // 调用上面刚刚实现的辅助函数
    // 打开path指定的fd，并且读取到attr中
    if (get_fd_to_attr(path, attr) == -1) // 打开path指定的文件，将文件属性读到attr中
    {
        printf("readdir:找不到该文件！\n");
        free(attr);
        free(blk);
        return -ENOENT;
    }
    // 根据读出来的fd，拿到对应的indoe
    struct inode *tinode = malloc(sizeof(struct inode));
    read_inode_by_no(tinode, attr->st_ino);
    // 拿到该inode之后，要保证对应的一定为目录！
    if (determineFileType(tinode) != 1)
    {
        // 如果返回值不为 1 ，说明inode对应的不是目录
        printf("readdir:%d下对应的不是目录！\n", path);
        free(attr);
        free(blk);
        return -ENOENT;
    }
    // 对buf里面的先使用filter函数添加 . 和 ..
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    // 接下来就是根据inode，分别对直接寻址和间接寻址(三级)利用while循环进行每一级的dir查找
    // 执行流程类似于get_fd_to_attr差不多
    // 设置一个装文件名的char数组
    char name[MAX_FILENAME + MAX_EXTENSION];
    // 对inode里8个addr进行查找
    for (int i = 0; i < 8; i++)
    {
        if (i <= 3)
        {
            // 直接寻址
            if (tinode->addr[i] == -1)
            {
                // 该目录不存在直接寻址
                printf("该目录不存在直接寻址\n");
                break;
            }
            int check1 = read_block_by_no(blk, DATA_BLOCK_START_NUM + tinode->addr[i]);
            if (check1 == -1)
            { // 读数据块失败
                printf("直接寻址读取数据块失败！\n");
                free(attr);
                free(blk);
                return -ENOENT;
            }
            int offsize = 0;
            struct file_directory *tfd = (struct file_directory *)blk->data;
            // 循环读取数据块中的文件名
            while (offsize < blk->size)
            {
                strcpy(name, tfd->fname);
                if (strlen(tfd->fext) != 0)
                {
                    strcat(name, ".");
                    strcat(name, tfd->fext);
                }
                if (name[strlen(name) - 1] != '~' && filler(buf, name, NULL, 0, 0)) // 将文件名添加到buf里面
                {
                    break;
                }
                tfd++;
                offsize += sizeof(struct file_directory);
            }
        }
        // 一级间接寻址
        // 和之前实现的get_info_by_path逻辑差不多
        // 也是加一层while循环
        else if (i == 4)
        {
            if (tinode->addr[i] == -1)
            {
                printf("该目录不存在一级间接寻址\n");
                break;
            }
            struct data_block *dir_blk1 = malloc(sizeof(struct data_block));
            int check1 = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + tinode->addr[i]);
            if (check1 == -1)
            { // 读数据块失败
                printf("一级间接寻址读取数据块失败！\n");
                free(attr);
                free(dir_blk1);
                free(blk);
                return -ENOENT;
            }
            short int *p1 = dir_blk1->data;
            int offsize1 = 0;
            while (offsize1 < dir_blk1->size)
            {
                int check = read_block_by_no(blk, DATA_BLOCK_START_NUM + *p1);
                if (check == -1)
                    return -1;
                int offsize = 0;
                struct file_directory *tfd = (struct file_directory *)blk->data;
                // 循环读取数据块中的文件名
                while (offsize < blk->size)
                {
                    strcpy(name, tfd->fname);
                    if (strlen(tfd->fext) != 0)
                    {
                        strcat(name, ".");
                        strcat(name, tfd->fext);
                    }
                    if (name[strlen(name) - 1] != '~' && filler(buf, name, NULL, 0, 0)) // 将文件名添加到buf里面
                    {
                        break;
                    }
                    tfd++;
                    offsize += sizeof(struct file_directory);
                }
                p1++;
                offsize1 += sizeof(short int);
            }
        }
        // 二级间接寻址
        // 在一级的条件下再多一层while
        else if (i == 5)
        {
            if (tinode->addr[i] == -1)
            {
                printf("该目录不存在二级间接寻址\n");
                break;
            }
            struct data_block *dir_blk2 = malloc(sizeof(struct data_block));
            int check2 = read_block_by_no(dir_blk2, DATA_BLOCK_START_NUM + tinode->addr[i]);
            if (check2 == -1)
            { // 读数据块失败
                printf("二级间接寻址读取数据块失败！\n");
                free(attr);
                free(dir_blk2);
                free(blk);
                return -ENOENT;
            }
            short int *p2 = dir_blk2->data;
            int offsize2 = 0;
            while (offsize2 < dir_blk2->size)
            {
                struct data_block *dir_blk1 = malloc(sizeof(struct data_block));
                int check1 = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p2);
                if (check1 == -1)
                { // 读数据块失败
                    return -1;
                }
                short int *p1 = dir_blk1->data;
                int offsize1 = 0;
                while (offsize1 < dir_blk1->size)
                {
                    int check = read_block_by_no(blk, DATA_BLOCK_START_NUM + *p1);
                    if (check == -1)
                        return -1;
                    int offsize = 0;
                    struct file_directory *tfd = (struct file_directory *)blk->data;
                    // 循环读取数据块中的文件名
                    while (offsize < blk->size)
                    {
                        strcpy(name, tfd->fname);
                        if (strlen(tfd->fext) != 0)
                        {
                            strcat(name, ".");
                            strcat(name, tfd->fext);
                        }
                        if (name[strlen(name) - 1] != '~' && filler(buf, name, NULL, 0, 0)) // 将文件名添加到buf里面
                        {
                            break;
                        }
                        tfd++;
                        offsize += sizeof(struct file_directory);
                    }
                    p1++;
                    offsize1 += sizeof(short int);
                }
                p2++;
                offsize2 += sizeof(short int);
            }
        }
        // 三级间接寻址
        else
        {
            if (tinode->addr[i] == -1)
            {
                printf("该目录不存在三级间接寻址\n");
                break;
            }
            struct data_block *dir_blk3 = malloc(sizeof(struct data_block));
            int check3 = read_block_by_no(dir_blk3, DATA_BLOCK_START_NUM + tinode->addr[i]);
            if (check3 == -1)
            { // 读数据块失败
                printf("三级间接寻址读取数据块失败！\n");
                free(attr);
                free(dir_blk3);
                free(blk);
                return -ENOENT;
            }
            short int *p3 = dir_blk3->data;
            int offsize3 = 0;
            while (offsize3 < dir_blk3->size)
            {
                struct data_block *dir_blk2 = malloc(sizeof(struct data_block));
                int check2 = read_block_by_no(dir_blk2, DATA_BLOCK_START_NUM + *p3);
                if (check2 == -1)
                { // 读数据块失败
                    return -1;
                }
                short int *p2 = dir_blk2->data;
                int offsize2 = 0;
                while (offsize2 < dir_blk2->size)
                {
                    struct data_block *dir_blk1 = malloc(sizeof(struct data_block));
                    int check1 = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p2);
                    if (check1 == -1)
                    { // 读数据块失败
                        return -1;
                    }
                    short int *p1 = dir_blk1->data;
                    int offsize1 = 0;
                    while (offsize1 < dir_blk1->size)
                    {
                        int check = read_block_by_no(blk, DATA_BLOCK_START_NUM + *p1);
                        if (check == -1)
                            return -1;
                        int offsize = 0;
                        struct file_directory *tfd = (struct file_directory *)blk->data;
                        // 循环读取数据块中的文件名
                        while (offsize < blk->size)
                        {
                            strcpy(name, tfd->fname);
                            if (strlen(tfd->fext) != 0)
                            {
                                strcat(name, ".");
                                strcat(name, tfd->fext);
                            }
                            if (name[strlen(name) - 1] != '~' && filler(buf, name, NULL, 0, 0)) // 将文件名添加到buf里面
                            {
                                break;
                            }
                            tfd++;
                            offsize += sizeof(struct file_directory);
                        }
                        p1++;
                        offsize1 += sizeof(short int);
                    }
                    p2++;
                    offsize2 += sizeof(short int);
                }
                p3++;
                offsize3 += sizeof(short int);
            }
        }
    }
    // for循环查找完之后会立刻break
    free(attr);
    free(blk);
    free(tinode);
    return 0;
}

// SFS_mkdir:将新目录添加到根目录，并且更新.directories文件
// 创建目录
static int SFS_mkdir(const char *path, mode_t mode)
{
    return create_file_dir(path, 2);
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
    if (flag == -1)
        return -1;
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
    // 分配数据块，用于等等inode和fd的内容的读取
    struct data_block *blk = malloc(sizeof(struct data_block));
    // 先找根目录的inode，一层一层
    struct inode *tinode = malloc(sizeof(struct tinode));
    struct file_directory *tfd; // 此处为while循环中利用tfd++来赋值,通过上面的inode先拿到初始的fd
    // 对根目录的判断
    if (strcmp(path, "/") == 0)
    {
        // 为根目录
        printf("get_info_by_paht：输入的路径为根目录：%s,函数已经执行\n", path);
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
    read_inode_by_no(tinode, 0);
    // 开始对路径进行分析
    char *tmp_path, *next_level, *cur_level, *next_ext; // tmp_path用于临时记录路径
    tmp_path = strdup(path);
    tmp_path++;        // 去掉最前面的一个"/"
    bool flag = false; // 是否找到文件 的表示
                       // 逐层检查(利用while循环)
    while (tmp_path != NULL)
    {
        cur_level = strdup(tmp_path); // 为源码的next_file_name
        next_level = strchr(tmp_path, "/");
        next_ext = '\0';
        // 说明查找的 文件或目录 就在当前目录tmp_path下
        if (next_level == null)
        {
            flag = true;
            // cur_level = strdup(tmp_path); //此时cur_level为目录的名字
            //  temp_path = strchr(temp_path, '/');
        }
        else
        {
            // 还存在下一级目录
            tmp_path = strchr(tmp_path, "/");
            char *tmp_next_ext = cur_level;
            tmp_next_ext = strchr(cur_level, "/");
            // 分割当前目录和下级目录
            if (tmp_next_ext)
                *tmp_next_ext = '\0';
        }
        if (flag)
        {
            // 如果已经找到该文件的目录
            // 对文件及其后缀名进行分割
            char *tmp_dot = strchr(cur_level, ".");
            if (tmp_dot)
            {
                *tmp_dot = '\0'; // 把文件名和后缀名分开成两个串
                // 此时cur_level单独指向文件名
                tmp_dot++;
                next_ext = tmp_dot;
            }
            printf("已找到文件！文件名为:%s,拓展名为%s\n", cur_level, next_ext);
        }
        bool isFindInode = false; // 寻找到当前层级下curlevel的inode
        for (int i = 0; i < 7 && tinode->addr[i] != -1 && !isFindInode, i++)
        {
            // 直接索引区addr[0]-addr[3]
            if (i <= 3)
            {
                // 通过addr的地址偏移量来对inode对应的数据块进行读取
                int check = read_block_by_no(blk, DATA_BLOCK_START_NUM + tinode->addr[i]);
                if (check == -1)
                    return -1;                            // 读取失败
                tfd = (struct file_directory *)blk->data; // tfd指向该数据块的开头，并且偏移量为fd大小
                int offsize = 0;                          // 记录当前的偏移量
                // 接下来对整个数据块内装的的目录或文件进行查找
                // 利用while循环，每次都偏移一个fd的大小
                while (offsize < blk->size)
                { // 条件为
                    // 当前fd的名字fname和curlevel相等
                    // 并且文件后缀名也相等（文件）或者无后缀名的分割符'.'
                    if (strcmp(tfd->fname, cur_level) == 0 && (next_ext == '\0' || strcmp(tfd->fext, next_ext) == 0))
                    {
                        printf("已找到当前目录或文件:%s\n\n", cur_level);
                        // 根据fd来获取当前目录文件对应的inode
                        read_inode_by_no(tinode, tfd->st_ino);
                        // 设置isFindInode为true
                        isFindInode = true;
                        if (flag) // 已经到最后一级路径
                        {
                            // 将该fd的信息读取到返回结果中
                            strcpy(fd->fname, tfd->fname);
                            strcpy(fd->fext, tfd->fext);
                            strcpy(fd->standby, tfd->standby);
                            fd->st_ino = tfd->st_ino;
                            // 根据fd的inode号来读取对应的inode信息
                            read_inode_by_no(ind, tfd->st_ino);
                            printf("找到对应的inode：inode号为:%d\n", tfd->st_ino);
                            return 0;
                        }
                        // 已经找到对应的inode，退出循环
                        break;
                    }
                    // 该fd和当前目录的name不同
                    offsize += sizeof(struct file_directory);
                    // 指针偏移fd字节继续查找
                    tfd++;
                }
            }
            else if (i == 4) // 一级间接寻址
            {
                // 与直接寻址不同的是
                // 在外层先拿到addr[i]一级间接寻址指向的磁盘空间
                // 这块数据块存的是一个个的目录
                struct data_block *dir_blk1 = malloc(sizeof(struct data_block));
                // 根据fd的addr读取改数据块
                int check1 = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + tinode->addr[i]);
                if (check1 == -1)
                    return -1;
                // 设置一个指针指向这个dir_blk1,通过偏移量来指向不同的dir
                // 记得回来debug这里
                printf("check dir_blk1 point\n");
                short int *p1 = dir_blk1->data;
                int offsize1 = 0;
                // 利用while循环遍历这些dir
                // 此处不同的是
                // 如果内层循环找到了inode，外层循环也退出
                while (offsize1 < dir_blk1->size && !isFindInode)
                {
                    // 依次读出dir_blk1中的fd
                    int check = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p1);
                    if (check == -1)
                        return -1;
                    // 下面的操作和直接寻址一样
                    tfd = (struct file_directory)blk->data;
                    int offsize = 0; // 记录当前的偏移量
                    // 接下来对整个数据块内装的的目录或文件进行查找
                    // 利用while循环，每次都偏移一个fd的大小
                    // 此处可能出bug
                    while (offsize < blk->size)
                    { // 条件为
                        // 当前fd的名字fname和curlevel相等
                        // 并且文件后缀名也相等（文件）或者无后缀名的分割符'.'
                        if (strcmp(tfd->fname, cur_level) == 0 && (next_ext == '\0' || strcmp(tfd->fext, next_ext) == 0))
                        {
                            printf("已找到当前目录或文件:%s\n\n", cur_level);
                            // 根据fd来获取当前目录文件对应的inode
                            read_inode_by_no(tinode, tfd->st_ino);
                            // 设置isFindInode为true
                            isFindInode = true;
                            if (flag) // 已经到最后一级路径
                            {
                                // 将该fd的信息读取到返回结果中
                                strcpy(fd->fname, tfd->fname);
                                strcpy(fd->fext, tfd->fext);
                                strcpy(fd->standby, tfd->standby);
                                fd->st_ino = tfd->st_ino;
                                // 根据fd的inode号来读取对应的inode信息
                                read_inode_by_no(ind, tfd->st_ino);
                                printf("找到对应的inode：inode号为:%d\n", tfd->st_ino);
                                return 0;
                            }
                            // 已经找到对应的inode，退出循环
                            break;
                        }
                        // 该fd和当前目录的name不同
                        offsize += sizeof(struct file_directory);
                        // 指针偏移fd字节继续查找
                        tfd++;
                    }
                    // 内层循环没找到对应的目录文件
                    // 偏移外层dir数据块的指针，对下一个dir指向的数据块进行查找
                    p1++;
                    offsize1 += sizeof(short int);
                }
            }
            else if (i == 5) // 二级间接寻址
            {
                // 原理和一级间接寻址一样
                // 只不过多了一层循环
                // 此处不多做注释
                struct data_block *dir_blk2 = malloc(sizeof(struct data_block));
                int check2 = read_block_by_no(dir_blk2, DATA_BLOCK_START_NUM + tinode->addr[i]);
                if (check2 = -1)
                    return -1;
                short int *p2 = dir_blk2->data;
                int offsize2 = 0;
                while (offsize2 < dir_blk2->size && !isFindInode)
                {
                    // 下面的内容都可以直接复制一层循环的代码
                    // 与直接寻址不同的是
                    // 在外层先拿到addr[i]一级间接寻址指向的磁盘空间
                    // 这块数据块存的是一个个的目录
                    struct data_block *dir_blk1 = malloc(sizeof(struct data_block));
                    // 根据fd的addr读取改数据块
                    // 此处为唯一修改的的地方
                    // 偏移量设置为二级间址的指针
                    int check1 = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p2);
                    if (check1 == -1)
                        return -1;
                    // 设置一个指针指向这个dir_blk1,通过偏移量来指向不同的dir
                    // 记得回来debug这里
                    printf("check dir_blk1 point\n");
                    short int *p1 = dir_blk1->data;
                    int offsize1 = 0;
                    // 利用while循环遍历这些dir
                    // 此处不同的是
                    // 如果内层循环找到了inode，外层循环也退出
                    while (offsize1 < dir_blk1->size && !isFindInode)
                    {
                        // 依次读出dir_blk1中的fd
                        int check = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p1);
                        if (check == -1)
                            return -1;
                        // 下面的操作和直接寻址一样
                        tfd = (struct file_directory)blk->data;
                        int offsize = 0; // 记录当前的偏移量
                        // 接下来对整个数据块内装的的目录或文件进行查找
                        // 利用while循环，每次都偏移一个fd的大小
                        // 此处可能出bug
                        while (offsize < blk->size)
                        { // 条件为
                            // 当前fd的名字fname和curlevel相等
                            // 并且文件后缀名也相等（文件）或者无后缀名的分割符'.'
                            if (strcmp(tfd->fname, cur_level) == 0 && (next_ext == '\0' || strcmp(tfd->fext, next_ext) == 0))
                            {
                                printf("已找到当前目录或文件:%s\n\n", cur_level);
                                // 根据fd来获取当前目录文件对应的inode
                                read_inode_by_no(tinode, tfd->st_ino);
                                // 设置isFindInode为true
                                isFindInode = true;
                                if (flag) // 已经到最后一级路径
                                {
                                    // 将该fd的信息读取到返回结果中
                                    strcpy(fd->fname, tfd->fname);
                                    strcpy(fd->fext, tfd->fext);
                                    strcpy(fd->standby, tfd->standby);
                                    fd->st_ino = tfd->st_ino;
                                    // 根据fd的inode号来读取对应的inode信息
                                    read_inode_by_no(ind, tfd->st_ino);
                                    printf("找到对应的inode：inode号为:%d\n", tfd->st_ino);
                                    return 0;
                                }
                                // 已经找到对应的inode，退出循环
                                break;
                            }
                            // 该fd和当前目录的name不同
                            offsize += sizeof(struct file_directory);
                            // 指针偏移fd字节继续查找
                            tfd++;
                        }
                        // 内层循环没找到对应的目录文件
                        // 偏移外层dir数据块的指针，对下一个dir指向的数据块进行查找
                        p1++;
                        offsize1 += sizeof(short int);
                    }
                    p2++;
                    offsize2 += sizeof(short int);
                }
            }
            else // 三级间接寻址
            {
                // 同理，只需多加一层while循环，并且改变read dir目录的数据块时候的偏移为上一层的指针即可
                struct data_block *dir_blk3 = malloc(sizeof(struct data_block));
                int check3 = read_block_by_no(dir_blk3, DATA_BLOCK_START_NUM + tinode->addr[i]);
                if (check3 == -1)
                    return -1;
                int offsize3 = 0;
                short int *p3 = dir_blk3->data;
                while (offsize3 < dir_blk3->size && !isFindInode)
                {
                    struct data_block *dir_blk2 = malloc(sizeof(struct data_block));
                    int check2 = read_block_by_no(dir_blk2, DATA_BLOCK_START_NUM + *p3);
                    if (check2 = -1)
                        return -1;
                    short int *p2 = dir_blk2->data;
                    int offsize2 = 0;
                    while (offsize2 < dir_blk2->size && !isFindInode)
                    {
                        // 下面的内容都可以直接复制一层循环的代码
                        // 与直接寻址不同的是
                        // 在外层先拿到addr[i]一级间接寻址指向的磁盘空间
                        // 这块数据块存的是一个个的目录
                        struct data_block *dir_blk1 = malloc(sizeof(struct data_block));
                        // 根据fd的addr读取改数据块
                        // 此处为唯一修改的的地方
                        // 偏移量设置为二级间址的指针
                        int check1 = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p2);
                        if (check1 == -1)
                            return -1;
                        // 设置一个指针指向这个dir_blk1,通过偏移量来指向不同的dir
                        // 记得回来debug这里
                        printf("check dir_blk1 point\n");
                        short int *p1 = dir_blk1->data;
                        int offsize1 = 0;
                        // 利用while循环遍历这些dir
                        // 此处不同的是
                        // 如果内层循环找到了inode，外层循环也退出
                        while (offsize1 < dir_blk1->size && !isFindInode)
                        {
                            // 依次读出dir_blk1中的fd
                            int check = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p1);
                            if (check == -1)
                                return -1;
                            // 下面的操作和直接寻址一样
                            tfd = (struct file_directory)blk->data;
                            int offsize = 0; // 记录当前的偏移量
                            // 接下来对整个数据块内装的的目录或文件进行查找
                            // 利用while循环，每次都偏移一个fd的大小
                            // 此处可能出bug
                            while (offsize < blk->size)
                            { // 条件为
                                // 当前fd的名字fname和curlevel相等
                                // 并且文件后缀名也相等（文件）或者无后缀名的分割符'.'
                                if (strcmp(tfd->fname, cur_level) == 0 && (next_ext == '\0' || strcmp(tfd->fext, next_ext) == 0))
                                {
                                    printf("已找到当前目录或文件:%s\n\n", cur_level);
                                    // 根据fd来获取当前目录文件对应的inode
                                    read_inode_by_no(tinode, tfd->st_ino);
                                    // 设置isFindInode为true
                                    isFindInode = true;
                                    if (flag) // 已经到最后一级路径
                                    {
                                        // 将该fd的信息读取到返回结果中
                                        strcpy(fd->fname, tfd->fname);
                                        strcpy(fd->fext, tfd->fext);
                                        strcpy(fd->standby, tfd->standby);
                                        fd->st_ino = tfd->st_ino;
                                        // 根据fd的inode号来读取对应的inode信息
                                        read_inode_by_no(ind, tfd->st_ino);
                                        printf("找到对应的inode：inode号为:%d\n", tfd->st_ino);
                                        return 0;
                                    }
                                    // 已经找到对应的inode，退出循环
                                    break;
                                }
                                // 该fd和当前目录的name不同
                                offsize += sizeof(struct file_directory);
                                // 指针偏移fd字节继续查找
                                tfd++;
                            }
                            // 内层循环没找到对应的目录文件
                            // 偏移外层dir数据块的指针，对下一个dir指向的数据块进行查找
                            p1++;
                            offsize1 += sizeof(short int);
                        }
                        p2++;
                        offsize2 += sizeof(short int);
                    }
                    p3++;
                    offsize3 += sizeof(short int);
                }
            }
        }
        // 进入下级目录
        // 继续执行新一轮的目录拆解寻找
        if (tmp_path != NULL)
        {
            tmp_path++;
        }
    }
    // 进入了for循环查询
    // 但是没有找到对应文件
    printf("找不到:%s 路径下的文件！\n", path);
    free(blk);
    free(tinode);
    return -1; // 未找到文件！
}

// 该函数用于分割路径得到父目录和要创建的文件名或目录名
int get_parent_and_fname(const char *path, char **parent_path, char **fname)
{
    char *tmp_path = strdup(path);
    *parent_path = tmp_path; // 记录父目录
    // 利用strrchr移动到最后一级（文件名或目录名）来取得信息
    // 首先判断path是不是根目录
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
    }
    printf("父目录为:%s,创建的文件名或目录名为:%s\n", *parent_path, *fname);
    return 0;
}

// 该函数创建path所指文件或目录的fd
// 并且创建空闲数据块
// 创建成功返回1，失败返回-1
int create_file_dir(const char *path, int flag)
{ // 获得父目录和目录名
    printf("create_file_dir函数被调用！\n");
    char *fname, *parent_path;
    get_parent_and_fname(path, &parent_path, &fname);
    // 根据父目录来获取父目录的inode和fd
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    struct inode *tinode = malloc(sizeof(struct inode));
    get_info_by_path(parent_path, tinode, tfd);

    // 判断文件名是否过长
    if (strlen(fname) > 8)
    {
        printf("文件名%s过长，超出8字节！函数终止");
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
    int res = create_new_fd_to_inode(tinode, fname);

    // 创建成功后返回
    free(tfd);
    free(tinode);
    if (res > 0)
        res = 0;
    return res;
}

// 该函数用于往已知的inode里面添加新建的目录项
int create_new_fd_to_inode(struct inode *ind, const char *fname)
{
    // 为新建的目录项分配空间
    struct file_directory *fd = malloc(sizeof(struct file_directory));
    // TODO:对是否存在目录项进行判断
    if (isExistDir(ind, fname, fd))
    {
        free(fd);
        return -EEXIST;
    }
    
    //对该inode中添加目录
    struct inode *cr_ind = malloc(sizeof(struct inode));
    struct data_block* cr_blk = malloc(sizeof(struct data_block));
    //设置创建的目录的inode信息

    cr_ind->st_ino = get_valid_ind();
    cr_ind->addr[0] = get_valid_blk();
    //cr_ind->st_mode = 
    cr_ind->st_size = 0;
    //设置目录项信息
    char *tname = strdup(fname);//文件名
    char *text = strchr(tname,'.');//拓展名的分隔符的指针
    if(text){
        //没有拓展名
        //直接创建
        create_dir_by_ino_number(tname,"",cr_ind->st_ino,"",fd);
    }
    else
    {
        //有拓展名，需要分割一下文件名和拓展名
        *text = '\0';//分割
        text++;//移动到拓展名位置
        create_dir_by_ino_number(tname,text,cr_ind->st_ino,"",fd);
    }
    fd->st_ino = cr_ind->st_ino;
    
    //寻找父目录对应的数据块的末尾块块号
    short int end_blk = ind->st_size / MAX_DATA_IN_BLOCK;
    

}

// 辅助函数的定义
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