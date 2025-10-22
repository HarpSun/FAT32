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


if __name__ == "__main__":
    make_large_dir()
    
