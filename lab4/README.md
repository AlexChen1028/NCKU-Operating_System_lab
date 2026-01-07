Commands below
Run Bonus

make
ls
sudo insmod osfs.ko
sudo mount -t osfs none mnt_bonus/
sudo touch test1.txt
sudo bash –c "echo 'I LOVE OSLAB' > test1.txt"
cat test1.txt


sudo dd if=/dev/zero of=mnt_bonus/file_20k bs=2K count=10
ls -l mnt_bonus/file_20k