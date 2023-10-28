# sampleFileSystem 使用教程 (Github:Wangccchen)

## 1. 基于libfuse开发
本UFS是基于libfuse3，在ubuntu 20.04上进行开发，libfuse具体的地址与使用说明参考 
 (https://github.com/libfuse/libfuse.git)  
 首先要安装libfuse
 <pre>sudo apt-get install libfuse3-dev</pre>  
 安装相关软件包
 <pre>sudo apt-get install git gcc vim lrzsz openssh-server meson pkg-config make unity-tweak-tool libtool m4 autoconf pkg-config meson</pre>


## 2. 使用方法
1. 首先将两个.c文件和makefile放入一个目录中
2. 初始化磁盘  
<pre>dd bs=512 count=16384 if=/dev/zero of=disk.img</pre>  
其中 disk.img 是要创建的磁盘文件的名字
3. 修改两个.c文件中的此行代码，将路径改为对应的磁盘文件的路径  
<pre>const char *disk_path = "/home/wc/桌面/SFS/disk.img";</pre>
4. 执行 make 命令编译代码  
<pre>make</pre> 
5. 执行初始化磁盘的文件  
<pre>./init_disk</pre>
6. 创建要挂载的文件系统所在目录并挂载  
<pre>
mkdir testmount
./UFS -d testmount
</pre>  
7.执行文件系统的相关命令  
8.卸载该文件系统
<pre>
fusermount -u testmount
</pre>

## 3. 支持的命令
<pre>mkdir
rmdir
echo
touch
cat
unlink
ls -al
ls
cd