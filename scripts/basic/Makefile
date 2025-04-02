###
# Makefile.basic list the most basic programs used during the build process.
# The programs listed herein is what is needed to do the basic stuff,
# such as splitting .config and fix dependency file.
# This initial step is needed to avoid files to be recompiled
# when kernel configuration changes (which is what happens when
# .config is included by main Makefile.
# ---------------------------------------------------------------------------
# fixdep: 	 Used to generate dependency information during build process
# 		 构建过程中生成依赖文件
# split-include: Divide all config symbols up in a number of files in
#                include/config/...
#                将配置文件分割成多个文件，并保存到include/config/.. 中
# docproc:	 Used in Documentation/docbook
# 		 Documentation/docbook 中使用

hostprogs-y	:= fixdep split-include docproc
always		:= $(hostprogs-y)

# fixdep is needed to compile other host programs
# 编译其他本地文件需要fixdep程序，增加依赖关系，首先编译fixdep程序
$(addprefix $(obj)/,$(filter-out fixdep,$(always))): $(obj)/fixdep
