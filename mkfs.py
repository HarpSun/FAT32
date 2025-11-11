"""
generate disk.img for test
"""
import os
import sys


def clean():
    os.system("umount vdisk")
    os.system("rm -rf vdisk")
    os.system("rm disk.img")

    
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
    os.system("mkdir vdisk")
    os.system("mount disk.img vdisk")
    os.system("chmod 777 disk.img")
    # 用随机数据填充一个 515 字节的文件 a.bin
    os.system("dd if=/dev/random of=vdisk/a.bin bs=515 count=1")
    os.system("umount vdisk")


def make_deep_dir():
    os.system("rm disk.img")
    os.system("dd if=/dev/zero of=disk.img bs=128M count=1")
    os.system("chmod 777 disk.img")
    os.system("mkfs.msdos -F 32 disk.img")
    os.system("mkdir vdisk")
    os.system("mount disk.img vdisk")
    os.system("mkdir -p vdisk/a/b")
    os.system("echo 'a1' > vdisk/a/a1.txt")
    os.system("echo 'b1' > vdisk/a/b/b1.txt")
    os.system("umount vdisk")


def make_dfs_img():
    s = """00000000: 6131 2e74 7874 0000 0300 0000 0000 0000  a1.txt..........
00000010: 3131 3100 0000 0000 0000 0000 0000 0000  111.............
00000020: 6132 2e74 7874 0000 0300 0000 0000 0000  a2.txt..........
00000030: 3232 3200 0000 0000 0000 0000 0000 0000  222.............
00000040: 6133 2e74 7874 0000 0300 0000 0000 0000  a3.txt..........
00000050: 3333 3300 0000 0000 0000 0000 0000 0000  333.............
00000060: 6134 2e74 7874 0000 0300 0000 0000 0000  a4.txt..........
00000070: 3434 3400 0000 0000 0000 0000 0000 0000  444.............
00000080: 6135 2e74 7874 0000 0300 0000 0000 0000  a5.txt..........
00000090: 3535 3500 0000 0000 0000 0000 0000 0000  555.............
000000a0: 6136 2e74 7874 0000 0300 0000 0000 0000  a6.txt..........
000000b0: 3636 3600 0000 0000 0000 0000 0000 0000  666.............
000000c0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
000000d0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
000000e0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
000000f0: 0000 0000 0000 0000 0000 0000 0000 0000  ................"""

    data = bytearray()
    lines = s.split('\n')
    for line in lines:
        tokens = line.split()[1:9]
        print(tokens)
        for t in tokens:
            b1 = int(t[:2], 16)
            b2 = int(t[2:], 16)
            data.append(b1)
            data.append(b2)
     
    with open("dfs.img", "wb") as f:
        f.write(data)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        command = sys.argv[1]
        if command == "large_file":
            make_large_file()
        elif command == "large_dir":
            make_large_dir()
        elif command == "deep_dir":
            make_deep_dir()
        elif command == "dfs_img":
            make_dfs_img()
        elif command == "clean":
            clean()
        else:
            print("unknown command!\nAvailable command: large_file, large_dir, dfs_img...")
    else:
        make_large_file()
        
    # make_large_file()
    # make_dfs_img()
    
