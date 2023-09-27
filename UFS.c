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
#define ROOT_DIR_BLOCK 1      //根目录块
#define INODE_BITMAP_BLOCK 1  // inode位图块数
#define DATA_BITMAP_BLOCK 4   //数据块位图 块数
#define INODE_AREA_BLOCK 512  // inode区块数
#define MAX_DATA_IN_BLOCK 508 //一个数据块实际能装的大小
#define BLOCK_NUMS (8 * 1024 * 1024 / BLOCK_SIZE)
#define DATA_BLOCK_START_NUM (SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK + INODE_AREA_BLOCK)         //数据区开始的块数                                                      //系统总块数
#define DATA_AREA_BLOCK (BLOCK_NUMS - SUPER_BLOCK - INODE_BITMAP_BLOCK - DATA_BITMAP_BLOCK - INODE_AREA_BLOCK) //剩下的空闲块数用作数据区的块
#define FILE_DIRECTORY_SIZE 16
#define MAX_FILENAME 8
#define MAX_EXTENSION 3

//用于判断inode对应的文件是文件还是目录
#define IS_DIRECTORY(mode) (((mode)&S_IFMT) == S_IFDIR)
#define IS_REGULAR_FILE(mode) (((mode)&S_IFMT) == S_IFREG)

//数据结构的定义
#pragma region

//超级块sb 占用一个磁盘块 总字节为56B
struct sb
{
    long fs_size;                  //文件系统的大小，以块为单位
    long first_blk;                //数据区的第一块块号，根目录也放在此
    long datasize;                 //数据区大小，以块为单位
    long first_inode;              // inode区起始块号
    long inode_area_size;          // inode区大小，以块为单位
    long fisrt_blk_of_inodebitmap; // inode位图区起始块号
    long inodebitmap_size;         // inode位图区大小，以块为单位
    long first_blk_of_databitmap;  //数据块位图起始块号
    long databitmap_size;          //数据块位图大小，以块为单位
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

//记录目录信息的数据结构
struct file_directory
{
    //文件名占用前8字节，拓展名占用3字节
    char fname[MAX_FILENAME]; //文件名
    char fext[MAX_EXTENSION]; //扩展名
    short int st_ino;         // inode占用2字节 表示inode的块号（从0开始）
    char standby[3];          //备用占用最后3字节
};

//存放文件的数据块，抽象成一个数据结构
struct data_block
{
    size_t size;                  //文件使用了这个块里面的多少Bytes 占用4B
    char data[MAX_DATA_IN_BLOCK]; // 一个块里面实际能存的数据大小 508B
};
#pragma endregion

//我的磁盘8M文件路径
char *disk_path = "/home/wangchen/桌面/SFS/disk.img";

//辅助函数声明
int read_block_by_no(struct data_block *dataB_blk, long no);
int read_inode_by_no(struct inode *ind, long no);
int write_block_by_no(struct data_block *dataB_blk, long no);
int write_inode_by_no(struct inode *ind, long no);
int read_inode(struct inode *ind, long no);

int determineFileType(const struct inode *myInode);

//功能函数声明
int get_fd_to_attr(const char *path, struct file_directory *attr);
int get_info_by_path(const char *path, struct inode *ind, struct file_directory *fd);
int create_file_dir(const char *path, int flag);
int remove_file_dir(const char *path, int flag);

//要实现的核心文件系统函数在此，fuse会根据命令来对我们编写的函数进行调用
static struct fuse_operations SFS_oper = {
    .init = SFS_init,       //初始化
    .getattr = SFS_getattr, //获取文件属性（包括目录的）
    .mknod = SFS_mknod,     //创建文件
    .unlink = SFS_unlink,   //删除文件
    .open = SFS_open,       //无论是read还是write文件，都要用到打开文件
    .read = SFS_read,       //读取文件内容
    .write = SFS_write,     //修改文件内容
    .mkdir = SFS_mkdir,     //创建目录
    .rmdir = SFS_rmdir,     //删除目录
    .access = SFS_access,   //进入目录
    .readdir = SFS_readdir, //读取目录
};
//核心函数的实现
#pragma region

//对初始化函数SFS_init的实现
static void *SFS_init(struct fuse_conn_info *conn)
{
    //其实这个init函数对整个文件系统左右不算太大，因为我们要得到的文件系统大小的数据
    //其实在宏定义已经算出来了
    //只不过该文件系统必须执行这个函数，所以只能按步骤走
    printf("SFS_init函数开始\n\n");
    FILE *fp = NULL;
    fp = fopen(disk_path, "r+");
    if (fp == NULL)
    {
        fprintf(stderr, "错误：打开文件失败，文件不存在，函数结束返回\n");
        return;
    }
    //首先读取超级块里面的内容获取文件系统的信息
    struct sb *sb_blk = malloc(sizeof(struct sb));
    fread(sb_blk, sizeof(struct sb), 1, fp);
    fclose(fp);
    //下面随便输出一些超级块的内容，确定读写是否成功
    printf("该文件系统的总块数为%d\n", sb_blk->fs_size);
    //测试完成可以free掉
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

//读取文件属性的函数SFS_getattr,并且赋值给stbuf
//查找输入的路径，确定它是一个目录还是一个文件。
//如果是目录，返回适当的权限。如果是文件，返回适当的权限以及实际大小。
static int SFS_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    printf("SFS_getattr函数执行\n\n");
    //通过对应的inode来判断文件的属性
    struct inode *ino_tmp = malloc(sizeof(struct inode));
    //重新设置stbuf的内容
    memset(stbuf, 0, sizeof(struct stat));
    //首先读取该目录下的文件到一个临时的文件中
    struct file_directory *t_file_directory = malloc(sizeof(struct file_directory));
    //非根目录
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
    //此时已经拿到了该文件/目录的信息
    //根据文件里面的inode号
    //移动指针进行读取属性
    fseek(fp, (SUPER_BLOCK + INODE_BITMAP_BLOCK + DATA_BITMAP_BLOCK) * BLOCK_SIZE + t_file_directory->st_ino * FILE_DIRECTORY_SIZE, SEEK_SET);
    fread(ino_tmp, sizeof(struct inode), fp);
    //读取inode的数据并赋给stbuf
    stbuf->st_ino = ino_tmp->st_ino;
    stbuf->st_atime = ino_tmp->st_atim;
    //下面判断文件是 目录 还是 一般的文件
    //并且修改stbuf对应的权限模式
    // 0666代表允许所有用户读取和写入目录，权限位是-rw-rw-rw-
    //根据返回值来判断
    int fileType = determineFileType(&ino_tmp);
    int ret = 0; //设置该函数的返回值
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

//根据输入路径来读取该目录，并且显示目录内的内容SFS_readdir
static int SFS_readdir()

#pragma endregion

    //功能函数的定义
    //根据传入的文件路径path找到对应的fd然后传给attr
    int get_fd_to_attr(const char *path, struct file_directory *attr)
{
    printf("get_fd_to_attr函数运行!\n\n");
    printf("查询的路径为%s\n\n", path);
    //先读取超级块
    //获得根目录在数据区的位置（根据块数）
    struct sb *super_blk;
    struct data_block *data_blk;

    data_blk = malloc(sizeof(struct data_block));
    //利用read_block_by_no函数读取超级块
    //超级块的块号为0，刚好直接查询并且读到创建的data_blk中
    int tmp = read_block_by_no(data_blk, 0);
    //根据查询的结果来判断是否读取成功
    if (tmp == -1)
    {
        printf("读取超级块失败!\n\n");
        free(data_blk);
        return -1;
    }
    //读取成功
    super_blk = (struct sb *)data_blk;
    //检查路径
    //如果路径为空，则出错返回1
    char *tmp_path = strdup(path);
    if (!tmp_path)
    {
        printf("错误：get_fd_to_attr：路径为空，函数结束返回\n\n");
        free(super_blk);
        return -1;
    }
    //根据路径来寻找对应的inode和fd文件
    //创建临时的inode和file_directory来接收
    struct inode *tinode = malloc(sizeof(struct inode));
    struct file_directory *tfd = malloc(sizeof(struct file_directory));
    //通过get_info_by_path函数找到路径对应文件的inode和file_directory，并且读取到临时的文件当中
    int flag = get_info_by_path(path, tinode, tfd);
    //根据返回值判断是否读取成功
    if (flag == -1)
        return -1;
    //读取成功，把fd的内容赋值给attr返回
    strcpy(attr->fname, tfd->fname);
    attr->st_ino = tfd->st_ino;
    // strcpy(attr->st_ino,tfd->st_ino);
    strcpy(attr->fext, tfd->fext);
    strcpy(attr->standby, tfd->standby);
    //释放内存
    free(data_blk);
    free(tinode);
    free(tfd);
    return 0;
}

//该函数根据输入文件的路径找到对应的inode和fd
int get_info_by_path(const char *path, struct inode *ind, struct file_directory *fd)
{
    //查找路径path只会显示文件系统下的路径
    //意思是把文件系统挂载的路径当成根目录
    printf("get_info_by_path函数开始执行,文件的路径为%s\n\n", path);
    //分配数据块，用于等等inode和fd的内容的读取
    struct data_block *blk = malloc(sizeof(struct data_block));
    //先找根目录的inode，一层一层
    struct inode *tinode = malloc(sizeof(struct tinode));
    struct file_directory *tfd; //此处为while循环中利用tfd++来赋值,通过上面的inode先拿到初始的fd
    //对根目录的判断
    if (strcmp(path, "/") == 0)
    {
        //为根目录
        printf("get_info_by_paht：输入的路径为根目录：%s,函数已经执行\n", path);
        //根目录的inode号为0
        read_inode_by_no(ind, 0);
        //设置一下根目录的文件返回
        strcpy(fd->fname, path);
        fd->st_ino = 0;
        return 0;
    }
    //不是根目录
    //文件查找需要一层一层查找
    //先拿到根目录的inode（inode号为0）
    read_inode_by_no(tinode, 0);
    //开始对路径进行分析
    char *tmp_path, *next_level, *cur_level, *next_ext; // tmp_path用于临时记录路径
    tmp_path = strdup(path);
    tmp_path++;        //去掉最前面的一个"/"
    bool flag = false; //是否找到文件 的表示
                       //逐层检查(利用while循环)
    while (tmp_path != NULL)
    {
        cur_level = strdup(tmp_path); //为源码的next_file_name
        next_level = strchr(tmp_path, "/");
        next_ext = '\0';
        //说明查找的 文件或目录 就在当前目录tmp_path下
        if (next_level == null)
        {
            flag = true;
            // cur_level = strdup(tmp_path); //此时cur_level为目录的名字
            //  temp_path = strchr(temp_path, '/');
        }
        else
        {
            //还存在下一级目录
            tmp_path = strchr(tmp_path, "/");
            char *tmp_next_ext = cur_level;
            tmp_next_ext = strchr(cur_level, "/");
            //分割当前目录和下级目录
            if (tmp_next_ext)
                *tmp_next_ext = '\0';
        }
        if (flag)
        {
            //如果已经找到该文件的目录
            //对文件及其后缀名进行分割
            char *tmp_dot = strchr(cur_level, ".");
            if (tmp_dot)
            {
                *tmp_dot = '\0'; //把文件名和后缀名分开成两个串
                //此时cur_level单独指向文件名
                tmp_dot++;
                next_ext = tmp_dot;
            }
            printf("已找到文件！文件名为:%s,拓展名为%s\n", cur_level, next_ext);
        }
        bool isFindInode = false; //寻找到当前层级下curlevel的inode
        for (int i = 0; i < 7 && tinode->addr[i] != -1 && !isFindInode, i++)
        {
            //直接索引区addr[0]-addr[3]
            if (i <= 3)
            {
                //通过addr的地址偏移量来对inode对应的数据块进行读取
                int check = read_block_by_no(blk, DATA_BLOCK_START_NUM + tinode->addr[i]);
                if (check == -1)
                    return -1;                            //读取失败
                tfd = (struct file_directory *)blk->data; // tfd指向该数据块的开头，并且偏移量为fd大小
                int offsize = 0;                          //记录当前的偏移量
                //接下来对整个数据块内装的的目录或文件进行查找
                //利用while循环，每次都偏移一个fd的大小
                while (offsize < blk->size)
                { //条件为
                    //当前fd的名字fname和curlevel相等
                    //并且文件后缀名也相等（文件）或者无后缀名的分割符'.'
                    if (strcmp(tfd->fname, cur_level) == 0 && (next_ext == '\0' || strcmp(tfd->fext, next_ext) == 0))
                    {
                        printf("已找到当前目录或文件:%s\n\n", cur_level);
                        //根据fd来获取当前目录文件对应的inode
                        read_inode_by_no(tinode, tfd->st_ino);
                        //设置isFindInode为true
                        isFindInode = true;
                        if (flag) //已经到最后一级路径
                        {
                            //将该fd的信息读取到返回结果中
                            strcpy(fd->fname, tfd->fname);
                            strcpy(fd->fext, tfd->fext);
                            strcpy(fd->standby, tfd->standby);
                            fd->st_ino = tfd->st_ino;
                            //根据fd的inode号来读取对应的inode信息
                            read_inode_by_no(ind, tfd->st_ino);
                            printf("找到对应的inode：inode号为:%d\n", tfd->st_ino);
                            return 0;
                        }
                        //已经找到对应的inode，退出循环
                        break;
                    }
                    //该fd和当前目录的name不同
                    offsize += sizeof(struct file_directory);
                    //指针偏移fd字节继续查找
                    tfd++;
                }
            }
            else if (i == 4) //一级间接寻址
            {
                //与直接寻址不同的是
                //在外层先拿到addr[i]一级间接寻址指向的磁盘空间
                //这块数据块存的是一个个的目录
                struct data_block *dir_blk1 = malloc(sizeof(struct data_block));
                //根据fd的addr读取改数据块
                int check1 = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + tinode->addr[i]);
                if (check1 == -1)
                    return -1;
                //设置一个指针指向这个dir_blk1,通过偏移量来指向不同的dir
                //记得回来debug这里
                printf("check dir_blk1 point\n");
                short int *p1 = dir_blk1->data;
                int offsize1 = 0;
                //利用while循环遍历这些dir
                //此处不同的是
                //如果内层循环找到了inode，外层循环也退出
                while (offsize1 < dir_blk1->size && !isFindInode)
                {
                    //依次读出dir_blk1中的fd
                    int check = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p1);
                    if (check == -1)
                        return -1;
                    //下面的操作和直接寻址一样
                    tfd = (struct file_directory)blk->data;
                    int offsize = 0; //记录当前的偏移量
                    //接下来对整个数据块内装的的目录或文件进行查找
                    //利用while循环，每次都偏移一个fd的大小
                    //此处可能出bug
                    while (offsize < blk->size)
                    { //条件为
                        //当前fd的名字fname和curlevel相等
                        //并且文件后缀名也相等（文件）或者无后缀名的分割符'.'
                        if (strcmp(tfd->fname, cur_level) == 0 && (next_ext == '\0' || strcmp(tfd->fext, next_ext) == 0))
                        {
                            printf("已找到当前目录或文件:%s\n\n", cur_level);
                            //根据fd来获取当前目录文件对应的inode
                            read_inode_by_no(tinode, tfd->st_ino);
                            //设置isFindInode为true
                            isFindInode = true;
                            if (flag) //已经到最后一级路径
                            {
                                //将该fd的信息读取到返回结果中
                                strcpy(fd->fname, tfd->fname);
                                strcpy(fd->fext, tfd->fext);
                                strcpy(fd->standby, tfd->standby);
                                fd->st_ino = tfd->st_ino;
                                //根据fd的inode号来读取对应的inode信息
                                read_inode_by_no(ind, tfd->st_ino);
                                printf("找到对应的inode：inode号为:%d\n", tfd->st_ino);
                                return 0;
                            }
                            //已经找到对应的inode，退出循环
                            break;
                        }
                        //该fd和当前目录的name不同
                        offsize += sizeof(struct file_directory);
                        //指针偏移fd字节继续查找
                        tfd++;
                    }
                    //内层循环没找到对应的目录文件
                    //偏移外层dir数据块的指针，对下一个dir指向的数据块进行查找
                    p1++;
                    offsize1 += sizeof(short int);
                }
            }
            else if (i == 5) //二级间接寻址
            {
                //原理和一级间接寻址一样
                //只不过多了一层循环
                //此处不多做注释
                struct data_block *dir_blk2 = malloc(sizeof(struct data_block));
                int check2 = read_block_by_no(dir_blk2, DATA_BLOCK_START_NUM + tinode->addr[i]);
                if (check2 = -1)
                    return -1;
                short int *p2 = dir_blk2->data;
                int offsize2 = 0;
                while (offsize2 < dir_blk2->size && !isFindInode)
                {
                    //下面的内容都可以直接复制一层循环的代码
                    //与直接寻址不同的是
                    //在外层先拿到addr[i]一级间接寻址指向的磁盘空间
                    //这块数据块存的是一个个的目录
                    struct data_block *dir_blk1 = malloc(sizeof(struct data_block));
                    //根据fd的addr读取改数据块
                    //此处为唯一修改的的地方
                    //偏移量设置为二级间址的指针
                    int check1 = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p2);
                    if (check1 == -1)
                        return -1;
                    //设置一个指针指向这个dir_blk1,通过偏移量来指向不同的dir
                    //记得回来debug这里
                    printf("check dir_blk1 point\n");
                    short int *p1 = dir_blk1->data;
                    int offsize1 = 0;
                    //利用while循环遍历这些dir
                    //此处不同的是
                    //如果内层循环找到了inode，外层循环也退出
                    while (offsize1 < dir_blk1->size && !isFindInode)
                    {
                        //依次读出dir_blk1中的fd
                        int check = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p1);
                        if (check == -1)
                            return -1;
                        //下面的操作和直接寻址一样
                        tfd = (struct file_directory)blk->data;
                        int offsize = 0; //记录当前的偏移量
                        //接下来对整个数据块内装的的目录或文件进行查找
                        //利用while循环，每次都偏移一个fd的大小
                        //此处可能出bug
                        while (offsize < blk->size)
                        { //条件为
                            //当前fd的名字fname和curlevel相等
                            //并且文件后缀名也相等（文件）或者无后缀名的分割符'.'
                            if (strcmp(tfd->fname, cur_level) == 0 && (next_ext == '\0' || strcmp(tfd->fext, next_ext) == 0))
                            {
                                printf("已找到当前目录或文件:%s\n\n", cur_level);
                                //根据fd来获取当前目录文件对应的inode
                                read_inode_by_no(tinode, tfd->st_ino);
                                //设置isFindInode为true
                                isFindInode = true;
                                if (flag) //已经到最后一级路径
                                {
                                    //将该fd的信息读取到返回结果中
                                    strcpy(fd->fname, tfd->fname);
                                    strcpy(fd->fext, tfd->fext);
                                    strcpy(fd->standby, tfd->standby);
                                    fd->st_ino = tfd->st_ino;
                                    //根据fd的inode号来读取对应的inode信息
                                    read_inode_by_no(ind, tfd->st_ino);
                                    printf("找到对应的inode：inode号为:%d\n", tfd->st_ino);
                                    return 0;
                                }
                                //已经找到对应的inode，退出循环
                                break;
                            }
                            //该fd和当前目录的name不同
                            offsize += sizeof(struct file_directory);
                            //指针偏移fd字节继续查找
                            tfd++;
                        }
                        //内层循环没找到对应的目录文件
                        //偏移外层dir数据块的指针，对下一个dir指向的数据块进行查找
                        p1++;
                        offsize1 += sizeof(short int);
                    }
                    p2++;
                    offsize2 += sizeof(short int);
                }
            }
            else //三级间接寻址
            {
                //同理，只需多加一层while循环，并且改变read dir目录的数据块时候的偏移为上一层的指针即可
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
                        //下面的内容都可以直接复制一层循环的代码
                        //与直接寻址不同的是
                        //在外层先拿到addr[i]一级间接寻址指向的磁盘空间
                        //这块数据块存的是一个个的目录
                        struct data_block *dir_blk1 = malloc(sizeof(struct data_block));
                        //根据fd的addr读取改数据块
                        //此处为唯一修改的的地方
                        //偏移量设置为二级间址的指针
                        int check1 = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p2);
                        if (check1 == -1)
                            return -1;
                        //设置一个指针指向这个dir_blk1,通过偏移量来指向不同的dir
                        //记得回来debug这里
                        printf("check dir_blk1 point\n");
                        short int *p1 = dir_blk1->data;
                        int offsize1 = 0;
                        //利用while循环遍历这些dir
                        //此处不同的是
                        //如果内层循环找到了inode，外层循环也退出
                        while (offsize1 < dir_blk1->size && !isFindInode)
                        {
                            //依次读出dir_blk1中的fd
                            int check = read_block_by_no(dir_blk1, DATA_BLOCK_START_NUM + *p1);
                            if (check == -1)
                                return -1;
                            //下面的操作和直接寻址一样
                            tfd = (struct file_directory)blk->data;
                            int offsize = 0; //记录当前的偏移量
                            //接下来对整个数据块内装的的目录或文件进行查找
                            //利用while循环，每次都偏移一个fd的大小
                            //此处可能出bug
                            while (offsize < blk->size)
                            { //条件为
                                //当前fd的名字fname和curlevel相等
                                //并且文件后缀名也相等（文件）或者无后缀名的分割符'.'
                                if (strcmp(tfd->fname, cur_level) == 0 && (next_ext == '\0' || strcmp(tfd->fext, next_ext) == 0))
                                {
                                    printf("已找到当前目录或文件:%s\n\n", cur_level);
                                    //根据fd来获取当前目录文件对应的inode
                                    read_inode_by_no(tinode, tfd->st_ino);
                                    //设置isFindInode为true
                                    isFindInode = true;
                                    if (flag) //已经到最后一级路径
                                    {
                                        //将该fd的信息读取到返回结果中
                                        strcpy(fd->fname, tfd->fname);
                                        strcpy(fd->fext, tfd->fext);
                                        strcpy(fd->standby, tfd->standby);
                                        fd->st_ino = tfd->st_ino;
                                        //根据fd的inode号来读取对应的inode信息
                                        read_inode_by_no(ind, tfd->st_ino);
                                        printf("找到对应的inode：inode号为:%d\n", tfd->st_ino);
                                        return 0;
                                    }
                                    //已经找到对应的inode，退出循环
                                    break;
                                }
                                //该fd和当前目录的name不同
                                offsize += sizeof(struct file_directory);
                                //指针偏移fd字节继续查找
                                tfd++;
                            }
                            //内层循环没找到对应的目录文件
                            //偏移外层dir数据块的指针，对下一个dir指向的数据块进行查找
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
        //进入下级目录
        //继续执行新一轮的目录拆解寻找
        if (tmp_path != NULL)
        {
            tmp_path++;
        }
    }
    //进入了for循环查询
    //但是没有找到对应文件
    printf("找不到:%s 路径下的文件！\n", path);
    free(blk);
    free(tinode);
    return -1; //未找到文件！
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