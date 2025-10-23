import os


def make_large_dir():
    os.system("rm disk.img")
    os.system("mkdir -p vdisk")
    os.system("dd if=/dev/zero of=disk.img bs=128M count=1")
    os.system("chmod 777 disk.img")
    os.system("mkfs.msdos -F 32 disk.img")
    os.system("mount disk.img vdisk")
    
    num_of_files = 12
    for i in range(num_of_files):
        os.system(f"echo '{i}abc' > vdisk/{i}.txt")


def make_large_file():
    os.system("rm disk.img")
    os.system("dd if=/dev/zero of=disk.img bs=128M count=1")
    os.system("mkfs.msdos -F 32 disk.img")
    os.system("mkdir vdisk4")
    os.system("mount disk.img vdisk4")
    os.system("chmod 777 disk.img")
    # 用随机数据填充一个 515 字节的文件 a.bin
    os.system("dd if=/dev/random of=vdisk4/a.bin bs=515 count=1")
    os.system("umount vdisk4")
    
    
if __name__ == "__main__":
    make_large_file()
    
