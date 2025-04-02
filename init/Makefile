#
# Makefile for the linux kernel.
#

obj-y				:= main.o version.o mounts.o initramfs.o
obj-$(CONFIG_GENERIC_CALIBRATE_DELAY) += calibrate.o

mounts-y			:= do_mounts.o
mounts-$(CONFIG_DEVFS_FS)	+= do_mounts_devfs.o
mounts-$(CONFIG_BLK_DEV_RAM)	+= do_mounts_rd.o
mounts-$(CONFIG_BLK_DEV_INITRD)	+= do_mounts_initrd.o
mounts-$(CONFIG_BLK_DEV_MD)	+= do_mounts_md.o

# files to be removed upon make clean
# 删除文件的时候，默认是当前路径，如果不是当前路径，需要使用相对路径or绝对路径
# 此处，代码在不同用户可能放到不同的路径下，所以需要使用相对路径.
clean-files := ../include/linux/compile.h

# dependencies on generated files need to be listed explicitly
# 需要列明依赖文件

$(obj)/version.o: include/linux/compile.h

# compile.h changes depending on hostname, generation number, etc,
# so we regenerate it always.
# mkcompile_h will make sure to only update the
# actual file if its content has changed.
# compile.h 文件会随着hostname 而变化，所以每次都需要重新更新.
# mkcompile_h 确保每次内容变化才会更新.

include/linux/compile.h: FORCE
	@echo '  CHK     $@'
	@$(CONFIG_SHELL) $(srctree)/scripts/mkcompile_h $@ "$(UTS_MACHINE)" "$(CONFIG_SMP)" "$(CC) $(CFLAGS)"
