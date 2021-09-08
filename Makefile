TOPDIR := $(shell cd ../..;pwd)
include $(TOPDIR)/build/base.mk

LIBS := -rdynamic -lcommon -lrt -ldl -lpthread -lm 
OBJS := lib_md5.o lib_aes.o miio_proto.o cloud.o
TARGET := cloud

include $(BUILD_APP)
